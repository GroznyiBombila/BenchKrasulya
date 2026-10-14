# BenchKrasulya — набор тестов перед выпуском версии.
#   powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1            все тесты (~3 мин)
#   ... -SkipReal      без замеров на этом ПК (только сборка, статика, эмуляция)
#   ... -Full          плюс полные замеры под эмуляцией нескольких профилей (~5 мин)
#   ... -Update        перезаписать эталоны tests\expected\*.txt (после проверки глазами!)
# Код выхода 0 — всё прошло. Результаты прогонов: build\testrun\
param([switch]$SkipReal, [switch]$Full, [switch]$Update)

$ErrorActionPreference = 'Continue'  # stderr gcc/objdump в PS 5.1 иначе становится ошибкой
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$env:PATH = "C:\mingw64\bin;$env:PATH"
$out = Join-Path $root 'build\testrun'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory $out -Force | Out-Null
$exe = Join-Path $root 'BenchKrasulya.exe'
$emu = Join-Path $root 'tests\BenchKrasulya_emu.exe'
$utf8 = New-Object Text.UTF8Encoding $false

$script:fails = @()
$script:passed = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:passed++; Write-Host "  OK    $name" -ForegroundColor Green }
    else { $script:fails += "$name $detail"; Write-Host "  FAIL  $name $detail" -ForegroundColor Red }
}
function Section([string]$t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }

# запуск GUI-программы без окна ожидания; возвращает код выхода
function RunExe([string]$path, [string[]]$a, [int]$timeoutSec = 600) {
    $p = Start-Process $path -ArgumentList $a -PassThru
    if (-not $p.WaitForExit($timeoutSec * 1000)) { $p.Kill(); return -1 }
    return $p.ExitCode
}
function Num([string]$s) { return [double]::Parse($s.Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture) }

# строки CSV одной секции [Имя] (без заголовков)
function CsvSection([string[]]$csv, [string]$name) {
    $rows = @(); $in = $false
    foreach ($l in $csv) {
        if ($l -match '^\[') { $in = $l.StartsWith("[$name]"); continue }
        if ($in -and $l -match '^\d') { $rows += , ($l -split ';') }
    }
    return , $rows
}

# ---------------------------------------------------------------- сборка
Section 'Сборка'
# полный путь: при длинном текущем каталоге cmd /c не находит bat по относительному имени
$b1 = & "$root\build.bat" 2>&1
Check 'build.bat' ($LASTEXITCODE -eq 0) ($b1 | Select-Object -Last 3 | Out-String)
$b2 = & "$root\tests\build_emu.bat" 2>&1
Check 'tests\build_emu.bat' ($LASTEXITCODE -eq 0) ($b2 | Select-Object -Last 3 | Out-String)
$job = Join-Path $root 'build\joblimit.exe'
gcc -O1 -municode tests\joblimit.c -o $job 2>&1 | Out-Null
Check 'tests\joblimit.c' ($LASTEXITCODE -eq 0)
if ($script:fails.Count) { Write-Host 'Сборка не прошла — дальше не идём' -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- версия
Section 'Версия'
$vh = Get-Content res\version.h -Raw
$maj = [int]([regex]::Match($vh, 'BK_VER_MAJOR (\d+)').Groups[1].Value)
$min = [int]([regex]::Match($vh, 'BK_VER_MINOR (\d+)').Groups[1].Value)
$pat = [int]([regex]::Match($vh, 'BK_VER_PATCH (\d+)').Groups[1].Value)
$vstr = [regex]::Match($vh, 'BK_VER_STR "([^"]+)"').Groups[1].Value
Write-Host "  res\version.h: $maj.$min.$pat «$vstr»"
$fv = (Get-Item $exe).VersionInfo
Check 'FileVersion в exe' ($fv.FileMajorPart -eq $maj -and $fv.FileMinorPart -eq $min -and $fv.FileBuildPart -eq $pat) "($($fv.FileVersion))"
Check 'ProductVersion в exe' ($fv.ProductVersion -eq $vstr) "($($fv.ProductVersion))"
Check 'app.manifest' ((Get-Content res\app.manifest -Raw) -match "version=`"$maj\.$min\.$pat\.0`"")
$readme = [IO.File]::ReadAllText("$root\README.md", $utf8)
Check 'заголовок README' ($readme -match "^# BenchKrasulya $([regex]::Escape($vstr))")
$cl = [IO.File]::ReadAllText("$root\CHANGELOG.md", $utf8)
Check 'запись в CHANGELOG.md' ($cl -match "## $([regex]::Escape($vstr))")

# ---------------------------------------------------------------- статические проверки
Section 'Статические проверки exe'
$pe = objdump -p $exe
$ss = [regex]::Match(($pe -join "`n"), 'MajorSubsystemVersion\s+(\d+)[\s\S]*?MinorSubsystemVersion\s+(\d+)')
$ssv = [int]$ss.Groups[1].Value * 10 + [int]$ss.Groups[2].Value
Check 'версия подсистемы PE ≤ 6.1 (Windows 7)' ($ssv -le 61) "($($ss.Groups[1].Value).$($ss.Groups[2].Value))"
$dlls = $pe | Select-String 'DLL Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value.ToLower() }
$okDll = 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'comctl32.dll', 'gdiplus.dll', 'comdlg32.dll', 'ole32.dll', 'shell32.dll', 'powrprof.dll'
$badDll = @($dlls | Where-Object { $_ -notin $okDll -and $_ -notlike 'api-ms-win-crt-*' })
Check 'только системные DLL Windows 7' ($badDll.Count -eq 0) ($badDll -join ', ')
# функции, которых нет в Windows 7 SP1 (их можно брать только через GetProcAddress)
$newer = 'SetThreadInformation', 'SetProcessInformation', 'GetSystemCpuSetInformation', 'SetThreadSelectedCpuSets',
         'GetCurrentThreadStackLimits', 'GetSystemTimePreciseAsFileTime', 'CreateFile2', 'CopyFile2', 'WaitOnAddress',
         'WakeByAddressSingle', 'WakeByAddressAll', 'SetThreadDescription', 'GetThreadDescription', 'PrefetchVirtualMemory',
         'DiscardVirtualMemory', 'OfferVirtualMemory', 'VirtualAlloc2', 'MapViewOfFile3', 'GetProcessMitigationPolicy',
         'SetProcessMitigationPolicy', 'GetFirmwareType', 'SetDefaultDllDirectories', 'AddDllDirectory', 'GetDpiForWindow',
         'GetDpiForSystem', 'SetProcessDpiAwarenessContext', 'SetThreadDpiAwarenessContext', 'AdjustWindowRectExForDpi',
         'GetSystemMetricsForDpi', 'EnableNonClientDpiScaling', 'GetProcessorSystemCycleTime', 'QueryUnbiasedInterruptTimePrecise'
$imports = $pe | Select-String '^\s+[0-9a-f]{8}\s+\S+\s+[0-9a-f]{4}\s+(\w+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
$hit = @($imports | Where-Object { $_ -in $newer })
Check 'нет функций новее Windows 7 в импорте' ($hit.Count -eq 0) ($hit -join ', ')
# общий код — только SSE2: AVX/AVX-512 допустимы лишь в kernels_avx*.o
foreach ($o in 'sysinfo', 'bench', 'chart', 'main', 'kernels_sse2') {
    $d = objdump -d --no-show-raw-insn "build\$o.o"
    $vex = @($d | Select-String "`t(v[a-z0-9]+|k(mov|or|and)[a-z]*)\s" | Where-Object { $_.Line -notmatch "`tverr|`tverw" })
    Check "build\$o.o без AVX-команд" ($vex.Count -eq 0) ("($($vex.Count): " + (($vex | Select-Object -First 2 | ForEach-Object { $_.Line.Trim() }) -join '; ') + ')')
}
$xg = objdump -d --no-show-raw-insn build\sysinfo.o | Select-String "`txgetbv"
Check 'xgetbv есть (проверка поддержки AVX ОС)' ($xg.Count -ge 1)

# ---------------------------------------------------------------- эмуляция: определение железа
Section 'Эмуляция: определение железа (сравнение с tests\expected)'
$listFile = Join-Path $out 'profiles.txt'
$ec = RunExe $emu @('--emu-list', "`"$listFile`"") 60
$profiles = @([IO.File]::ReadAllLines($listFile, $utf8) | Where-Object { $_ -match "^\S+`t" } | ForEach-Object { ($_ -split "`t")[0] })
Check "список профилей ($($profiles.Count))" ($ec -eq 0 -and $profiles.Count -ge 17)
$expDir = Join-Path $root 'tests\expected'
New-Item -ItemType Directory $expDir -Force | Out-Null

# из system.txt — только то, что зависит от профиля (без частот, свободной памяти и среды этого ПК)
function NormalizeReport([string]$path) {
    $res = @(); $in = $false
    foreach ($l in [IO.File]::ReadAllLines($path, $utf8)) {
        if ($l -eq 'ПРОЦЕССОР') { $in = $true }
        if ($l -eq 'СРЕДА') { break }
        if (-not $in) { continue }
        if ($l -match '^\s+(TSC|Объём \(ОС\)):' -or $l -match '\* частота измерена') { continue }
        $res += ($l -replace '\s+\d+\.\d+ ГГц\s*$', '').TrimEnd()
    }
    return $res
}
foreach ($p in $profiles) {
    $dir = Join-Path $out "emu\$p"
    $ec = RunExe $emu @('--emu', $p, '--auto', "`"$dir`"", '--tests', 'x') 120
    $sys = Join-Path $dir 'system.txt'
    if ($ec -ne 0 -or -not (Test-Path $sys)) { Check "профиль $p" $false "(код $ec, system.txt нет)"; continue }
    $norm = NormalizeReport $sys
    $exp = Join-Path $expDir "$p.txt"
    if ($Update -or -not (Test-Path $exp)) {
        [IO.File]::WriteAllLines($exp, [string[]]$norm, $utf8)
        Write-Host "  NEW   эталон $p записан — проверьте его глазами" -ForegroundColor Yellow
        continue
    }
    $want = [IO.File]::ReadAllLines($exp, $utf8)
    $diff = Compare-Object $want $norm
    Check "профиль $p" ($null -eq $diff) ("`n" + (($diff | Select-Object -First 6 | ForEach-Object { "      $($_.SideIndicator) $($_.InputObject)" }) -join "`n"))
}

# ---------------------------------------------------------------- фаззинг SMBIOS
Section 'Фаззинг таблиц SMBIOS'
$fz = Join-Path $out 'fuzz.txt'
$ec = RunExe $emu @('--emu-fuzz', '20000', "`"$fz`"") 300
Check '20000 испорченных таблиц без падения' ($ec -eq 0 -and (Test-Path $fz)) "(код $ec)"

# ---------------------------------------------------------------- проверка правдоподобия замеров
function CheckRun([string]$dir, [string]$tag, [bool]$strict) {
    $csvPath = Join-Path $dir 'results.csv'
    if (-not (Test-Path $csvPath)) { Check "$tag results.csv" $false; return }
    $csv = [IO.File]::ReadAllLines($csvPath, $utf8)
    $sys = [IO.File]::ReadAllText((Join-Path $dir 'system.txt'), $utf8)
    $peak = 0
    $m = [regex]::Match($sys, 'Теоретический пик:.*= ([\d.]+) ГБ/с')
    if ($m.Success) { $peak = Num $m.Groups[1].Value }

    $cache = CsvSection $csv 'Кэш'
    if ($cache.Count) {
        $rd = $cache | ForEach-Object { Num $_[1] }
        Check "$tag кэш: скорости 0.5–3000 ГБ/с" (@($rd | Where-Object { $_ -lt 0.5 -or $_ -gt 3000 }).Count -eq 0)
        Check "$tag кэш: L1 быстрее ОЗУ в ≥ 2 раза" ($rd[0] -gt 2 * $rd[-1]) "($([math]::Round($rd[0],1)) / $([math]::Round($rd[-1],1)))"
    }
    $lat = CsvSection $csv 'Задержка'
    if ($lat.Count) {
        $ns = $lat | ForEach-Object { Num $_[1] }
        Check "$tag задержка: L1 0.2–5 нс" ($ns[0] -gt 0.2 -and $ns[0] -lt 5) "($([math]::Round($ns[0],2)))"
        Check "$tag задержка: ОЗУ 30–1000 нс" ($ns[-1] -gt 30 -and $ns[-1] -lt 1000) "($([math]::Round($ns[-1],1)))"
    }
    foreach ($s in 'Чтение RAM', 'Запись RAM') {
        $j = CsvSection $csv $s
        if ($j.Count) {
            $v = $j | ForEach-Object { $_[1..($_.Count - 1)] } | Where-Object { $_ -ne '' } | ForEach-Object { Num $_ }
            Check "$tag $s`: 0.1–500 ГБ/с" (@($v | Where-Object { $_ -lt 0.1 -or $_ -gt 500 }).Count -eq 0)
        }
    }
    Check "$tag в results.csv есть замеры" (($cache.Count + $lat.Count) -gt 0)
    $mt = CsvSection $csv 'Многопоток'
    if ($mt.Count) {
        $rd = $mt | Where-Object { $_[0] -match '^\d+$' } | ForEach-Object { Num $_[1] }
        Check "$tag многопоток: все точки > 0" (@($rd | Where-Object { -not ($_ -gt 0) }).Count -eq 0)
        if ($strict -and $peak -gt 0) {
            $mx = ($rd | Measure-Object -Maximum).Maximum
            Check "$tag многопоток: чтение ≤ 105% теор. пика" ($mx -le 1.05 * $peak) "($([math]::Round($mx,1)) из $peak ГБ/с)"
        }
    }
}

if (-not $SkipReal) {
    Section 'Замеры на этом ПК'
    $dir = Join-Path $out 'real'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $ec = RunExe $exe @('--auto', "`"$dir`"", '--tests', 'clrwm') 900
    $files = @(Get-ChildItem $dir -File -ErrorAction SilentlyContinue).Count
    Check "полный прогон ($([int]$sw.Elapsed.TotalSeconds) с)" ($ec -eq 0 -and $files -eq 8) "(код $ec, файлов $files)"
    CheckRun $dir 'ПК:' $true

    Section 'Лимит памяти процесса (job object)'
    foreach ($mb in 512, 120) {
        $dir = Join-Path $out "job$mb"
        $r = & $job $mb "`"$exe`" --auto `"$dir`" --tests clrwm"
        $files = @(Get-ChildItem $dir -File -ErrorAction SilentlyContinue).Count
        Check "лимит $mb МБ: без падения" ($LASTEXITCODE -eq 0 -and $files -eq 8) "($r, файлов $files)"
        if ($files) { CheckRun $dir "лимит $mb МБ:" $true }
    }
}

if ($Full) {
    Section 'Полные замеры под эмуляцией'
    foreach ($p in 'core2-ddr2', 'athlon64-ddr', 'glpi-fail', 'ultra7-155h', 'threadripper-7980x') {
        $dir = Join-Path $out "emufull\$p"
        $ec = RunExe $emu @('--emu', $p, '--auto', "`"$dir`"", '--tests', 'clrwm') 900
        $files = @(Get-ChildItem $dir -File -ErrorAction SilentlyContinue).Count
        Check "$p`: прогон" ($ec -eq 0 -and $files -eq 8) "(код $ec, файлов $files)"
        # пик эмулированной памяти не связан с этим ПК — проверяем без сравнения с пиком
        if ($files) { CheckRun $dir "$p`:" $false }
    }
}

Write-Host ""
if ($script:fails.Count) {
    Write-Host "ИТОГ: пройдено $($script:passed), ОШИБОК $($script:fails.Count)" -ForegroundColor Red
    $script:fails | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host "ИТОГ: все $($script:passed) проверок пройдены ($vstr)" -ForegroundColor Green
exit 0
