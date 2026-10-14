// BenchKrasulya
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#ifdef BK_EMU
#include "../tests/emu.h"
#endif
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "bench.h"
#include "chart.h"
#include "theme.h"

#define APP_NAME L"BenchKrasulya"
#include "../res/version.h"
#define APP_VER L"" BK_VER_STR

enum {
    IDC_TAB = 100, IDC_CPU, IDC_ISA, IDC_AREA, IDC_LARGE, IDC_LOGY, IDC_FILTER,
    IDC_START, IDC_ALL, IDC_STOP, IDC_PNG, IDC_CSV, IDC_STATUS, IDC_EDIT, IDC_VIEW,
    IDC_TAB0 = 200  // кнопки вкладок: IDC_TAB0 + номер вкладки
};
#define WM_APP_READY (WM_APP + 1)
#define WM_APP_TESTSTART (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)
#define TIMER_UI 1

enum { TAB_CACHE = 0, TAB_LAT, TAB_READ, TAB_WRITE, TAB_MT, TAB_SYS, TAB_ABOUT, TAB_COUNT };
static const wchar_t* kTabNames[TAB_COUNT] = {L"Кэш", L"Задержка", L"Чтение RAM", L"Запись RAM", L"Многопоток", L"Система", L"О программе"};
static const COLORREF kBlockCol[4] = {RGB(70, 150, 255), RGB(0, 230, 230), RGB(120, 255, 120), RGB(255, 240, 0)};

static HINSTANCE g_hi;
static HWND g_hwnd, g_view, g_edit, g_hot;  // g_hot — элемент под курсором
static HWND g_tabBtn[7];
static HWND g_cbCpu, g_cbIsa, g_cbArea, g_chkLarge, g_chkLogY, g_chkFilter;
static HWND g_btnStart, g_btnAll, g_btnStop, g_btnPng, g_btnCsv;
static HWND g_lblCpu, g_lblIsa, g_lblArea;
static HBRUSH g_brBg, g_brPanel, g_brEdit;
static std::wstring g_statusText;
static int g_progPos = 0;
static HFONT g_fUI, g_fUIBold, g_fChart, g_fChartBold, g_fTable, g_fTableBold, g_fMono;
static int g_dpi = 96;
static int g_tabSel = 0;
static bool g_ready = false;
static std::vector<int> g_coreNo;  // номер ядра внутри кластера
static std::vector<Isa> g_isaMap;
static std::vector<TestKind> g_queue;
static BenchConfig g_runCfg;
static HANDLE g_worker;
static double g_runStart;
static POINT g_hover = {-1, -1};
static DWORD_PTR g_guiAffinity = 0;
static std::wstring g_autoDir;  // --auto <папка>: прогнать все тесты, сохранить PNG/CSV и выйти
static void MakeDirs(const std::wstring& p) {  // создать папку вместе с недостающими родителями
    for (size_t i = 0; (i = p.find_first_of(L"\\/", i + 1)) != std::wstring::npos;) CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(p.c_str(), nullptr);
}
static int g_autoCpu = -1;      // --cpu N
static bool g_autoCompat = false;  // --compat: область 4 МБ
static std::wstring g_autoTests;   // --tests clrwm (c — кэш, l — задержка, r/w — чтение/запись RAM, m — многопоток)

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static bool Checked(HWND h) { return GetWindowLongPtrW(h, GWLP_USERDATA) != 0; }  // галочки рисуются сами, состояние — здесь

static double NowSec()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / f.QuadPart;
}

static std::wstring Pad(const std::wstring& s, size_t n)
{
    return s.size() >= n ? s + L" " : s + std::wstring(n - s.size(), L' ');
}

// ================================================================ данные для вкладок

// «DDR4-3200»; без SMBIOS или без частоты — что известно
static std::wstring MemLabel()
{
    std::wstring t = g_mem.typeName.empty() || g_mem.typeName == L"неизвестно" || g_mem.typeName == L"другое" ? L"ОЗУ" : g_mem.typeName;
    return g_mem.speedMT ? Fmt(L"%ls-%u", t.c_str(), g_mem.speedMT) : t;
}

static std::wstring CpuName(int cpu)
{
    if (cpu < 0 || cpu >= (int)g_cpu.cpus.size()) return L"?";
    const LogicalCpu& c = g_cpu.cpus[cpu];
    std::wstring n = c.cluster >= 0 ? g_cpu.clusters[c.cluster].name : L"Ядро";
    return Fmt(L"%ls #%d%ls (CPU %d)", n.c_str(), g_coreNo[cpu], c.threadInCore ? L", HT-поток" : L"", cpu);
}

static std::wstring Header(int cpu, double ghz, const std::wstring& extra)
{
    std::wstring h = g_cpu.brand + L"  ·  " + CpuName(cpu);
    if (ghz > 0) h += Fmt(L"  ·  %.2f ГГц", ghz);
    if (!extra.empty()) h += L"  ·  " + extra;
    return h;
}

struct Level {
    std::wstring name;
    uint64_t size;
    double lo, hi;
};

static std::vector<Level> Levels(int cpu)
{
    std::vector<Level> v;
    if (cpu < 0 || cpu >= (int)g_cpu.cpus.size()) return v;
    const LogicalCpu& c = g_cpu.cpus[cpu];
    double l1 = c.l1d.size ? (double)c.l1d.size : 32768.0;
    double l2 = (double)c.l2.size, l3 = (double)c.l3.size;
    v.push_back({L"L1D", (uint64_t)l1, 0, l1 / 2});
    if (l2 > 0) v.push_back({L"L2", (uint64_t)l2, l1 * 2, l2 / 2});
    if (l3 > 0) v.push_back({L"L3", (uint64_t)l3, (l2 > 0 ? l2 : l1) * 2, l3 / 2});
    double last = l3 > 0 ? std::max(l3, (double)g_cpu.maxL3) : (l2 > 0 ? l2 : l1);
    v.push_back({L"ОЗУ", 0, last * 4, 1e30});
    return v;
}

static double Median(const std::vector<double>& xs, const std::vector<double>& ys, double lo, double hi)
{
    std::vector<double> t;
    for (size_t i = 0; i < xs.size() && i < ys.size(); i++)
        if (xs[i] >= lo && xs[i] <= hi && std::isfinite(ys[i])) t.push_back(ys[i]);
    if (t.empty()) return NAN;
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

static int SelectedCpu()
{
    int i = (int)SendMessageW(g_cbCpu, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : i;
}

struct JumpStats {
    double mn, mx, rnd, lin, eff;
};

static JumpStats Stats(const JumpResult& r, const std::vector<double>& y, double rnd)
{
    JumpStats s = {INFINITY, -INFINITY, rnd, NAN, NAN};
    for (size_t i = 0; i < y.size(); i++) {
        if (!std::isfinite(y[i])) continue;
        s.mn = std::min(s.mn, y[i]);
        s.mx = std::max(s.mx, y[i]);
        if (std::lround(r.jumps[i]) == 0) s.lin = y[i];
    }
    if (!std::isfinite(s.mn)) s.mn = s.mx = NAN;
    if (std::isfinite(s.lin) && std::isfinite(rnd) && s.lin > 0) s.eff = rnd / s.lin;
    return s;
}

// «Результат» — средневзвешенная скорость случайных блоков,
// вес 32 Б наибольший, каждый следующий размер в 1.5 раза меньше
static double JumpScore(const double* rnd)
{
    double s = 0, w = 1, ws = 0;
    for (int b = 0; b < 4; b++, w /= 1.5) {
        if (!std::isfinite(rnd[b])) return NAN;
        s += rnd[b] * w;
        ws += w;
    }
    return s / ws;
}

static void BuildTab(int tab, Chart& ch, Table& tb, std::vector<std::wstring>& notes)
{
    bool logY = Checked(g_chkLogY);
    bool raw = Checked(g_chkFilter);  // «Сырые точки»: линии по замерам, без полосы разброса
    ch.empty = L"Нажмите «Старт», чтобы запустить тест этой вкладки";
    if (!g_ready || g_cpu.cpus.empty()) {
        ch.empty = L"Определение процессора, кэшей и памяти…";
        return;
    }

    if (tab == TAB_CACHE) {
        const CacheResult& r = g_cache;
        int cpu = r.valid ? r.cpu : SelectedCpu();
        ch.xKind = X_SIZE;
        ch.yLog = logY;
        ch.yUnit = L"ГБ/с";
        ch.xTitle = L"Размер блока данных (буфера)";
        ch.legendTitle = L"Скорость:";
        ch.xmin = 1024;
        ch.xmax = r.valid && !r.sizes.empty() ? r.sizes.back() : 256.0 * 1048576;
        ch.header = Header(cpu, r.valid ? r.ghzBefore : 0, r.valid ? r.isaName : L"");
        if (r.valid) {
            ch.s.push_back({L"чтение", RGB(0, 230, 230), false, r.sizes, r.read});
            ch.s.push_back({L"запись", RGB(255, 240, 0), false, r.sizes, r.write});
            ch.s.push_back({L"запись NT", RGB(255, 140, 60), true, r.sizes, r.writeNT});
            ch.s.push_back({L"код (I-кэш)", RGB(120, 255, 120), false, r.sizes, r.code});
        }
        const LogicalCpu& c = g_cpu.cpus[cpu];
        if (c.l1d.size) ch.vmarks.push_back({(double)c.l1d.size, L"L1D " + FormatSizeAxis((double)c.l1d.size), th::mark});
        if (c.l2.size) ch.vmarks.push_back({(double)c.l2.size, L"L2 " + FormatSizeAxis((double)c.l2.size), th::mark});
        if (c.l3.size) ch.vmarks.push_back({(double)c.l3.size, L"L3 " + FormatSizeAxis((double)c.l3.size), th::mark});

        bool latOk = g_lat.valid && g_lat.cpu == cpu;
        tb.head = {L"Уровень", L"Размер", L"Чтение, ГБ/с", L"Запись, ГБ/с", L"Запись NT", L"Код", L"Задержка, нс", L"Тактов"};
        for (auto& lv : Levels(cpu)) {
            std::vector<std::wstring> row = {lv.name, lv.size ? FormatSize(lv.size) : L"—"};
            if (r.valid) {
                row.push_back(FmtGB(Median(r.sizes, r.read, lv.lo, lv.hi)));
                row.push_back(FmtGB(Median(r.sizes, r.write, lv.lo, lv.hi)));
                row.push_back(FmtGB(Median(r.sizes, r.writeNT, lv.lo, lv.hi)));
                row.push_back(FmtGB(Median(r.sizes, r.code, lv.lo, lv.hi)));
            } else
                for (int i = 0; i < 4; i++) row.push_back(L"—");
            double ns = latOk ? Median(g_lat.sizes, g_lat.ns, lv.lo, lv.hi) : NAN;
            row.push_back(std::isfinite(ns) ? Fmt(L"%.1f", ns) : L"—");
            row.push_back(std::isfinite(ns) ? Fmt(L"%.0f", ns * g_lat.ghz) : L"—");
            tb.rows.push_back(row);
        }
        if (r.valid && std::fabs(r.ghzAfter - r.ghzBefore) > 0.15 * r.ghzBefore)
            notes.push_back(Fmt(L"! Частота ядра менялась во время теста: %.2f → %.2f ГГц (нагрев или лимит мощности).", r.ghzBefore, r.ghzAfter));
    } else if (tab == TAB_LAT) {
        const LatencyResult& r = g_lat;
        int cpu = r.valid ? r.cpu : SelectedCpu();
        ch.xKind = X_SIZE;
        ch.yLog = true;
        ch.yUnit = L"нс";
        ch.xTitle = L"Размер блока (случайный обход цепочки указателей)";
        ch.legendTitle = L"Задержка чтения:";
        ch.xmin = 2048;
        ch.xmax = r.valid && !r.sizes.empty() ? r.sizes.back() : 256.0 * 1048576;
        ch.header = Header(cpu, r.ghz, r.valid && r.largePages ? L"страницы 2 МБ" : L"");
        if (r.valid) ch.s.push_back({L"нс на обращение", RGB(255, 190, 0), false, r.sizes, r.ns});
        const LogicalCpu& c = g_cpu.cpus[cpu];
        if (c.l1d.size) ch.vmarks.push_back({(double)c.l1d.size, L"L1D " + FormatSizeAxis((double)c.l1d.size), th::mark});
        if (c.l2.size) ch.vmarks.push_back({(double)c.l2.size, L"L2 " + FormatSizeAxis((double)c.l2.size), th::mark});
        if (c.l3.size) ch.vmarks.push_back({(double)c.l3.size, L"L3 " + FormatSizeAxis((double)c.l3.size), th::mark});
        tb.head = {L"Уровень", L"Размер", L"Задержка, нс", L"Тактов ядра"};
        for (auto& lv : Levels(cpu)) {
            double ns = r.valid ? Median(r.sizes, r.ns, lv.lo, lv.hi) : NAN;
            tb.rows.push_back({lv.name, lv.size ? FormatSize(lv.size) : L"—", std::isfinite(ns) ? Fmt(L"%.1f", ns) : L"—",
                               std::isfinite(ns) ? Fmt(L"%.0f", ns * r.ghz) : L"—"});
        }
    } else if (tab == TAB_READ || tab == TAB_WRITE) {
        bool w = tab == TAB_WRITE;
        const JumpResult& r = w ? g_jwrite : g_jread;
        int cpu = r.valid ? r.cpu : SelectedCpu();
        ch.xKind = X_JUMP;
        ch.yLog = false;
        ch.yUnit = L"ГБ/с";
        ch.xTitle = L"Скачок адреса между блоками, байт (< 0 — блоки идут к младшим адресам)";
        ch.legendTitle = w ? L"Запись:" : L"Чтение блока:";
        ch.xmin = -3072;
        ch.xmax = 7168;
        std::wstring area = r.valid ? (r.ramMode ? L"область " + FormatSize(r.span) + L" (ОЗУ)" : L"область 4 МБ (в пределах L3)") : L"";
        ch.header = Header(cpu, r.ghz, area);
        // чтение — один график: линия — среднее по соседним точкам, полоса — разброс;
        // запись — по панели на размер блока, во всю ширину, с сырыми замерами (все колебания видны);
        // в панели две линии цвета блока: NT (в обход кэша) сплошной и обычная (через кэш) пунктиром
        if (w) {
            ch.rows = 4;
            for (int b = 0; b < 4; b++) ch.panelTitle.push_back(Fmt(L"Блок %d Б", kBlockSizes[b]));
        }
        if (r.valid)
            for (int b = 0; b < 4; b++) {
                if (!w) {
                    ch.s.push_back({Fmt(L"%d", kBlockSizes[b]), kBlockCol[b], false, r.jumps, r.y[b], !raw});
                    continue;
                }
                ch.s.push_back({L"в обход кэша (NT)", kBlockCol[b], false, r.jumps, r.y[b], false, b});
                ch.s.push_back({L"через кэш", kBlockCol[b], true, r.jumps, r.yc[b], false, b});
            }
        auto cells = [&](std::vector<std::wstring>& row, const std::vector<double>& y, double rnd) {
            JumpStats s = r.valid ? Stats(r, y, rnd) : JumpStats{NAN, NAN, NAN, NAN, NAN};
            for (double v : {s.mn, s.mx, s.rnd, s.lin}) row.push_back(FmtGB(v));
            row.push_back(std::isfinite(s.eff) ? Fmt(L"%.0f%%", s.eff * 100) : L"—");
        };
        if (w)
            tb.head = {L"Запись блока", L"в обход кэша (NT): мин.", L"макс.", L"случайн.", L"линейн.", L"эффект.",
                       L"через кэш: мин.", L"макс.", L"случайн.", L"линейн.", L"эффект."};
        else
            tb.head = {L"Чтение блока", L"мин.", L"макс.", L"средняя (случ.)", L"линейная", L"эффект."};
        for (int b = 0; b < 4; b++) {
            std::vector<std::wstring> row = {Fmt(L"%d байт", kBlockSizes[b])};
            cells(row, r.y[b], r.random[b]);
            if (w) cells(row, r.yc[b], r.randomC[b]);
            tb.rows.push_back(row);
            tb.swatch.push_back(kBlockCol[b]);
            tb.swatchDash.push_back(false);
        }
        std::vector<std::wstring> res = {L"Результат", L"", L"", r.valid ? FmtGB(JumpScore(r.random)) : L"—", L"", L""};
        if (w) res.insert(res.end(), {L"", L"", r.valid ? FmtGB(JumpScore(r.randomC)) : L"—", L"", L""});
        tb.rows.push_back(res);
        tb.swatch.push_back(CLR_INVALID);
        tb.swatchDash.push_back(false);
        if (r.valid && !r.ramMode) notes.push_back(L"! Режим «4 МБ»: на современных CPU эта область целиком в L3 — это тест кэша, а не ОЗУ.");
    } else if (tab == TAB_MT) {
        const MTResult& r = g_mt;
        ch.xKind = X_COUNT;
        ch.yLog = logY;
        ch.yUnit = L"ГБ/с";
        ch.xTitle = L"Число потоков (сначала по одному на ядро: P → E → LP E, затем HT)";
        ch.legendTitle = L"ОЗУ, все потоки вместе:";
        ch.xmin = 1;
        ch.xmax = std::max<double>(2, r.valid && !r.n.empty() ? r.n.back() : (double)g_cpu.cpus.size());
        ch.header = g_cpu.brand + L"  ·  " + MemLabel();
        if (r.valid) {
            ch.s.push_back({L"чтение", RGB(0, 230, 230), false, r.n, r.read});
            ch.s.push_back({L"запись NT", RGB(255, 240, 0), false, r.n, r.writeNT});
            ch.s.push_back({L"запись", RGB(230, 230, 230), true, r.n, r.write});
            for (size_t i = 1; i < r.order.size(); i++) {
                const LogicalCpu& a = g_cpu.cpus[r.order[i - 1]];
                const LogicalCpu& b = g_cpu.cpus[r.order[i]];
                if (a.cluster != b.cluster || a.threadInCore != b.threadInCore) {
                    std::wstring nm = b.threadInCore ? L"HT" : g_cpu.clusters[b.cluster].shortName;
                    ch.vmarks.push_back({i + 0.5, L"→ " + nm, th::mark});
                }
            }
        }
        if (g_mem.peakGBs > 0)
            ch.hmarks.push_back({g_mem.peakGBs, Fmt(L"теоретический пик %ls: %.1f ГБ/с", MemLabel().c_str(), g_mem.peakGBs), RGB(255, 80, 80)});
        tb.head = {L"Конфигурация", L"Потоков", L"Чтение, ГБ/с", L"Запись NT", L"Запись", L"% пика (чтение)"};
        for (auto& s : r.summary)
            tb.rows.push_back({s.name, Fmt(L"%d", s.threads), FmtGB(s.read), FmtGB(s.writeNT), FmtGB(s.write),
                               g_mem.peakGBs > 0 ? Fmt(L"%.0f%%", s.read / g_mem.peakGBs * 100) : L"—"});
        if (r.valid && !r.read.empty()) {
            double best = 0;
            size_t bi = 0;
            for (size_t i = 0; i < r.read.size(); i++)
                if (std::isfinite(r.read[i]) && r.read[i] > best) best = r.read[i], bi = i;
            if (best > 0)
                tb.rows.push_back({L"Максимум чтения на кривой", Fmt(L"%.0f", r.n[bi]), FmtGB(best), L"", L"",
                                   g_mem.peakGBs > 0 ? Fmt(L"%.0f%%", best / g_mem.peakGBs * 100) : L"—"});
        }
        if (tb.rows.empty()) tb.rows.push_back({L"—", L"", L"", L"", L"", L""});
        if (r.cacheOnly > 0)
            notes.push_back(Fmt(L"! Мало свободной памяти: точки для 1…%d потоков пропущены — их буферы помещались в L3, замер показал бы кэш.", r.cacheOnly));
        if (r.skipped > 0)
            notes.push_back(Fmt(L"! Памяти хватило только на %zu потоков из %zu — остальные CPU в замере не участвуют.", r.order.size(),
                                r.order.size() + r.skipped));
    }
}

static void PaintView(HDC dc, const RECT& rc, POINT hover)
{
    HBRUSH bg = CreateSolidBrush(th::bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    Chart ch;
    Table tb;
    std::vector<std::wstring> notes;
    EnterCriticalSection(&g_bench.cs);
    BuildTab(g_tabSel, ch, tb, notes);
    LeaveCriticalSection(&g_bench.cs);

    ChartFonts f = {g_fChart, g_fChartBold, g_fTable, g_fTableBold, g_dpi};
    int tableH = DrawTable(dc, 0, 0, tb, f, true);
    std::wstring allNotes;
    for (auto& n : notes) allNotes += n + L"\n";
    RECT nr = {rc.left + S(12), 0, rc.right - S(12), 0};
    HGDIOBJ of = SelectObject(dc, g_fUI);
    DrawTextW(dc, allNotes.c_str(), -1, &nr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int notesH = nr.bottom - nr.top;
    int bottomH = tableH + notesH + S(24);
    RECT cr = rc;
    cr.bottom = std::max<LONG>(rc.top + S(240), rc.bottom - bottomH);
    DrawChart(dc, cr, ch, f, hover);
    int y = cr.bottom + S(8);
    y += DrawTable(dc, rc.left + S(10), y, tb, f, false) + S(8);
    nr = {rc.left + S(12), y, rc.right - S(12), rc.bottom};
    SelectObject(dc, g_fUI);
    SetTextColor(dc, th::warn);  // под таблицей — только предупреждения
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, allNotes.c_str(), -1, &nr, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
}

// ================================================================ тексты «Система» и «О программе»

static std::wstring CacheShare(const CacheInfo& c, const LogicalCpu& lc)
{
    if (!c.size) return L"нет";
    std::wstring s = FormatSize(c.size);
    int cores = c.sharedCores ? (int)c.sharedCores : (int)c.sharedBy / std::max(1, lc.threadsInCore);
    if (cores <= 1) return s + L" на ядро";
    if (cores >= g_cpu.physCores) return s + L" общий";
    return s + Fmt(L" на %d яд.", cores);
}

static std::wstring SystemReport()
{
    std::wstring t;
    auto L = [&](const std::wstring& s) { t += s + L"\r\n"; };
    L(APP_NAME L" " APP_VER L" — сведения о системе");
#ifdef BK_EMU
    if (EmuActive()) L(L"!!! " + EmuTitle() + L" (тесты выполняются на этом ПК)");
#endif
    L(L"");
    L(L"ПРОЦЕССОР");
    L(L"  Модель:             " + g_cpu.brand);
    L(Fmt(L"  Производитель:      %ls · семейство %d, модель 0x%X, степпинг %d", g_cpu.vendor.c_str(), g_cpu.family, g_cpu.model, g_cpu.stepping));
    L(Fmt(L"  Ядер / потоков:     %d / %zu", g_cpu.physCores, g_cpu.cpus.size()));
    L(std::wstring(L"  Гибридный:          ") + (g_cpu.hybrid ? L"да (P- и E-ядра, CPUID 07h EDX[15])" : L"нет"));
    std::wstring isa = L"SSE2";
    if (g_cpu.avx) isa += L", AVX";
    if (g_cpu.avx2) isa += L", AVX2";
    isa += g_cpu.avx512 ? L", AVX-512" : L"  (AVX-512 нет)";
    L(L"  Векторные команды:  " + isa);
    L(Fmt(L"  TSC:                %.3f ГГц%ls", g_cpu.tscGHz, g_cpu.invariantTsc ? L" (инвариантный)" : L""));
    L(std::wstring(L"  Гипервизор:         ") +
      (g_cpu.hypervisor ? L"обнаружен — Hyper-V (VBS, «изоляция ядра», WSL2) или виртуальная машина; на скорость влияет слабо" : L"нет"));
    L(Fmt(L"  Всего L3:           %ls", FormatSize(g_cpu.totalL3).c_str()));
    L(L"");
    L(L"КЛАСТЕРЫ ЯДЕР (Windows + CPUID 1Ah)");
    L(L"  " + Pad(L"Тип", 12) + Pad(L"Ядер", 6) + Pad(L"Потоков", 9) + Pad(L"L1D", 9) + Pad(L"L1I", 9) + Pad(L"L2", 19) + Pad(L"L3", 17) + L"Частота*");
    for (auto& k : g_cpu.clusters) {
        const LogicalCpu& c = g_cpu.cpus[k.rep];
        L(L"  " + Pad(k.name, 12) + Pad(Fmt(L"%d", k.cores), 6) + Pad(Fmt(L"%zu", k.cpus.size()), 9) + Pad(FormatSize(c.l1d.size), 9) +
          Pad(FormatSize(c.l1i.size), 9) + Pad(CacheShare(c.l2, c), 19) + Pad(CacheShare(c.l3, c), 17) +
          (k.clockGHz > 0 ? Fmt(L"%.2f ГГц", k.clockGHz) : L"—"));
    }
    L(L"  * частота измерена: 100 зависимых сложений = 100 тактов, делим на время.");
    if (g_cpu.eCacheFromTable)
        L(L"  ! Гипервизор скрывает настоящие кэши E-ядер (Windows видит у них кэши P-ядра) — размеры взяты из справочника Intel.");
    if (g_cpu.topologyUnknown)
        L(L"  ! Windows не сообщила топологию: каждый поток считается отдельным ядром, размеры кэшей неизвестны (граница L1 принята 32 КБ).");
    L(L"");
    L(L"ЛОГИЧЕСКИЕ ПРОЦЕССОРЫ");
    for (size_t i = 0; i < g_cpu.cpus.size(); i++) L(L"  " + CpuName((int)i));
    L(L"");
    L(L"ПАМЯТЬ");
    L(Fmt(L"  Объём (ОС):         %.1f ГБ, свободно %.1f ГБ", g_mem.totalMB / 1024.0, g_mem.availMB / 1024.0));
    if (g_mem.smbiosOk) {
        L(Fmt(L"  SMBIOS:             %d.%d", g_mem.smbiosMajor, g_mem.smbiosMinor));
        L(L"  " + Pad(L"Слот", 38) + Pad(L"Объём", 8) + Pad(L"Тип", 8) + Pad(L"МТ/с", 7) + Pad(L"Настр.", 8) + Pad(L"Шина", 6) + Pad(L"Ранги", 7) +
          Pad(L"Напр.", 8) + Pad(L"Производитель", 16) + L"Партномер");
        for (auto& m : g_mem.modules) {
            std::wstring slot = m.locator;
            if (!m.bank.empty() && m.bank != m.locator) slot = m.bank + L" / " + m.locator;
            L(L"  " + Pad(slot, 38) + Pad(FormatSize(m.sizeMB << 20), 8) + Pad(m.typeName, 8) + Pad(Fmt(L"%u", m.speed), 7) +
              Pad(Fmt(L"%u", m.confSpeed), 8) + Pad(Fmt(L"%d", m.dataWidth), 6) + Pad(m.ranks ? Fmt(L"%d", m.ranks) : L"—", 7) +
              Pad(m.voltage_mV ? Fmt(L"%.2f В", m.voltage_mV / 1000.0) : L"—", 8) + Pad(m.manufacturer, 16) + m.part);
        }
        for (auto& m : g_mem.modules)
            if (m.rawSpeed) {
                L(Fmt(L"  ! BIOS записал частоту в МГц (%u), а не в МТ/с — пересчитано: %u МТ/с.", m.rawSpeed, m.confSpeed ? m.confSpeed : m.speed));
                break;
            }
        if (g_mem.modules.empty())
            L(L"  В SMBIOS нет записей о модулях памяти — тип и частота неизвестны.");
        else
            L(Fmt(L"  Режим:              %ls, каналов %d, шина %d бит (%ls)", MemLabel().c_str(), g_mem.channels, g_mem.busBits,
                  g_mem.channelNote.c_str()));
        if (g_mem.peakGBs > 0)
            L(Fmt(L"  Теоретический пик:  %u МТ/с × %d Б = %.1f ГБ/с", g_mem.speedMT, g_mem.busBits / 8, g_mem.peakGBs));
        else if (!g_mem.modules.empty())
            L(L"  Теоретический пик:  не рассчитан — BIOS не сообщает частоту памяти.");
        if (g_mem.typeName == L"DDR5")
            L(L"  DDR5: каждый модуль — 2 независимых подканала по 32 бит (всего те же 64 бит), что повышает параллелизм обращений.");
        if (g_mem.typeName == L"LPDDR5" || g_mem.typeName == L"LPDDR4")
            L(L"  LPDDR: память распаяна, каналы по 16 бит; суммарная ширина обычно 128 бит.");
    } else
        L(L"  SMBIOS недоступен (виртуальная машина?) — тип и частота памяти неизвестны.");
    L(L"");
    L(L"СРЕДА");
    L(L"  ОС:                 " + OsVersionString());
    SYSTEM_POWER_STATUS ps = {};
    GetSystemPowerStatus(&ps);
    L(std::wstring(L"  Питание:            ") +
      (ps.ACLineStatus == 0 ? L"ОТ БАТАРЕИ — частоты и лимиты мощности снижены, подключите зарядку!" : ps.ACLineStatus == 1 ? L"от сети" : L"неизвестно"));
    L(std::wstring(L"  Страницы 2 МБ:      ") +
      (LargePagesAvailable() ? L"доступны" : L"недоступны (нужно право «Блокировка страниц в памяти» в secpol.msc и перезаход)"));
    L(L"  Область RAM-тестов: " + FormatSize(RamSpanBytes()) + L" (в ≥4 раза больше всего L3)");
    return t;
}

static std::wstring AboutText()
{
    return APP_NAME L" " APP_VER L"\r\n"
        L"Открытый исходный код: github.com/GroznyiBombila/BenchKrasulya\r\n"
        L"Связь: alexandrkrasulya@gmail.com\r\n"
        L"\r\n"
        L"ОСОБЕННОСТИ\r\n"
        L"  • Размеры L1/L2/L3 берутся из Windows для КАЖДОГО ядра. L3 всегда виден, тест «Кэш» идёт до 8×L3 (до 1 ГБ).\r\n"
        L"  • Гибридные CPU (Alder/Raptor Lake, Core Ultra Meteor/Arrow/Lunar Lake, AMD Zen 5 + Zen 5c): тест закрепляется за\r\n"
        L"    выбранным ядром (P, E или LP E), поток не перескакивает между ядрами, отключено энергосбережение потока (EcoQoS).\r\n"
        L"  • «Чтение/Запись RAM» по умолчанию идут в области в ≥4 раза больше всего L3 с вытеснением кэша перед замером —\r\n"
        L"    измеряется настоящая ОЗУ. Режим «4 МБ» — небольшая область, целиком лежащая в L3.\r\n"
        L"  • Тип и частота памяти (DDR3, DDR4, DDR5, LPDDR4/5) из SMBIOS, теоретический пик пропускной способности.\r\n"
        L"  • SSE2 / AVX / AVX-512 — выбор ширины векторов (влияет на скорость L1).\r\n"
        L"  • Новые тесты: задержка (нс и такты), многопоточная пропускная способность ОЗУ.\r\n"
        L"\r\n"
        L"ВКЛАДКИ\r\n"
        L"  Кэш         Последовательное чтение, запись, запись в обход кэша (NT) и выполнение кода (NOP) для буферов от 2 КБ.\r\n"
        L"              Ступени графика — уровни кэша: L1 → L2 → L3 → ОЗУ.\r\n"
        L"  Задержка    Случайный обход цепочки указателей: сколько наносекунд/тактов ждёт одно обращение на каждом уровне.\r\n"
        L"  Чтение RAM  Блоки 32/64/128/256 Б, между блоками скачок от −3 до +7 КБ. В нуле — последовательный доступ\r\n"
        L"              (работает префетчер, строка 64 Б используется целиком, открытая строка DRAM).\r\n"
        L"  Запись RAM  То же для записи, панель на размер блока: сплошная — в обход кэша (MOVNT, без чтения строки), пунктир — обычная запись (RFO).\r\n"
        L"  Многопоток  Суммарная скорость ОЗУ при 1…N потоках: видно насыщение контроллера памяти и вклад P/E-ядер.\r\n"
        L"  Система     Процессор, кластеры ядер, кэши, модули памяти, режим работы.\r\n"
        L"\r\n"
        L"КАК ПОЛУЧИТЬ ТОЧНЫЙ РЕЗУЛЬТАТ\r\n"
        L"  • Ноутбук — на зарядке, режим питания «Максимальная производительность».\r\n"
        L"  • Закройте браузер и другие программы. Во время теста не двигайте окно.\r\n"
        L"  • Для гибридных CPU сравнивайте P- и E-ядро: выберите ядро в списке «Ядро» и повторите тест.\r\n"
        L"  • Каждая точка — лучший результат из 2–3 замеров. Нажмите «Старт» повторно, чтобы оценить разброс.\r\n"
        L"\r\n"
        L"КАК УСТРОЕНЫ ЗАМЕРЫ\r\n"
        L"  Время — QueryPerformanceCounter, частота ядра — 100 зависимых ADD (ровно 100 тактов) на время.\r\n"
        L"  Перед тестом ядро 0.25 с разгоняется до турбо-частоты. Процесс работает с высоким приоритетом,\r\n"
        L"  окно программы переносится на другие ядра, чтобы не мешать тесту.\r\n"
        L"  Скачок < 0 означает, что блоки идут в сторону младших адресов с промежутком |скачок|.\r\n"
        L"  1 ГБ/с = 10⁹ байт/с.\r\n";
}

// ================================================================ сохранение

static bool AskSavePath(const wchar_t* filter, const wchar_t* ext, std::wstring& path)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[MAX_PATH];
    swprintf(name, MAX_PATH, L"BenchKrasulya_%ls_%04d%02d%02d_%02d%02d.%ls", g_tabSel < TAB_SYS ? kTabNames[g_tabSel] : L"все", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, ext);
    for (wchar_t* p = name; *p; p++)
        if (*p == L' ') *p = L'_';
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = filter;
    of.lpstrFile = name;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = ext;
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&of)) return false;
    path = name;
    return true;
}

static bool GetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    auto* info = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++)
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    return false;
}

static bool SavePngTo(const std::wstring& path, int w, int h);

static void SavePng()
{
    if (g_tabSel >= TAB_SYS) {
        MessageBoxW(g_hwnd, L"Откройте вкладку с графиком (Кэш, Задержка, Чтение/Запись RAM, Многопоток).", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    std::wstring path;
    if (!AskSavePath(L"Изображение PNG\0*.png\0", L"png", path)) return;
    if (!SavePngTo(path, 0, 0)) MessageBoxW(g_hwnd, L"Не удалось сохранить PNG.", APP_NAME, MB_ICONERROR);
}

static bool SavePngTo(const std::wstring& path, int w, int h)
{
    RECT rc;
    GetClientRect(g_view, &rc);
    if (w > 0) rc = {0, 0, w, h};
    HDC wdc = GetDC(g_view);
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mdc, bmp);
    PaintView(mdc, rc, POINT{-1, -1});
    SelectObject(mdc, ob);
    bool ok = false;
    {
        Gdiplus::Bitmap gb(bmp, nullptr);
        CLSID png;
        if (GetEncoderClsid(L"image/png", &png)) ok = gb.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(g_view, wdc);
    return ok;
}

static std::wstring Num(double v)
{
    if (!std::isfinite(v)) return L"";
    std::wstring s = Fmt(L"%.3f", v);
    for (auto& c : s)
        if (c == L'.') c = L',';
    return s;
}

static bool SaveCsvTo(const std::wstring& path)
{
    std::wstring t;
    auto L = [&](const std::wstring& s) { t += s + L"\r\n"; };
    EnterCriticalSection(&g_bench.cs);
    L(APP_NAME L" " APP_VER L";" + g_cpu.brand + L";" + MemLabel());
    if (g_cache.valid) {
        L(L"");
        L(L"[Кэш];" + CpuName(g_cache.cpu) + L";" + g_cache.isaName + L";ГГц;" + Num(g_cache.ghzBefore));
        L(L"Размер, байт;Чтение, ГБ/с;Запись, ГБ/с;Запись NT, ГБ/с;Код, ГБ/с");
        for (size_t i = 0; i < g_cache.sizes.size(); i++)
            L(Fmt(L"%.0f;", g_cache.sizes[i]) + Num(g_cache.read[i]) + L";" + Num(g_cache.write[i]) + L";" + Num(g_cache.writeNT[i]) + L";" + Num(g_cache.code[i]));
    }
    if (g_lat.valid) {
        L(L"");
        L(L"[Задержка];" + CpuName(g_lat.cpu) + L";ГГц;" + Num(g_lat.ghz));
        L(L"Размер, байт;нс;такты");
        for (size_t i = 0; i < g_lat.sizes.size(); i++) L(Fmt(L"%.0f;", g_lat.sizes[i]) + Num(g_lat.ns[i]) + L";" + Num(g_lat.ns[i] * g_lat.ghz));
    }
    for (int w = 0; w < 2; w++) {
        const JumpResult& r = w ? g_jwrite : g_jread;
        if (!r.valid) continue;
        L(L"");
        L(std::wstring(w ? L"[Запись RAM];" : L"[Чтение RAM];") + CpuName(r.cpu) + L";область, байт;" + Fmt(L"%zu", r.span));
        std::wstring h = L"Скачок, байт";
        for (int b = 0; b < 4; b++) h += Fmt(w ? L";%d Б NT" : L";%d Б", kBlockSizes[b]);
        if (w)
            for (int b = 0; b < 4; b++) h += Fmt(L";%d Б кэш", kBlockSizes[b]);
        L(h);
        for (size_t i = 0; i < r.jumps.size(); i++) {
            std::wstring row = Fmt(L"%.0f", r.jumps[i]);
            for (int b = 0; b < 4; b++) row += L";" + Num(r.y[b][i]);
            if (w)
                for (int b = 0; b < 4; b++) row += L";" + Num(r.yc[b][i]);
            L(row);
        }
        std::wstring row = L"случайные";
        for (int b = 0; b < 4; b++) row += L";" + Num(r.random[b]);
        if (w)
            for (int b = 0; b < 4; b++) row += L";" + Num(r.randomC[b]);
        L(row);
    }
    if (g_mt.valid) {
        L(L"");
        L(L"[Многопоток]");
        L(L"Потоков;Чтение, ГБ/с;Запись NT, ГБ/с;Запись, ГБ/с");
        for (size_t i = 0; i < g_mt.n.size(); i++) L(Fmt(L"%.0f;", g_mt.n[i]) + Num(g_mt.read[i]) + L";" + Num(g_mt.writeNT[i]) + L";" + Num(g_mt.write[i]));
        for (auto& s : g_mt.summary) L(s.name + Fmt(L" [%d];", s.threads) + Num(s.read) + L";" + Num(s.writeNT) + L";" + Num(s.write));
    }
    LeaveCriticalSection(&g_bench.cs);
    L(L"");
    L(L"[Система]");
    std::wstring rep = SystemReport();
    t += rep;

    int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &u[0], n, nullptr, nullptr);
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return false;
    fwrite("\xEF\xBB\xBF", 1, 3, fp);
    fwrite(u.data(), 1, u.size(), fp);
    fclose(fp);
    return true;
}

static void SaveCsv()
{
    std::wstring path;
    if (!AskSavePath(L"Таблица CSV (Excel)\0*.csv\0", L"csv", path)) return;
    if (!SaveCsvTo(path)) MessageBoxW(g_hwnd, L"Не удалось записать файл.", APP_NAME, MB_ICONERROR);
}

static void AutoSave()
{
    MakeDirs(g_autoDir);
    static const wchar_t* files[TAB_SYS] = {L"1_cache", L"2_latency", L"3_read", L"4_write", L"5_mt"};
    int keep = g_tabSel;
    for (int t = 0; t < TAB_SYS; t++) {
        g_tabSel = t;
        SavePngTo(g_autoDir + L"\\" + files[t] + L".png", S(1160), S(780));
    }
    g_tabSel = keep;
    SaveCsvTo(g_autoDir + L"\\results.csv");
    std::wstring rep = SystemReport();
    int n = WideCharToMultiByte(CP_UTF8, 0, rep.c_str(), (int)rep.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, rep.c_str(), (int)rep.size(), &u[0], n, nullptr, nullptr);
    if (FILE* fp = _wfopen((g_autoDir + L"\\system.txt").c_str(), L"wb")) {
        fwrite("\xEF\xBB\xBF", 1, 3, fp);
        fwrite(u.data(), 1, u.size(), fp);
        fclose(fp);
    }
}

// ================================================================ запуск тестов

static void EnableControls()
{
    bool run = g_bench.running;
    bool ok = g_ready && !run;
    for (HWND h : {g_cbCpu, g_cbIsa, g_cbArea, g_btnStart, g_btnAll, g_btnPng, g_btnCsv}) EnableWindow(h, ok);
    EnableWindow(g_chkLarge, ok && LargePagesAvailable());
    EnableWindow(g_btnStop, run);
    EnableWindow(g_btnStart, ok && g_tabSel < TAB_SYS);
}

static void ShowTab(int t)
{
    g_tabSel = t;
    for (HWND b : g_tabBtn) InvalidateRect(b, nullptr, FALSE);
    bool text = t >= TAB_SYS;
    if (text) SetWindowTextW(g_edit, t == TAB_SYS ? SystemReport().c_str() : AboutText().c_str());
    ShowWindow(g_view, text ? SW_HIDE : SW_SHOW);
    ShowWindow(g_edit, text ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_view, nullptr, FALSE);
    EnableControls();
}

static DWORD WINAPI WorkerThread(LPVOID)
{
    for (TestKind k : g_queue) {
        if (g_bench.stop) break;
        g_bench.progress = 0;
        PostMessageW(g_hwnd, WM_APP_TESTSTART, k, 0);
        RunTest(k, g_runCfg);
    }
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void StartTests(const std::vector<TestKind>& q)
{
    if (g_bench.running || !g_ready) return;
    g_queue = q;
    g_runCfg.cpu = SelectedCpu();
    int ii = (int)SendMessageW(g_cbIsa, CB_GETCURSEL, 0, 0);
    g_runCfg.isa = ii >= 0 && ii < (int)g_isaMap.size() ? g_isaMap[ii] : ISA_SSE2;
    g_runCfg.ramMode = SendMessageW(g_cbArea, CB_GETCURSEL, 0, 0) != 1;
    g_runCfg.largePages = Checked(g_chkLarge);
    g_bench.stop = false;
    g_bench.progress = 0;
    g_bench.running = true;
    g_runStart = NowSec();

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    // окно программы — на любые ядра, кроме тестируемого (и его HT-брата)
    const LogicalCpu& tc = g_cpu.cpus[g_runCfg.cpu];
    DWORD_PTR mask = 0;
    for (auto& c : g_cpu.cpus)
        if (c.group == 0 && c.core != tc.core) mask |= (DWORD_PTR)1 << c.number;
    g_guiAffinity = mask ? SetThreadAffinityMask(GetCurrentThread(), mask) : 0;

    EnableControls();
    SetTimer(g_hwnd, TIMER_UI, 120, nullptr);
    g_worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
}

static const int kStatusH = 30;  // высота строки состояния (до масштабирования)

static void SetStatus(const std::wstring& s)
{
    g_statusText = s;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    rc.top = rc.bottom - S(kStatusH);
    InvalidateRect(g_hwnd, &rc, FALSE);
}

static void UpdateStatus()
{
    std::wstring s = g_bench.running ? g_bench.GetStatus() : L"";
    SetStatus(s);
    static std::wstring last;
    if (!g_autoDir.empty() && s != last) {  // журнал хода тестов в авто-режиме
        last = s;
        MakeDirs(g_autoDir);
        if (FILE* fp = _wfopen((g_autoDir + L"\\log.txt").c_str(), L"a, ccs=UTF-8")) {
            fwprintf(fp, L"%8.2f  %ls\n", NowSec() - g_runStart, s.c_str());
            fclose(fp);
        }
    }
    g_progPos = g_bench.progress;
}

// ================================================================ окно графика

static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
        HGDIOBJ ob = SelectObject(mdc, bmp);
        PaintView(mdc, rc, g_hover);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (p.x != g_hover.x || p.y != g_hover.y) {
            g_hover = p;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, h, 0};
        TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = {-1, -1};
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================================================================ тёмная тема

static void FillRound(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int rad)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void Polyline3(HDC dc, POINT a, POINT b, POINT c, COLORREF col, int width)
{
    HPEN p = CreatePen(PS_SOLID, width, col);
    HGDIOBJ op = SelectObject(dc, p);
    POINT pt[3] = {a, b, c};
    Polyline(dc, pt, 3);
    SelectObject(dc, op);
    DeleteObject(p);
}

// подсветка под курсором для кнопок и списков
static void TrackHover(HWND h, UINT m)
{
    if (m == WM_MOUSEMOVE && g_hot != h) {
        HWND old = g_hot;
        g_hot = h;
        if (old) InvalidateRect(old, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE);
        TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, h, 0};
        TrackMouseEvent(&te);
    } else if (m == WM_MOUSELEAVE && g_hot == h) {
        g_hot = nullptr;
        InvalidateRect(h, nullptr, FALSE);
    }
}

static LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    TrackHover(h, m);
    if (m == WM_LBUTTONDBLCLK) m = WM_LBUTTONDOWN;  // быстрые щелчки — тоже щелчки, а не двойной клик
    return DefSubclassProc(h, m, w, l);
}

static std::wstring ComboText(HWND h, int i)
{
    if (i < 0) return L"";
    std::wstring t((size_t)std::max<LRESULT>(0, SendMessageW(h, CB_GETLBTEXTLEN, i, 0)) + 1, L'\0');
    SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)&t[0]);
    t.resize(wcslen(t.c_str()));
    return t;
}

static LRESULT CALLBACK ComboProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    TrackHover(h, m);
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        bool dis = !IsWindowEnabled(h), open = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0, hot = g_hot == h && !dis;
        FillRect(dc, &r, g_brBg);
        FillRound(dc, r, dis ? th::bg : hot || open ? th::panelHot : th::panel,
                  open || GetFocus() == h ? th::accent : hot ? th::frame : th::border, S(8));
        HGDIOBJ of = SelectObject(dc, g_fUI);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dis ? th::disabled : th::text);
        std::wstring t = ComboText(h, (int)SendMessageW(h, CB_GETCURSEL, 0, 0));
        RECT tr = {r.left + S(10), r.top, r.right - S(28), r.bottom};
        DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        int cx = r.right - S(15), cy = (r.top + r.bottom) / 2;
        Polyline3(dc, {cx - S(4), cy - S(2)}, {cx, cy + S(2)}, {cx + S(4) + 1, cy - S(3)}, dis ? th::disabled : th::muted, std::max(1, S(2) - 1));
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    LRESULT res = DefSubclassProc(h, m, w, l);
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_ENABLE || m == CB_SETCURSEL) InvalidateRect(h, nullptr, FALSE);
    return res;
}

static void DrawComboItem(const DRAWITEMSTRUCT* d)
{
    if (d->itemID == (UINT)-1 || (d->itemState & ODS_COMBOBOXEDIT)) return;  // поле выбора рисует ComboProc
    bool sel = d->itemState & ODS_SELECTED;
    HBRUSH b = CreateSolidBrush(sel ? th::accent : th::panel);
    FillRect(d->hDC, &d->rcItem, b);
    DeleteObject(b);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, sel ? RGB(255, 255, 255) : th::text);
    HGDIOBJ of = SelectObject(d->hDC, g_fUI);
    RECT tr = d->rcItem;
    tr.left += S(10);
    std::wstring t = ComboText(d->hwndItem, (int)d->itemID);
    DrawTextW(d->hDC, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d->hDC, of);
}

static void DrawButton(const DRAWITEMSTRUCT* d)
{
    HWND w = d->hwndItem;
    int id = (int)d->CtlID;
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool dis = d->itemState & ODS_DISABLED, down = d->itemState & ODS_SELECTED, hot = w == g_hot && !dis;
    FillRect(dc, &r, g_brBg);
    wchar_t txt[80];
    GetWindowTextW(w, txt, 80);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_fUI);
    if (id >= IDC_TAB0 && id < IDC_TAB0 + (int)TAB_COUNT) {  // вкладка: текст и полоса снизу у выбранной
        bool sel = id - IDC_TAB0 == g_tabSel;
        if (hot && !sel) {
            RECT b = {r.left + S(2), r.top + S(3), r.right - S(2), r.bottom - S(5)};
            FillRound(dc, b, th::panel, th::panel, S(8));
        }
        SelectObject(dc, sel ? g_fUIBold : g_fUI);
        SetTextColor(dc, sel ? th::text : hot ? RGB(205, 210, 220) : th::muted);
        RECT tr = r;
        tr.bottom -= S(2);
        DrawTextW(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (sel) {
            RECT u = {r.left + S(10), r.bottom - S(3), r.right - S(10), r.bottom + S(2)};
            FillRound(dc, u, th::accent, th::accent, S(3));
        }
    } else if (id == IDC_LARGE || id == IDC_LOGY || id == IDC_FILTER) {  // галочка
        bool on = Checked(w);
        int bs = S(16);
        RECT b = {r.left + S(1), (r.top + r.bottom - bs) / 2, r.left + S(1) + bs, (r.top + r.bottom + bs) / 2};
        COLORREF fill = on ? (dis ? th::disabled : hot ? th::accentHot : th::accent) : hot ? th::panelHot : th::panel;
        FillRound(dc, b, fill, on ? fill : hot ? th::muted : th::frame, S(6));
        if (on)
            Polyline3(dc, {b.left + S(4), b.top + bs / 2}, {b.left + S(7), b.bottom - S(5)}, {b.right - S(4), b.top + S(4)},
                      RGB(255, 255, 255), S(2));
        RECT tr = r;
        tr.left = b.right + S(8);
        SetTextColor(dc, dis ? th::disabled : th::text);
        DrawTextW(dc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {  // кнопка: «Все тесты» — основная (синяя), «Стоп» — красная рамка
        COLORREF fill, line, tc;
        if (dis) {
            fill = th::panel, line = th::panel, tc = th::disabled;
        } else if (id == IDC_ALL) {
            fill = down ? th::accent : hot ? th::accentHot : th::accent, line = fill, tc = RGB(255, 255, 255);
        } else if (id == IDC_STOP) {
            fill = down ? RGB(80, 40, 42) : hot ? RGB(64, 36, 38) : th::panel, line = th::danger, tc = RGB(255, 150, 140);
        } else {
            fill = down ? th::panelDown : hot ? th::panelHot : th::panel, line = hot ? th::frame : th::border, tc = th::text;
        }
        FillRound(dc, r, fill, line, S(10));
        SetTextColor(dc, tc);
        DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
}

// строка состояния: текст слева, полоса хода тестов справа
static void PaintMain(HDC dc, const RECT& rc)
{
    FillRect(dc, &rc, g_brBg);
    if (g_tabBtn[0]) {  // линия под вкладками
        RECT tr;
        GetWindowRect(g_tabBtn[0], &tr);
        MapWindowPoints(nullptr, g_hwnd, (POINT*)&tr, 2);
        RECT ln = {S(10), tr.bottom, rc.right - S(10), tr.bottom + 1};
        HBRUSH b = CreateSolidBrush(th::border);
        FillRect(dc, &ln, b);
        DeleteObject(b);
    }
    RECT sr = {0, rc.bottom - S(kStatusH), rc.right, rc.bottom};
    FillRect(dc, &sr, g_brPanel);
    RECT ln = {0, sr.top, rc.right, sr.top + 1};
    HBRUSH b = CreateSolidBrush(th::border);
    FillRect(dc, &ln, b);
    DeleteObject(b);
    HGDIOBJ of = SelectObject(dc, g_fUI);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, th::muted);
    RECT tr = {S(12), sr.top + 1, rc.right - S(260), sr.bottom};
    DrawTextW(dc, g_statusText.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    if (!g_bench.running && g_progPos <= 0) return;  // полоса хода — только когда есть что показать
    int cy = (sr.top + sr.bottom) / 2;
    RECT track = {rc.right - S(236), cy - S(3), rc.right - S(16), cy + S(3)};
    FillRound(dc, track, th::panelDown, th::panelDown, S(6));
    if (g_progPos > 0) {
        RECT fill = track;
        fill.right = fill.left + std::max<LONG>(S(6), MulDiv(track.right - track.left, std::min(g_progPos, 1000), 1000));
        COLORREF c = g_progPos >= 1000 && !g_bench.running ? RGB(63, 185, 80) : th::accent;
        FillRound(dc, fill, c, c, S(6));
    }
}

// тёмный заголовок окна и тёмные полосы прокрутки (Windows 10 1809+; на старых системах — как было)
static DWORD WinBuild()
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    auto f = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {sizeof v};
    return f && f(&v) == 0 && v.dwMajorVersion >= 10 ? v.dwBuildNumber : 0;
}

static HMODULE SystemDll(const wchar_t* name)
{
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    return LoadLibraryW((std::wstring(dir) + L"\\" + name).c_str());
}

static void DarkTitle(HWND h)
{
    DWORD build = WinBuild();
    if (build < 17763) return;
    HMODULE dwm = SystemDll(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    auto set = (SetAttr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!set) return;
    BOOL on = TRUE;
    set(h, build >= 18985 ? 20 : 19, &on, sizeof on);  // DWMWA_USE_IMMERSIVE_DARK_MODE
    if (build >= 22000) {
        COLORREF cap = th::bg, br = th::border;
        set(h, 35, &cap, sizeof cap);  // DWMWA_CAPTION_COLOR
        set(h, 34, &br, sizeof br);    // DWMWA_BORDER_COLOR
    }
}

static void DarkScrollbars(HWND h)
{
    DWORD build = WinBuild();
    if (build < 17763) return;
    HMODULE ux = SystemDll(L"uxtheme.dll");
    if (!ux) return;
    typedef int(WINAPI * AppModeFn)(int);
    typedef BOOL(WINAPI * AllowWndFn)(HWND, BOOL);
    typedef HRESULT(WINAPI * SetThemeFn)(HWND, LPCWSTR, LPCWSTR);
    auto appMode = (AppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));  // SetPreferredAppMode / AllowDarkModeForApp
    auto allow = (AllowWndFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));   // AllowDarkModeForWindow
    auto theme = (SetThemeFn)(void*)GetProcAddress(ux, "SetWindowTheme");
    if (!appMode || !allow || !theme) return;
    appMode(1);
    allow(h, TRUE);
    theme(h, L"DarkMode_Explorer", nullptr);
}

// ================================================================ главное окно

static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, g_hi, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    if (!wcscmp(cls, L"BUTTON")) SetWindowSubclass(h, ButtonProc, 1, 0);
    if (!wcscmp(cls, WC_COMBOBOXW)) SetWindowSubclass(h, ComboProc, 1, 0);
    return h;
}

static void Layout()
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int x = S(12), y = S(12), h = S(30), gap = S(8);
    auto place = [&](HWND w, int width, int dy = 0, int hh = 0) {
        MoveWindow(w, x, y + dy, width, hh ? hh : h, TRUE);
        x += width + gap;
    };
    place(g_lblCpu, S(38), S(7), S(20));
    place(g_cbCpu, S(270), 0, S(400));
    x += S(8);
    place(g_lblIsa, S(80), S(7), S(20));
    place(g_cbIsa, S(150), 0, S(200));
    x += S(8);
    place(g_lblArea, S(130), S(7), S(20));
    place(g_cbArea, S(270), 0, S(200));
    x = S(12);
    y = S(52);
    h = S(32);
    place(g_btnStart, S(110));
    place(g_btnAll, S(110));
    place(g_btnStop, S(80));
    x += S(10);
    place(g_btnPng, S(118));
    place(g_btnCsv, S(118));
    x += S(10);
    x += S(6);
    place(g_chkLarge, S(132));
    place(g_chkLogY, S(116));
    place(g_chkFilter, S(146));

    // вкладки — ширина по тексту
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ of = SelectObject(dc, g_fUIBold);
    x = S(10);
    for (int i = 0; i < TAB_COUNT; i++) {
        SIZE sz;
        GetTextExtentPoint32W(dc, kTabNames[i], (int)wcslen(kTabNames[i]), &sz);
        MoveWindow(g_tabBtn[i], x, S(96), sz.cx + S(28), S(34), TRUE);
        x += sz.cx + S(28);
    }
    SelectObject(dc, of);
    ReleaseDC(g_hwnd, dc);

    RECT dr = {S(10), S(138), rc.right - S(10), rc.bottom - S(kStatusH) - S(8)};
    SetWindowPos(g_view, HWND_TOP, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, 0);
    SetWindowPos(g_edit, HWND_TOP, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, 0);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static DWORD WINAPI InitThread(LPVOID)
{
    QueryCpuInfo(g_cpu);
    DetectCoreTypes(g_cpu);
    QueryMemInfo(g_mem);
    MeasureClusterClocks();
    PostMessageW(g_hwnd, WM_APP_READY, 0, 0);
    return 0;
}

static void OnReady()
{
    g_coreNo.assign(g_cpu.cpus.size(), 0);
    for (auto& k : g_cpu.clusters) {
        int n = -1, lastCore = -1;
        for (int c : k.cpus) {
            if (g_cpu.cpus[c].core != lastCore) n++, lastCore = g_cpu.cpus[c].core;
            g_coreNo[c] = n;
        }
    }
    for (size_t i = 0; i < g_cpu.cpus.size(); i++) SendMessageW(g_cbCpu, CB_ADDSTRING, 0, (LPARAM)CpuName((int)i).c_str());
    int def = g_cpu.clusters.empty() ? 0 : g_cpu.clusters[0].rep;
    SendMessageW(g_cbCpu, CB_SETCURSEL, def, 0);

    g_isaMap.push_back(ISA_SSE2);
    SendMessageW(g_cbIsa, CB_ADDSTRING, 0, (LPARAM)L"SSE2 · 128 бит");
    if (g_cpu.avx) {
        g_isaMap.push_back(ISA_AVX);
        SendMessageW(g_cbIsa, CB_ADDSTRING, 0, (LPARAM)L"AVX · 256 бит");
    }
    if (g_cpu.avx512) {
        g_isaMap.push_back(ISA_AVX512);
        SendMessageW(g_cbIsa, CB_ADDSTRING, 0, (LPARAM)L"AVX-512 · 512 бит");
    }
    SendMessageW(g_cbIsa, CB_SETCURSEL, g_isaMap.size() - 1, 0);

    SendMessageW(g_cbArea, CB_ADDSTRING, 0, (LPARAM)(L"ОЗУ: " + FormatSize(RamSpanBytes()) + L" (≥ 4×L3) — точно").c_str());
    SendMessageW(g_cbArea, CB_ADDSTRING, 0, (LPARAM)L"4 МБ — в пределах L3");
    SendMessageW(g_cbArea, CB_SETCURSEL, 0, 0);

    g_ready = true;
    SYSTEM_POWER_STATUS ps = {};
    GetSystemPowerStatus(&ps);
    std::wstring st = Fmt(L"Готово. %ls, %d яд./%zu пот., %ls, %.1f ГБ.", g_cpu.brand.c_str(), g_cpu.physCores, g_cpu.cpus.size(),
                          MemLabel().c_str(), g_mem.totalMB / 1024.0);
    if (ps.ACLineStatus == 0) st += L"  ⚠ Работа от батареи — подключите зарядку!";
    SetStatus(st);
    ShowTab(g_tabSel);
    if (!g_autoDir.empty()) {
        if (g_autoCpu >= 0 && g_autoCpu < (int)g_cpu.cpus.size()) SendMessageW(g_cbCpu, CB_SETCURSEL, g_autoCpu, 0);
        if (g_autoCompat) SendMessageW(g_cbArea, CB_SETCURSEL, 1, 0);
        std::vector<TestKind> q;
        const std::wstring ts = g_autoTests.empty() ? L"clrwm" : g_autoTests;
        const wchar_t* letters = L"clrwm";
        for (int i = 0; i < TEST_COUNT; i++)
            if (ts.find(letters[i]) != std::wstring::npos) q.push_back((TestKind)i);
        StartTests(q);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        for (int i = 0; i < TAB_COUNT; i++) g_tabBtn[i] = MakeCtl(L"BUTTON", kTabNames[i], BS_OWNERDRAW, IDC_TAB0 + i);
        g_lblCpu = MakeCtl(L"STATIC", L"Ядро:", 0, 0);
        g_cbCpu = MakeCtl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, IDC_CPU);
        g_lblIsa = MakeCtl(L"STATIC", L"Инструкции:", 0, 0);
        g_cbIsa = MakeCtl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, IDC_ISA);
        g_lblArea = MakeCtl(L"STATIC", L"Область RAM-тестов:", 0, 0);
        g_cbArea = MakeCtl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, IDC_AREA);
        g_btnStart = MakeCtl(L"BUTTON", L"▶ Старт", BS_OWNERDRAW | WS_TABSTOP, IDC_START);
        g_btnAll = MakeCtl(L"BUTTON", L"Все тесты", BS_OWNERDRAW | WS_TABSTOP, IDC_ALL);
        g_btnStop = MakeCtl(L"BUTTON", L"■ Стоп", BS_OWNERDRAW | WS_TABSTOP, IDC_STOP);
        g_btnPng = MakeCtl(L"BUTTON", L"Сохранить PNG", BS_OWNERDRAW | WS_TABSTOP, IDC_PNG);
        g_btnCsv = MakeCtl(L"BUTTON", L"Сохранить CSV", BS_OWNERDRAW | WS_TABSTOP, IDC_CSV);
        g_chkLarge = MakeCtl(L"BUTTON", L"Страницы 2 МБ", BS_OWNERDRAW | WS_TABSTOP, IDC_LARGE);
        g_chkLogY = MakeCtl(L"BUTTON", L"Лог. шкала Y", BS_OWNERDRAW | WS_TABSTOP, IDC_LOGY);
        g_chkFilter = MakeCtl(L"BUTTON", L"Сырые точки", BS_OWNERDRAW | WS_TABSTOP, IDC_FILTER);
        g_view = CreateWindowExW(0, L"BenchKrasulyaView", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)IDC_VIEW, g_hi, nullptr);
        g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL,
                                 0, 0, 10, 10, h, (HMENU)IDC_EDIT, g_hi, nullptr);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fMono, TRUE);
        SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(S(14), S(14)));
        DarkScrollbars(g_edit);
        DarkTitle(h);
        SetStatus(L"Определение процессора, кэшей и памяти…");
        Layout();
        ShowTab(0);
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
        return 0;
    }
    case WM_SIZE:
        if (!g_view) return 0;
        Layout();
        InvalidateRect(g_view, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = {S(1000), S(640)};
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
        HGDIOBJ ob = SelectObject(mdc, bmp);
        PaintMain(mdc, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_edit) {
            SetTextColor((HDC)w, th::text);
            SetBkColor((HDC)w, th::chartBg);
            return (LRESULT)g_brEdit;
        }
        SetTextColor((HDC)w, th::muted);
        SetBkColor((HDC)w, th::bg);
        return (LRESULT)g_brBg;
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, th::text);
        SetBkColor((HDC)w, th::panel);
        return (LRESULT)g_brPanel;
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlType == ODT_COMBOBOX) mi->itemHeight = mi->itemID == (UINT)-1 ? S(24) : S(26);
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* d = (DRAWITEMSTRUCT*)l;
        if (d->CtlType == ODT_COMBOBOX) DrawComboItem(d);
        else if (d->CtlType == ODT_BUTTON) DrawButton(d);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_LARGE || id == IDC_LOGY || id == IDC_FILTER) {
            SetWindowLongPtrW((HWND)l, GWLP_USERDATA, !Checked((HWND)l));
            InvalidateRect((HWND)l, nullptr, FALSE);
        }
        if (id >= IDC_TAB0 && id < IDC_TAB0 + (int)TAB_COUNT) ShowTab(id - IDC_TAB0);
        else if (id == IDC_START && g_tabSel < TAB_SYS) StartTests({(TestKind)g_tabSel});
        else if (id == IDC_ALL) StartTests({TEST_CACHE, TEST_LATENCY, TEST_READ, TEST_WRITE, TEST_MT});
        else if (id == IDC_STOP) g_bench.stop = true;
        else if (id == IDC_PNG) SavePng();
        else if (id == IDC_CSV) SaveCsv();
        else if (id == IDC_LOGY || id == IDC_FILTER) InvalidateRect(g_view, nullptr, FALSE);
        else if (id == IDC_CPU && HIWORD(w) == CBN_SELCHANGE) InvalidateRect(g_view, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        UpdateStatus();
        InvalidateRect(g_view, nullptr, FALSE);
        return 0;
    case WM_APP_READY:
        OnReady();
        return 0;
    case WM_APP_TESTSTART:
        ShowTab((int)w);
        return 0;
    case WM_APP_DONE: {
        KillTimer(h, TIMER_UI);
        if (g_worker) {
            WaitForSingleObject(g_worker, INFINITE);
            CloseHandle(g_worker);
            g_worker = nullptr;
        }
        g_bench.running = false;
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        if (g_guiAffinity) SetThreadAffinityMask(GetCurrentThread(), g_guiAffinity);
        g_bench.progress = g_bench.stop ? g_bench.progress.load() : 1000;
        UpdateStatus();
        std::wstring st = Fmt(L"%ls за %.1f с.", g_bench.stop ? L"Остановлено" : L"Готово", NowSec() - g_runStart);
        SetStatus(st);
        EnableControls();
        InvalidateRect(g_view, nullptr, FALSE);
        if (!g_autoDir.empty()) {
            AutoSave();
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_bench.running) {
            g_bench.stop = true;
            if (g_worker) WaitForSingleObject(g_worker, 15000);
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show)
{
    g_hi = hi;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--compat")) g_autoCompat = true;
        if (i + 1 >= argc) continue;
        if (!wcscmp(argv[i], L"--auto")) g_autoDir = argv[i + 1];
        if (!wcscmp(argv[i], L"--cpu")) g_autoCpu = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--tests")) g_autoTests = argv[i + 1];
    }
#ifdef BK_EMU
    if (EmuParseArgs(argc, argv)) {
        LocalFree(argv);
        return 0;
    }
#endif
    LocalFree(argv);
    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(nullptr, sdc);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtok;
    Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr);
    BenchInit();

    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    auto font = [](const wchar_t* face, int pt10, int weight) {
        return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    g_fUI = font(L"Segoe UI", 95, FW_NORMAL);
    g_fUIBold = font(L"Segoe UI", 95, FW_SEMIBOLD);
    g_brBg = CreateSolidBrush(th::bg);
    g_brPanel = CreateSolidBrush(th::panel);
    g_brEdit = CreateSolidBrush(th::chartBg);
    g_fChart = font(L"Segoe UI", 90, FW_NORMAL);
    g_fChartBold = font(L"Segoe UI", 90, FW_SEMIBOLD);
    g_fTable = font(L"Segoe UI", 95, FW_NORMAL);
    g_fTableBold = font(L"Segoe UI", 95, FW_SEMIBOLD);
    g_fMono = font(L"Consolas", 100, FW_NORMAL);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = ViewProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = L"BenchKrasulyaView";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"BenchKrasulyaMain";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    std::wstring title = APP_NAME L" " APP_VER;
#ifdef BK_EMU
    if (EmuActive()) title += L"   [" + EmuTitle() + L"]";
#endif
    HWND h = CreateWindowExW(0, L"BenchKrasulyaMain", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, S(1180), S(820), nullptr, nullptr, hi, nullptr);
    ShowWindow(h, show);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    Gdiplus::GdiplusShutdown(gtok);
    return 0;
}
