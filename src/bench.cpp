// BenchKrasulya — тесты
#include "bench.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <psapi.h>

BenchState g_bench;
CpuInfo g_cpu;
MemInfo g_mem;
CacheResult g_cache;
LatencyResult g_lat;
JumpResult g_jread, g_jwrite;
MTResult g_mt;

static double g_qpf = 1;
static bool g_largeOk = false;
static volatile uint64_t g_sink;

static inline double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_qpf;
}

void BenchState::SetStatus(const std::wstring& s)
{
    EnterCriticalSection(&cs);
    status = s;
    LeaveCriticalSection(&cs);
}

std::wstring BenchState::GetStatus()
{
    EnterCriticalSection(&cs);
    std::wstring s = status;
    LeaveCriticalSection(&cs);
    return s;
}

struct ResultLock {
    ResultLock() { EnterCriticalSection(&g_bench.cs); }
    ~ResultLock() { LeaveCriticalSection(&g_bench.cs); }
};

// Большие страницы требуют права «Блокировка страниц в памяти» (SeLockMemoryPrivilege)
static bool EnableLockMemory()
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

void BenchInit()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (double)f.QuadPart;
    InitializeCriticalSection(&g_bench.cs);
    g_largeOk = EnableLockMemory() && GetLargePageMinimum() > 0;
}

bool LargePagesAvailable() { return g_largeOk; }

// ---------------------------------------------------------------- буферы

struct Buf {
    uint8_t* p = nullptr;
    size_t size = 0;
    bool large = false;
    bool Alloc(size_t sz, bool tryLarge, bool exec = false)
    {
        Free();
        DWORD prot = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        if (tryLarge && g_largeOk) {
            size_t lp = GetLargePageMinimum();
            size_t sz2 = (sz + lp - 1) / lp * lp;
            p = (uint8_t*)VirtualAlloc(nullptr, sz2, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, prot);
            if (p) large = true;
        }
        if (!p) p = (uint8_t*)VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_COMMIT, prot);
        size = p ? sz : 0;
        if (p) memset(p, 0x5A, sz);  // реально выделить страницы до замеров
        return p != nullptr;
    }
    void Free()
    {
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        p = nullptr;
        size = 0;
        large = false;
    }
    ~Buf() { Free(); }
};

static uint64_t Pow2Ceil(uint64_t v)
{
    uint64_t r = 1;
    while (r < v) r <<= 1;
    return r;
}

static uint64_t Pow2Floor(uint64_t v)
{
    uint64_t r = 1;
    while (r * 2 <= v) r <<= 1;
    return r;
}

static uint64_t AvailBytes()
{
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    uint64_t avail = ms.ullAvailPhys;
    // процесс может быть в job-объекте с лимитом памяти (песочницы, планировщики задач) —
    // GlobalMemoryStatusEx его не видит, а VirtualAlloc сверх лимита вернёт NULL
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof pmc;
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &li, sizeof li, nullptr) &&
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc)) {
        DWORD f = li.BasicLimitInformation.LimitFlags;
        uint64_t lim = UINT64_MAX;
        if (f & JOB_OBJECT_LIMIT_PROCESS_MEMORY) lim = li.ProcessMemoryLimit;
        if (f & JOB_OBJECT_LIMIT_JOB_MEMORY) lim = std::min<uint64_t>(lim, li.JobMemoryLimit);
        if (lim != UINT64_MAX) avail = std::min<uint64_t>(avail, lim > pmc.PrivateUsage ? lim - pmc.PrivateUsage : 0);
    }
    return avail;
}

size_t RamSpanBytes()
{
    // Область заведомо больше всех L3 (в 4 раза), но не меньше 128 МБ и не больше 1 ГБ
    uint64_t s = Pow2Ceil(std::max<uint64_t>(4 * g_cpu.totalL3, 128ull << 20));
    s = std::min<uint64_t>(s, 1ull << 30);
    uint64_t lim = Pow2Floor(std::max<uint64_t>(AvailBytes() / 3, 64ull << 20));
    return (size_t)std::min(s, lim);
}

// ---------------------------------------------------------------- частота

// 100 зависимых сложений регистр+регистр = ровно 100 тактов на любом x86 ядре.
// add с константой не годится: P-ядра Golden Cove/Raptor Cove «сворачивают» такие цепочки
// при переименовании регистров и выполняют по несколько за такт (частота выходит в 5–6 раз выше).
static void AddChain(uint64_t iters)
{
    uint64_t x = 0, y = 1;
    __asm__ volatile(
        "1:\n\t"
        ".rept 100\n\t"
        "add %2, %0\n\t"
        ".endr\n\t"
        "dec %1\n\t"
        "jnz 1b\n\t"
        : "+r"(x), "+r"(iters)
        : "r"(y)
        : "cc");
}

static void SpinWarmup(double sec)
{
    double t0 = Now();
    while (Now() - t0 < sec) AddChain(20000);
}

double MeasureCoreGHz()
{
    double best = 0;
    for (int t = 0; t < 5; t++) {
        const uint64_t it = 100000;  // 10 млн тактов
        double t0 = Now();
        AddChain(it);
        double dt = Now() - t0;
        best = std::max(best, it * 100.0 / dt / 1e9);
    }
    return best;
}

static DWORD WINAPI ClockThread(LPVOID arg)
{
    CpuCluster* k = (CpuCluster*)arg;
    PinCurrentThread(g_cpu.cpus[k->rep]);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DisablePowerThrottling();
    SpinWarmup(0.15);
    k->clockGHz = MeasureCoreGHz();
    return 0;
}

void MeasureClusterClocks()
{
    for (auto& k : g_cpu.clusters) {
        HANDLE h = CreateThread(nullptr, 0, ClockThread, &k, 0, nullptr);
        if (h) {
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
        }
    }
}

// ---------------------------------------------------------------- общие помощники

struct Progress {
    size_t done = 0, total = 1;
    void Step(size_t n = 1)
    {
        done += n;
        g_bench.progress = (int)std::min<size_t>(1000, done * 1000 / std::max<size_t>(total, 1));
    }
};

static void PrepareSingleThread(const BenchConfig& cfg)
{
    PinCurrentThread(g_cpu.cpus[cfg.cpu]);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DisablePowerThrottling();
    SpinWarmup(0.25);  // разгоняем ядро до турбо-частоты до начала замеров
}

// Лучший результат из нескольких попыток, каждая ≥ target секунд
template <class F>
static double MeasureBW(F fn, size_t bytes, int trials = 3, double target = 0.004)
{
    fn(1);  // прогрев: данные попадают в нужный уровень кэша
    double t0 = Now();
    fn(1);
    double t1 = Now() - t0;
    size_t reps = t1 > 1e-9 ? (size_t)std::max(1.0, target / t1) : 100000;
    reps = std::min<size_t>(reps, 50000000);
    double best = 0;
    for (int t = 0; t < trials; t++) {
        t0 = Now();
        fn(reps);
        double dt = Now() - t0;
        if (dt > 0) best = std::max(best, (double)bytes * reps / dt);
    }
    return best / 1e9;
}

static std::vector<double> SizeList(size_t from, size_t to)
{
    std::vector<double> v;
    for (int e = 0;; e++) {
        double s = from * std::pow(2.0, e / 4.0);
        if (s > to * 1.0001) break;
        size_t r = ((size_t)(s + 511) / 1024) * 1024;
        if (r < 1024) r = 1024;
        if (v.empty() || r != (size_t)v.back()) v.push_back((double)r);
    }
    return v;
}

static uint64_t LlcFor(const LogicalCpu& c)
{
    if (c.l3.size) return c.l3.size;
    if (c.l2.size) return c.l2.size;
    return 8ull << 20;
}

// ---------------------------------------------------------------- тест «Кэш»

static void RunCache(const BenchConfig& cfg)
{
    const LogicalCpu& lc = g_cpu.cpus[cfg.cpu];
    KernelSet k = MakeKernels(cfg.isa);
    uint64_t llc = std::max<uint64_t>(LlcFor(lc), g_cpu.maxL3);
    uint64_t maxSize = Pow2Ceil(8 * llc);
    maxSize = std::max<uint64_t>(maxSize, 64ull << 20);
    maxSize = std::min<uint64_t>(maxSize, 1ull << 30);
    maxSize = std::min<uint64_t>(maxSize, Pow2Floor(std::max<uint64_t>(AvailBytes() / 3, 64ull << 20)));

    g_bench.SetStatus(L"Кэш: выделение памяти…");
    Buf data, code;
    if (!data.Alloc(maxSize, cfg.largePages)) {
        g_bench.SetStatus(L"Не удалось выделить память");
        return;
    }
    size_t codeMax = (size_t)std::min<uint64_t>(maxSize, 256ull << 20);
    if (code.Alloc(codeMax, false, true)) {
        static const uint8_t nop8[8] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
        for (size_t i = 0; i + 8 <= codeMax; i += 8) memcpy(code.p + i, nop8, 8);
    }

    CacheResult r;
    r.valid = true;
    r.cpu = cfg.cpu;
    r.isaName = k.name;
    r.sizes = SizeList(2048, (size_t)maxSize);
    size_t n = r.sizes.size();
    r.read.assign(n, NAN);
    r.write.assign(n, NAN);
    r.writeNT.assign(n, NAN);
    r.code.assign(n, NAN);

    PrepareSingleThread(cfg);
    r.ghzBefore = MeasureCoreGHz();
    {
        ResultLock l;
        g_cache = r;
    }

    Progress pr;
    pr.total = n;
    for (size_t i = 0; i < n && !g_bench.stop; i++) {
        size_t sz = (size_t)r.sizes[i];
        g_bench.SetStatus(Fmt(L"Кэш: блок %ls (%zu из %zu)", FormatSize(sz).c_str(), i + 1, n));
        g_cache.read[i] = MeasureBW([&](size_t reps) { g_sink += k.seqRead(data.p, sz, reps); }, sz);
        g_cache.write[i] = MeasureBW([&](size_t reps) { k.seqWrite(data.p, sz, reps); }, sz);
        g_cache.writeNT[i] = MeasureBW([&](size_t reps) { k.seqWriteNT(data.p, sz, reps); }, sz);
        if (code.p && sz <= code.size && sz >= 64) {
            uint8_t* ret = code.p + sz - 8;
            uint8_t saved[8];
            memcpy(saved, ret, 8);
            ret[0] = 0xC3;  // RET
            FlushInstructionCache(GetCurrentProcess(), code.p, sz);
            typedef void (*Fn)();
            Fn fn = (Fn)(void*)code.p;
            g_cache.code[i] = MeasureBW([&](size_t reps) { for (size_t j = 0; j < reps; j++) fn(); }, sz);
            memcpy(ret, saved, 8);
            FlushInstructionCache(GetCurrentProcess(), code.p, sz);
        }
        pr.Step();
    }
    g_cache.ghzAfter = MeasureCoreGHz();
}

// ---------------------------------------------------------------- тест «Задержка»

__attribute__((noinline)) static void* Chase(void* p, size_t n16)
{
    for (; n16; --n16) {
#define C1 p = *(void**)p;
        C1 C1 C1 C1 C1 C1 C1 C1 C1 C1 C1 C1 C1 C1 C1 C1
#undef C1
    }
    return p;
}

static void RunLatency(const BenchConfig& cfg)
{
    const LogicalCpu& lc = g_cpu.cpus[cfg.cpu];
    uint64_t llc = std::max<uint64_t>(LlcFor(lc), g_cpu.maxL3);
    uint64_t maxSize = std::max<uint64_t>(Pow2Ceil(8 * llc), 64ull << 20);
    maxSize = std::min<uint64_t>(maxSize, 512ull << 20);
    maxSize = std::min<uint64_t>(maxSize, Pow2Floor(std::max<uint64_t>(AvailBytes() / 4, 64ull << 20)));

    g_bench.SetStatus(L"Задержка: выделение памяти…");
    Buf data;
    if (!data.Alloc(maxSize, cfg.largePages)) {
        g_bench.SetStatus(L"Не удалось выделить память");
        return;
    }
    std::vector<uint32_t> perm;

    LatencyResult r;
    r.valid = true;
    r.cpu = cfg.cpu;
    r.largePages = data.large;
    r.sizes = SizeList(4096, (size_t)maxSize);
    r.ns.assign(r.sizes.size(), NAN);
    PrepareSingleThread(cfg);
    r.ghz = MeasureCoreGHz();
    {
        ResultLock l;
        g_lat = r;
    }

    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&]() {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        return rng * 2685821657736338717ull;
    };
    Progress pr;
    pr.total = r.sizes.size();
    for (size_t i = 0; i < r.sizes.size() && !g_bench.stop; i++) {
        size_t sz = (size_t)r.sizes[i];
        size_t lines = sz / 64;
        g_bench.SetStatus(Fmt(L"Задержка: блок %ls (%zu из %zu)", FormatSize(sz).c_str(), i + 1, r.sizes.size()));
        // Алгоритм Саттоло: одна случайная петля через все строки кэша — префетчер не угадает
        perm.resize(lines);
        for (size_t j = 0; j < lines; j++) perm[j] = (uint32_t)j;
        for (size_t j = lines - 1; j > 0; j--) std::swap(perm[j], perm[next() % j]);
        for (size_t j = 0; j < lines; j++) *(uint8_t**)(data.p + j * 64) = data.p + (size_t)perm[j] * 64;

        void* p = data.p;
        p = Chase(p, lines / 16 + 1);  // прогрев
        double t0 = Now();
        p = Chase(p, 2048);
        double t1 = Now() - t0;
        size_t n16 = (size_t)std::max(256.0, 0.015 / std::max(t1 / 2048, 1e-12));
        double best = 1e9;
        for (int t = 0; t < 3; t++) {
            t0 = Now();
            p = Chase(p, n16);
            double dt = Now() - t0;
            best = std::min(best, dt / (n16 * 16.0) * 1e9);
        }
        g_sink += (uintptr_t)p;
        g_lat.ns[i] = best;
        pr.Step();
    }
}

// ---------------------------------------------------------------- «Чтение RAM» / «Запись RAM»

static void RunJump(const BenchConfig& cfg, bool write)
{
    KernelSet k = MakeKernels(cfg.isa >= ISA_AVX ? ISA_AVX : ISA_SSE2);
    size_t S = cfg.ramMode ? RamSpanBytes() : (4u << 20);
    size_t F = (size_t)std::max<uint64_t>(2 * g_cpu.totalL3, 32ull << 20);
    F = (F + (1 << 20) - 1) & ~(size_t)((1 << 20) - 1);

    g_bench.SetStatus(L"Выделение памяти…");
    Buf buf, flush;
    if (!buf.Alloc(S + 65536, cfg.largePages) || (cfg.ramMode && !flush.Alloc(F, false))) {
        g_bench.SetStatus(L"Не удалось выделить память");
        return;
    }

    JumpResult r;
    r.valid = true;
    r.write = write;
    r.ramMode = cfg.ramMode;
    r.cpu = cfg.cpu;
    r.span = S;
    for (int j = -3072; j <= 7168; j += 32) r.jumps.push_back(j);
    size_t nj = r.jumps.size();
    for (int b = 0; b < 4; b++) {
        r.y[b].assign(nj, NAN);
        if (write) r.yc[b].assign(nj, NAN);
        r.random[b] = r.randomC[b] = NAN;
    }
    PrepareSingleThread(cfg);
    r.ghz = MeasureCoreGHz();
    {
        ResultLock l;
        (write ? g_jwrite : g_jread) = r;
    }
    JumpResult& R = write ? g_jwrite : g_jread;

    const int trials = cfg.ramMode ? 2 : 3;
    auto doFlush = [&]() {
        if (cfg.ramMode) g_sink += k.seqRead(flush.p, F, 1);
    };

    // kind: 0 — чтение, 1 — запись NT, 2 — запись через кэш
    auto measure = [&](int b, int J, int kind) -> double {
        const size_t B = kBlockSizes[b];
        const size_t stride = B + (size_t)std::abs(J);
        const bool back = J < 0;
        const size_t r64 = (B + 63) & ~(size_t)63;
        const size_t phStep = r64 + 64;  // сдвиг фазы между проходами — новые, нетронутые строки
        size_t maxPasses = stride >= r64 + 64 ? 1 + (stride - r64 - 64) / phStep : 1;
        size_t blocks0 = std::max<size_t>(1, (S - 512) / stride);
        size_t passes;
        if (cfg.ramMode) {
            size_t need = (size_t)std::max<uint64_t>(2 * g_cpu.totalL3, 32ull << 20);
            passes = std::min(maxPasses, std::max<size_t>(1, (need + blocks0 * r64 - 1) / (blocks0 * r64)));
        } else {
            passes = std::max<size_t>(1, (8u << 20) / (blocks0 * B));
        }
        auto runPasses = [&]() -> size_t {
            size_t bytes = 0;
            for (size_t p = 0; p < passes; p++) {
                size_t ph = (p % maxPasses) * phStep;
                size_t cnt = std::max<size_t>(1, (S - 512 - ph) / stride);
                uint8_t* start = buf.p + ph + (back ? (cnt - 1) * stride : 0);
                ptrdiff_t st = back ? -(ptrdiff_t)stride : (ptrdiff_t)stride;
                if (kind == 0) g_sink += k.jumpRead[b](start, st, cnt);
                else if (kind == 1) k.jumpWriteNT[b](start, st, cnt);
                else k.jumpWrite[b](start, st, cnt);
                bytes += cnt * B;
            }
            return bytes;
        };
        double best = 0;
        if (!cfg.ramMode) runPasses();  // прогрев: 4 МБ ложатся в L3
        for (int t = 0; t < trials; t++) {
            doFlush();
            double t0 = Now();
            size_t bytes = runPasses();
            double dt = Now() - t0;
            if (dt > 0) best = std::max(best, bytes / dt);
        }
        return best / 1e9;
    };

    auto measureRandom = [&](int b, int kind) -> double {
        const size_t B = kBlockSizes[b];
        size_t mask = S / B - 1;
        size_t count = cfg.ramMode ? std::max<size_t>(1 << 18, (32u << 20) / B) : (8u << 20) / B;
        double best = 0;
        uint64_t seed = 12345;
        if (!cfg.ramMode) g_sink += k.randRead[b](buf.p, mask, count, seed);
        for (int t = 0; t < trials; t++) {
            doFlush();
            seed = seed * 31 + 7;
            double t0 = Now();
            if (kind == 0) g_sink += k.randRead[b](buf.p, mask, count, seed);
            else if (kind == 1) k.randWriteNT[b](buf.p, mask, count, seed);
            else k.randWrite[b](buf.p, mask, count, seed);
            double dt = Now() - t0;
            if (dt > 0) best = std::max(best, count * B / dt);
        }
        return best / 1e9;
    };

    Progress pr;
    pr.total = nj * 4 * (write ? 2 : 1) + 4 * (write ? 2 : 1);
    const wchar_t* what = write ? L"Запись" : L"Чтение";
    for (size_t j = 0; j < nj && !g_bench.stop; j++) {
        int J = (int)r.jumps[j];
        g_bench.SetStatus(Fmt(L"%ls: скачок %d Б (%zu из %zu)", what, J, j + 1, nj));
        for (int b = 0; b < 4; b++) {
            R.y[b][j] = measure(b, J, write ? 1 : 0);
            pr.Step();
            if (write) {
                R.yc[b][j] = measure(b, J, 2);
                pr.Step();
            }
        }
    }
    for (int b = 0; b < 4 && !g_bench.stop; b++) {
        g_bench.SetStatus(Fmt(L"%ls: случайные блоки %d Б", what, kBlockSizes[b]));
        R.random[b] = measureRandom(b, write ? 1 : 0);
        pr.Step();
        if (write) {
            R.randomC[b] = measureRandom(b, 2);
            pr.Step();
        }
    }
}

// ---------------------------------------------------------------- «Многопоток»

struct MTWorker {
    int cpu = 0;
    size_t size = 0;
    Buf buf;
    HANDLE th = nullptr, go = nullptr, done = nullptr;
    double bytes = 0, tEnd = 0;
    size_t pos = 0;
};

static volatile int g_mtCmd = 0;  // 0 — чтение, 1 — запись NT, 2 — запись, -1 — выход
static double g_mtStart = 0, g_mtEnd = 0;
static KernelSet g_mtK;

static DWORD WINAPI MTThread(LPVOID arg)
{
    MTWorker& w = *(MTWorker*)arg;
    PinCurrentThread(g_cpu.cpus[w.cpu]);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    DisablePowerThrottling();
    // первое касание — на своём ядре (важно для NUMA); при нехватке памяти — меньший буфер
    // (но не меньше половины и не меньше 16 МБ — иначе поток будет мерить кэш, а не ОЗУ)
    const size_t minSize = std::max<size_t>(w.size / 2, 16u << 20);
    while (!w.buf.Alloc(w.size, false) && w.size / 2 >= minSize) w.size /= 2;
    if (!w.buf.p) w.size = 0;
    SetEvent(w.done);
    const size_t chunk = 2u << 20;
    for (;;) {
        WaitForSingleObject(w.go, INFINITE);
        int cmd = g_mtCmd;
        if (cmd < 0) break;
        while (Now() < g_mtStart) {
        }
        double t;
        double bytes = 0;
        do {
            uint8_t* p = w.buf.p + w.pos;
            if (cmd == 0) g_sink += g_mtK.seqRead(p, chunk, 1);
            else if (cmd == 1) g_mtK.seqWriteNT(p, chunk, 1);
            else g_mtK.seqWrite(p, chunk, 1);
            bytes += chunk;
            w.pos += chunk;
            if (w.pos + chunk > w.size) w.pos = 0;
            t = Now();
        } while (t < g_mtEnd);
        w.bytes = bytes;
        w.tEnd = t;
        SetEvent(w.done);
    }
    return 0;
}

static double MTRun(std::vector<MTWorker>& ws, const std::vector<int>& idx, int cmd, double dur)
{
    g_mtCmd = cmd;
    g_mtStart = Now() + 0.004;
    g_mtEnd = g_mtStart + dur;
    for (int i : idx) SetEvent(ws[i].go);
    for (int i : idx) WaitForSingleObject(ws[i].done, INFINITE);
    // общий объём / общее время окна: поток, запущенный планировщиком с опозданием, не мерит
    // «свой» короткий отрезок в одиночку и не завышает сумму
    double bytes = 0, end = g_mtStart;
    for (int i : idx) bytes += ws[i].bytes, end = std::max(end, ws[i].tEnd);
    return end > g_mtStart ? bytes / (end - g_mtStart) / 1e9 : 0;
}

static void RunMT(const BenchConfig& cfg)
{
    g_mtK = MakeKernels(cfg.isa);
    // порядок: сначала по одному потоку на физическое ядро (P, потом E, потом LP E), затем HT-потоки
    std::vector<int> order;
    for (int pass = 0; pass < 2; pass++)
        for (auto& cl : g_cpu.clusters)
            for (int c : cl.cpus)
                if ((g_cpu.cpus[c].threadInCore == 0) == (pass == 0)) order.push_back(c);
    size_t N = order.size();

    uint64_t budget = std::min<uint64_t>(AvailBytes() / 4, 1ull << 30);
    size_t per = (size_t)std::max<uint64_t>(32ull << 20, budget / N);
    per = std::min<size_t>(per, 256u << 20) & ~(size_t)((2u << 20) - 1);
    size_t first = (size_t)std::min<uint64_t>(std::max<uint64_t>(per, Pow2Ceil(4 * g_cpu.totalL3)), std::max<uint64_t>(per, budget / 2));
    first &= ~(size_t)((2u << 20) - 1);

    g_bench.SetStatus(Fmt(L"Многопоток: запуск %zu потоков и выделение памяти…", N));
    std::vector<MTWorker> ws(N);
    for (size_t i = 0; i < N; i++) {
        ws[i].cpu = order[i];
        ws[i].size = i == 0 ? first : per;
        ws[i].go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ws[i].done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ws[i].th = CreateThread(nullptr, 0, MTThread, &ws[i], 0, nullptr);
    }
    for (auto& w : ws) WaitForSingleObject(w.done, INFINITE);

    // потоки, которым не хватило памяти, в замерах не участвуют
    std::vector<int> live;
    for (size_t i = 0; i < N; i++)
        if (ws[i].buf.p) live.push_back((int)i);
    auto stopWorkers = [&] {
        g_mtCmd = -1;
        for (auto& w : ws) SetEvent(w.go);
        for (auto& w : ws) {
            WaitForSingleObject(w.th, INFINITE);
            CloseHandle(w.th);
            CloseHandle(w.go);
            CloseHandle(w.done);
        }
    };
    if (live.empty()) {
        stopWorkers();
        g_bench.SetStatus(L"Не удалось выделить память");
        return;
    }
    if (live.size() < N) {
        std::vector<int> o;
        for (int i : live) o.push_back(order[i]);
        order = o;
        N = live.size();
    }

    MTResult r;
    r.valid = true;
    r.perThread = per;
    r.order = order;
    r.skipped = (int)(ws.size() - N);
    // при нехватке памяти буферы маленькие: если суммарный объём меньше 4×L3, заметная часть замера придётся на кэш —
    // такие точки и строки сводки пропускаются
    const uint64_t minTotal = 4 * g_cpu.totalL3;
    auto bigEnough = [&](const std::vector<int>& idx) {
        uint64_t s = 0;
        for (int i : idx) s += ws[i].size;
        return s >= minTotal;
    };
    std::vector<int> counts;
    uint64_t cum = 0;
    for (size_t n = 1; n <= N; n++) {
        cum += ws[live[n - 1]].size;
        if (cum < minTotal) {
            r.cacheOnly++;
            continue;
        }
        if (N <= 32 || n <= 8 || n == N || n % ((N + 23) / 24) == 0) counts.push_back((int)n);
    }
    for (int c : counts) r.n.push_back(c);
    r.read.assign(counts.size(), NAN);
    r.writeNT.assign(counts.size(), NAN);
    r.write.assign(counts.size(), NAN);
    {
        ResultLock l;
        g_mt = r;
    }

    // сводка: каждый кластер отдельно (по потоку на ядро), с HT, все потоки
    struct Sub { std::wstring name; std::vector<int> idx; };
    std::vector<Sub> subs;
    auto idxOf = [&](int cpu) {
        size_t k = std::find(order.begin(), order.end(), cpu) - order.begin();
        return k < N ? live[k] : -1;
    };
    for (auto& cl : g_cpu.clusters) {
        Sub s, sh;
        for (int c : cl.cpus) {
            int k = idxOf(c);
            if (k < 0) continue;
            if (g_cpu.cpus[c].threadInCore == 0) s.idx.push_back(k);
            sh.idx.push_back(k);
        }
        if (s.idx.empty()) continue;
        s.name = Fmt(L"%ls × %zu (по потоку на ядро)", cl.name.c_str(), s.idx.size());
        subs.push_back(s);
        if (sh.idx.size() > s.idx.size()) {
            sh.name = Fmt(L"%ls × %zu + HT (%zu пот.)", cl.name.c_str(), s.idx.size(), sh.idx.size());
            subs.push_back(sh);
        }
    }
    Progress pr;
    pr.total = counts.size() * 3 + subs.size() * 3;
    const double dur = 0.12;
    for (size_t i = 0; i < counts.size() && !g_bench.stop; i++) {
        std::vector<int> idx;
        for (int j = 0; j < counts[i]; j++) idx.push_back(live[j]);
        g_bench.SetStatus(Fmt(L"Многопоток: %d из %zu потоков", counts[i], N));
        g_mt.read[i] = MTRun(ws, idx, 0, dur);
        pr.Step();
        g_mt.writeNT[i] = MTRun(ws, idx, 1, dur);
        pr.Step();
        g_mt.write[i] = MTRun(ws, idx, 2, dur);
        pr.Step();
    }
    std::vector<MTSummary> sum;
    if (!g_bench.stop && !counts.empty() && counts[0] == 1) {
        MTSummary s1;
        s1.name = Fmt(L"1 поток (%ls)", g_cpu.clusters.empty() ? L"ядро" : g_cpu.clusters[0].name.c_str());
        s1.threads = 1;
        s1.read = g_mt.read[0];
        s1.writeNT = g_mt.writeNT[0];
        s1.write = g_mt.write[0];
        sum.push_back(s1);
    }
    for (auto& s : subs) {
        if (g_bench.stop) break;
        if (!bigEnough(s.idx)) continue;
        g_bench.SetStatus(L"Многопоток: " + s.name);
        MTSummary m;
        m.name = s.name;
        m.threads = (int)s.idx.size();
        m.read = MTRun(ws, s.idx, 0, dur);
        pr.Step();
        m.writeNT = MTRun(ws, s.idx, 1, dur);
        pr.Step();
        m.write = MTRun(ws, s.idx, 2, dur);
        pr.Step();
        sum.push_back(m);
    }
    if (!g_bench.stop && !counts.empty() && counts.back() == (int)N && g_cpu.clusters.size() > 1) {
        MTSummary a;
        a.name = Fmt(L"Все потоки (%zu)", N);
        a.threads = (int)N;
        a.read = g_mt.read.back();
        a.writeNT = g_mt.writeNT.back();
        a.write = g_mt.write.back();
        sum.push_back(a);
    }
    {
        ResultLock l;
        g_mt.summary = sum;
    }

    stopWorkers();
}

void RunTest(TestKind kind, const BenchConfig& cfg)
{
    switch (kind) {
    case TEST_CACHE: RunCache(cfg); break;
    case TEST_LATENCY: RunLatency(cfg); break;
    case TEST_READ: RunJump(cfg, false); break;
    case TEST_WRITE: RunJump(cfg, true); break;
    case TEST_MT: RunMT(cfg); break;
    default: break;
    }
}
