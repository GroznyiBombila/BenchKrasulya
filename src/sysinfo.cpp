// BenchKrasulya — сведения о процессоре, кэшах и памяти
#include "sysinfo.h"
#include <cpuid.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#ifdef BK_EMU
#include "../tests/emu.h"
#endif

std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}

std::wstring Widen(const std::string& s)
{
    std::wstring w;
    for (unsigned char c : s) w += (c >= 32 && c < 127) ? (wchar_t)c : L'?';
    return w;
}

static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') i++;
    return s.substr(i);
}

std::wstring FormatSize(uint64_t b)
{
    if (b == 0) return L"—";
    auto num = [](double v) {
        wchar_t t[32];
        if (v == (double)(uint64_t)v) swprintf(t, 32, L"%.0f", v);
        else swprintf(t, 32, L"%.2g", v);
        if (v >= 10 && v != (double)(uint64_t)v) swprintf(t, 32, L"%.1f", v);
        return std::wstring(t);
    };
    if (b >= (1ull << 30)) return num(b / 1073741824.0) + L" ГБ";
    if (b >= (1ull << 20)) return num(b / 1048576.0) + L" МБ";
    if (b >= 1024) return num(b / 1024.0) + L" КБ";
    return num((double)b) + L" Б";
}

std::wstring FormatSizeAxis(double b)
{
    wchar_t t[32];
    if (b >= 1073741824.0) swprintf(t, 32, L"%gГ", b / 1073741824.0);
    else if (b >= 1048576.0) swprintf(t, 32, L"%gМ", b / 1048576.0);
    else if (b >= 1024.0) swprintf(t, 32, L"%gК", b / 1024.0);
    else swprintf(t, 32, L"%g", b);
    return t;
}

// ---------------------------------------------------------------- CPUID

static void Cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
#ifdef BK_EMU
    if (EmuCpuid(leaf, sub, r)) return;
#endif
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
}

static uint64_t Xgetbv0()
{
    unsigned a, d;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    return ((uint64_t)d << 32) | a;
}

static double MeasureTscGHz()
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    uint64_t t0 = __builtin_ia32_rdtsc();
    do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) < f.QuadPart / 20);  // 50 мс
    uint64_t t1 = __builtin_ia32_rdtsc();
    double sec = double(b.QuadPart - a.QuadPart) / f.QuadPart;
    return (t1 - t0) / sec / 1e9;
}

static bool ReadTopology(std::vector<BYTE>& buf)
{
#ifdef BK_EMU
    bool ok;
    if (EmuGlpi(buf, ok)) return ok;
#endif
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    buf.resize(len);
    if (!len || !GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) return false;
    buf.resize(len);
    return true;
}

// Windows не отдала топологию: каждый логический CPU считаем отдельным ядром, кэши неизвестны
static void FallbackTopology(CpuInfo& ci)
{
    ci.cpus.clear();
    WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < std::max<WORD>(groups, 1); g++) {
        DWORD n = std::min<DWORD>(GetActiveProcessorCount(g), 64);
        for (DWORD b = 0; b < n; b++) {
            LogicalCpu c;
            c.group = g;
            c.number = (BYTE)b;
            c.core = (int)ci.cpus.size();
            ci.cpus.push_back(c);
        }
    }
    if (ci.cpus.empty()) ci.cpus.push_back(LogicalCpu{});
    ci.cpus[0].core = 0;
    ci.physCores = (int)ci.cpus.size();
    ci.topologyUnknown = true;
}

void QueryCpuInfo(CpuInfo& ci)
{
    unsigned r[4];
    Cpuid(0, 0, r);
    unsigned maxLeaf = r[0];
    char vend[13] = {};
    memcpy(vend, &r[1], 4);
    memcpy(vend + 4, &r[3], 4);
    memcpy(vend + 8, &r[2], 4);
    ci.vendor = Widen(vend);

    Cpuid(1, 0, r);
    unsigned fam = (r[0] >> 8) & 0xF, mod = (r[0] >> 4) & 0xF;
    if (fam == 0xF) fam += (r[0] >> 20) & 0xFF;
    if (fam == 0x6 || fam >= 0xF) mod += ((r[0] >> 16) & 0xF) << 4;
    ci.family = fam;
    ci.model = mod;
    ci.stepping = r[0] & 0xF;
    ci.sse2 = (r[3] >> 26) & 1;
    ci.hypervisor = (r[2] >> 31) & 1;
    bool osxsave = (r[2] >> 27) & 1, avxBit = (r[2] >> 28) & 1;
    uint64_t xcr0 = osxsave ? Xgetbv0() : 0;
    bool osYmm = (xcr0 & 0x6) == 0x6, osZmm = (xcr0 & 0xE6) == 0xE6;
    ci.avx = avxBit && osYmm;
    if (maxLeaf >= 7) {
        Cpuid(7, 0, r);
        ci.avx2 = ci.avx && ((r[1] >> 5) & 1);
        ci.avx512 = osZmm && ((r[1] >> 16) & 1);
        ci.hybrid = (r[3] >> 15) & 1;
    }
    Cpuid(0x80000000, 0, r);
    unsigned maxExt = r[0];
    if (maxExt >= 0x80000004) {
        char brand[49] = {};
        for (unsigned i = 0; i < 3; i++) {
            Cpuid(0x80000002 + i, 0, r);
            memcpy(brand + i * 16, r, 16);
        }
        ci.brand = Trim(Widen(brand));
    }
    if (maxExt >= 0x80000007) {
        Cpuid(0x80000007, 0, r);
        ci.invariantTsc = (r[3] >> 8) & 1;
    }
    ci.tscGHz = MeasureTscGHz();

    // ---- топология и кэши от Windows (знает про P/E-ядра и все уровни кэша)
    std::vector<BYTE> buf;
    if (!ReadTopology(buf)) {
        FallbackTopology(ci);
        return;
    }
    DWORD len = (DWORD)buf.size();

    std::map<std::pair<int, int>, int> idx;  // (group, number) -> индекс в ci.cpus
    int coreNo = 0;
    for (DWORD off = 0; off + 8 <= len;) {
        auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (p->Size < 8 || off + p->Size > len) break;
        if (p->Relationship == RelationProcessorCore) {
            auto& pr = p->Processor;
            int nThreads = 0;
            for (WORD g = 0; g < pr.GroupCount; g++)
                nThreads += __builtin_popcountll(pr.GroupMask[g].Mask);
            int t = 0;
            for (WORD g = 0; g < pr.GroupCount; g++)
                for (int b = 0; b < 64; b++)
                    if (pr.GroupMask[g].Mask & (1ull << b)) {
                        LogicalCpu c;
                        c.group = pr.GroupMask[g].Group;
                        c.number = (BYTE)b;
                        c.core = coreNo;
                        c.threadInCore = t++;
                        c.threadsInCore = nThreads;
                        c.effClass = pr.EfficiencyClass;
                        idx[{c.group, c.number}] = (int)ci.cpus.size();
                        ci.cpus.push_back(c);
                    }
            coreNo++;
        }
        off += p->Size;
    }
    ci.physCores = coreNo;
    if (ci.cpus.empty()) {
        FallbackTopology(ci);
        return;
    }

    std::set<std::pair<WORD, KAFFINITY>> l3seen;
    for (DWORD off = 0; off + 8 <= len;) {
        auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (p->Size < 8 || off + p->Size > len) break;
        if (p->Relationship == RelationCache) {
            auto& c = p->Cache;
            WORD gc = c.GroupCount ? c.GroupCount : 1;
            for (WORD g = 0; g < gc; g++) {
                const GROUP_AFFINITY& ga = c.GroupMasks[g];
                CacheInfo inf;
                inf.size = c.CacheSize;
                inf.line = c.LineSize;
                inf.sharedBy = __builtin_popcountll(ga.Mask);
                std::set<int> cores;
                for (int b = 0; b < 64; b++) {
                    auto it = (ga.Mask & (1ull << b)) ? idx.find({ga.Group, b}) : idx.end();
                    if (it != idx.end()) cores.insert(ci.cpus[it->second].core);
                }
                inf.sharedCores = (uint32_t)cores.size();
                if (c.Level == 3 && l3seen.insert({ga.Group, ga.Mask}).second) {
                    ci.totalL3 += c.CacheSize;
                    ci.maxL3 = std::max<uint64_t>(ci.maxL3, c.CacheSize);
                }
                for (int b = 0; b < 64; b++) {
                    if (!(ga.Mask & (1ull << b))) continue;
                    auto it = idx.find({ga.Group, b});
                    if (it == idx.end()) continue;
                    LogicalCpu& lc = ci.cpus[it->second];
                    if (c.Level == 1 && c.Type == CacheData) lc.l1d = inf;
                    else if (c.Level == 1 && c.Type == CacheInstruction) lc.l1i = inf;
                    else if (c.Level == 1 && c.Type == CacheUnified) lc.l1d = inf;
                    else if (c.Level == 2) lc.l2 = inf;
                    else if (c.Level == 3) lc.l3 = inf;
                }
            }
        }
        off += p->Size;
    }
}

bool PinCurrentThread(const LogicalCpu& c)
{
#ifdef BK_EMU
    bool ok;
    if (EmuPin(c, ok)) return ok;
#endif
    GROUP_AFFINITY ga = {};
    ga.Group = c.group;
    ga.Mask = (KAFFINITY)1 << c.number;
    if (!SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr)) return false;
    Sleep(0);  // дать планировщику перенести поток
    return true;
}

void DisablePowerThrottling()
{
    // Windows 11 по умолчанию может отправлять фоновые/«экономные» потоки на E-ядра
    // и снижать им частоту (EcoQoS). Явно просим высокий QoS.
    typedef BOOL(WINAPI * PFN)(HANDLE, int, LPVOID, DWORD);
    static PFN fn = (PFN)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation");
    static PFN fnP = (PFN)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    struct { ULONG Version, ControlMask, StateMask; } st = {1, 1, 0};
    if (fn) fn(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &st, sizeof st);
    if (fnP) fnP(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &st, sizeof st);
}

static DWORD WINAPI CoreTypeThread(LPVOID arg)
{
    CpuInfo& ci = *(CpuInfo*)arg;
    unsigned r[4];
    Cpuid(0, 0, r);
    bool has1A = r[0] >= 0x1A;
    for (auto& c : ci.cpus) {
        if (!ci.hybrid || !has1A) break;
        if (!PinCurrentThread(c)) continue;
        Sleep(1);
        Cpuid(0x1A, 0, r);
        c.hybridType = (r[0] >> 24) & 0xFF;
    }
    return 0;
}

// Под гипервизором (Hyper-V/VBS, «изоляция ядра» Windows) CPUID 4 перехватывается, и Windows
// приписывает E-ядрам кэши P-ядра. Если E-ядро «получило» ровно кэши P-ядра — берём размеры
// из справочника микроархитектур Intel.
struct ECoreCaches {
    int model;
    uint32_t l1d, l1i, l2;  // КБ; L2 — на модуль из 4 E-ядер
};
static const ECoreCaches kECore[] = {
    {0x97, 32, 64, 2048}, {0x9A, 32, 64, 2048},                         // Alder Lake-S/P/H (Gracemont)
    {0xB7, 32, 64, 4096}, {0xBA, 32, 64, 2048}, {0xBF, 32, 64, 2048},   // Raptor Lake (Gracemont)
    {0xAA, 32, 64, 2048}, {0xAC, 32, 64, 2048}, {0xB5, 32, 64, 2048},   // Meteor Lake (Crestmont)
    {0xBD, 32, 64, 4096}, {0xC5, 32, 64, 4096}, {0xC6, 32, 64, 4096},   // Lunar/Arrow Lake (Skymont)
    {0xCC, 32, 64, 4096},                                               // Panther Lake (Darkmont)
};

static void FixHybridCaches(CpuInfo& ci)
{
    if (!ci.hybrid || ci.vendor != L"GenuineIntel" || ci.family != 6) return;
    const LogicalCpu* p = nullptr;
    for (auto& c : ci.cpus)
        if (c.hybridType == 0x40) { p = &c; break; }
    const ECoreCaches* t = nullptr;
    for (auto& e : kECore)
        if (e.model == ci.model) t = &e;
    if (!p || !t) return;
    for (auto& c : ci.cpus) {
        if (c.hybridType != 0x20) continue;
        if (c.l1d.size != p->l1d.size || c.l1i.size != p->l1i.size || c.l2.size != p->l2.size) continue;
        c.l1d.size = t->l1d << 10;
        c.l1i.size = t->l1i << 10;
        c.l2.size = (uint64_t)t->l2 << 10;
        ci.eCacheFromTable = true;
    }
}

void DetectCoreTypes(CpuInfo& ci)
{
    HANDLE h = CreateThread(nullptr, 0, CoreTypeThread, &ci, 0, nullptr);
    if (h) {
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
    FixHybridCaches(ci);
    // ---- кластеры: группируем ядра одного типа
    bool anyL3 = false;
    std::set<int> effClasses;
    for (auto& c : ci.cpus) {
        if (c.l3.size) anyL3 = true;
        effClasses.insert(c.effClass);
    }
    std::map<std::wstring, int> byKey;
    for (int i = 0; i < (int)ci.cpus.size(); i++) {
        LogicalCpu& c = ci.cpus[i];
        std::wstring name, sn;
        int rank;
        if (ci.hybrid && c.hybridType == 0x40) {
            name = L"P-ядро"; sn = L"P"; rank = 0;
        } else if (ci.hybrid && c.hybridType == 0x20 && (c.l3.size || !anyL3)) {
            name = L"E-ядро"; sn = L"E"; rank = 1;
        } else if (ci.hybrid && c.hybridType == 0x20) {
            name = L"LP E-ядро"; sn = L"LPE"; rank = 2;  // Core Ultra: ядра SoC-тайла без L3
        } else if (effClasses.size() == 2) {
            // AMD Zen 5 + Zen 5c (Ryzen AI), Intel без CPUID 1Ah (в ВМ): два класса по данным Windows
            bool fast = c.effClass == *effClasses.rbegin();
            name = fast ? L"P-ядро" : L"E-ядро";
            if (ci.vendor == L"AuthenticAMD") name = fast ? L"Zen-ядро" : L"Zen c-ядро";
            sn = fast ? L"P" : L"E";
            rank = fast ? 0 : 1;
        } else if (effClasses.size() > 2) {
            int maxCls = *effClasses.rbegin();
            rank = maxCls - c.effClass;
            name = Fmt(L"Ядро класса %d", c.effClass);
            sn = Fmt(L"C%d", c.effClass);
        } else {
            name = L"Ядро"; sn = L"C"; rank = 0;
        }
        auto it = byKey.find(name);
        int cl;
        if (it == byKey.end()) {
            CpuCluster k;
            k.name = name;
            k.shortName = sn;
            k.rank = rank;
            k.rep = i;
            k.l1d = c.l1d; k.l1i = c.l1i; k.l2 = c.l2; k.l3 = c.l3;
            cl = (int)ci.clusters.size();
            ci.clusters.push_back(k);
            byKey[name] = cl;
        } else cl = it->second;
        c.cluster = cl;
        ci.clusters[cl].cpus.push_back(i);
        if (c.threadInCore == 0) ci.clusters[cl].cores++;
    }
    // сортировка кластеров: P, E, LPE
    std::vector<int> order(ci.clusters.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return ci.clusters[a].rank < ci.clusters[b].rank; });
    std::vector<CpuCluster> sorted;
    std::vector<int> remap(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        remap[order[i]] = (int)i;
        sorted.push_back(ci.clusters[order[i]]);
    }
    ci.clusters = sorted;
    for (auto& c : ci.cpus) c.cluster = remap[c.cluster];
}

// ---------------------------------------------------------------- SMBIOS

static const wchar_t* MemTypeName(int t)
{
    switch (t) {
    case 0x0F: return L"SDRAM";
    case 0x12: return L"DDR";
    case 0x13: return L"DDR2";
    case 0x14: return L"DDR2 FB";
    case 0x18: return L"DDR3";
    case 0x19: return L"FBD2";
    case 0x1A: return L"DDR4";
    case 0x1B: return L"LPDDR";
    case 0x1C: return L"LPDDR2";
    case 0x1D: return L"LPDDR3";
    case 0x1E: return L"LPDDR4";
    case 0x20: return L"HBM";
    case 0x21: return L"HBM2";
    case 0x22: return L"DDR5";
    case 0x23: return L"LPDDR5";
    case 0x24: return L"HBM3";
    case 0x07: return L"RAM";
    case 0x02: return L"неизвестно";
    default: return L"другое";
    }
}

// Пытаемся вытащить «контроллер:канал» из строк локатора:
// "Controller0-ChannelA-DIMM0", "P0 CHANNEL A", "ChannelB-DIMM1", "DIMM_A1", "CHA_DIMM0"...
static std::wstring ChannelKey(const std::wstring& a, const std::wstring& b)
{
    std::wstring s = a + L" " + b;
    for (auto& ch : s) ch = towupper(ch);
    std::wstring ctrl = L"0", chn;
    size_t p = s.find(L"CONTROLLER");
    if (p != std::wstring::npos && p + 10 < s.size()) ctrl = s.substr(p + 10, 1);
    else if ((p = s.find(L"P0 ")) != std::wstring::npos || (p = s.find(L"P1 ")) != std::wstring::npos)
        ctrl = s.substr(p + 1, 1);
    else {
        // серверные платы: «NODE1», «CPU2_DIMM_A1», «SOCKET 1», «P2-DIMMA1» — номер процессора
        for (const wchar_t* pat : {L"NODE", L"CPU", L"SOCKET", L"P"}) {
            size_t q = s.find(pat);
            while (q != std::wstring::npos) {
                size_t d = q + wcslen(pat);
                while (d < s.size() && (s[d] == L' ' || s[d] == L'_')) d++;
                bool word = q == 0 || !iswalpha(s[q - 1]);
                if (word && d < s.size() && iswdigit(s[d]) && (d + 1 >= s.size() || !iswdigit(s[d + 1]))) {
                    ctrl = std::wstring(pat) + s.substr(d, 1);
                    break;
                }
                q = s.find(pat, q + 1);
            }
            if (ctrl != L"0") break;
        }
    }
    p = s.find(L"CHANNEL");
    if (p != std::wstring::npos) {
        size_t q = p + 7;
        while (q < s.size() && (s[q] == L' ' || s[q] == L'_' || s[q] == L'-')) q++;
        if (q < s.size() && iswalnum(s[q])) chn = s.substr(q, 1);
    }
    if (chn.empty()) {
        for (const wchar_t* pat : {L"CH_", L"CH-", L"CH "}) {
            p = s.find(pat);
            if (p != std::wstring::npos && p + 3 < s.size() && iswalnum(s[p + 3])) { chn = s.substr(p + 3, 1); break; }
        }
    }
    if (chn.empty()) {
        p = s.find(L"CH");
        if (p != std::wstring::npos && p + 2 < s.size() && s[p + 2] >= L'A' && s[p + 2] <= L'H' &&
            (p + 3 >= s.size() || !iswalpha(s[p + 3])))
            chn = s.substr(p + 2, 1);
    }
    if (chn.empty()) {
        p = s.find(L"DIMM");
        while (p != std::wstring::npos) {
            size_t q = p + 4;
            while (q < s.size() && (s[q] == L'_' || s[q] == L' ' || s[q] == L'-')) q++;
            if (q + 1 < s.size() && s[q] >= L'A' && s[q] <= L'H' && iswdigit(s[q + 1])) { chn = s.substr(q, 1); break; }
            p = s.find(L"DIMM", p + 4);
        }
    }
    if (chn.empty()) return L"";
    return ctrl + L":" + chn;
}

static bool ReadSmbios(std::vector<BYTE>& buf)
{
#ifdef BK_EMU
    bool ok;
    if (EmuSmbios(buf, ok)) return ok;
#endif
    const DWORD RSMB = 'R' << 24 | 'S' << 16 | 'M' << 8 | 'B';
    UINT sz = GetSystemFirmwareTable(RSMB, 0, nullptr, 0);
    if (!sz) return false;
    buf.resize(sz);
    return GetSystemFirmwareTable(RSMB, 0, buf.data(), sz) == sz;
}

void QueryMemInfo(MemInfo& mi)
{
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    mi.totalMB = ms.ullTotalPhys >> 20;
    mi.availMB = ms.ullAvailPhys >> 20;

#ifdef BK_EMU
    if (uint64_t t = EmuTotalMB()) mi.totalMB = t;
#endif
    std::vector<BYTE> buf;
    if (!ReadSmbios(buf) || buf.size() < 8) return;
    size_t sz = buf.size();
    mi.smbiosOk = true;
    mi.smbiosMajor = buf[1];
    mi.smbiosMinor = buf[2];
    DWORD tlen = *(DWORD*)&buf[4];
    const BYTE* p = buf.data() + 8;
    const BYTE* end = p + std::min<size_t>(tlen, sz - 8);

    while (p + 4 <= end) {
        BYTE type = p[0], flen = p[1];
        if (flen < 4 || p + flen > end) break;
        std::vector<std::string> strs;
        const char* sp = (const char*)(p + flen);
        while ((const BYTE*)sp < end && *sp) {
            std::string str(sp, strnlen(sp, (const char*)end - sp));
            strs.push_back(str);
            sp += str.size() + 1;
        }
        const BYTE* next = strs.empty() ? p + flen + 2 : (const BYTE*)sp + 1;
        auto S = [&](int off) -> std::wstring {
            if (off >= flen) return L"";
            int i = p[off];
            return (i > 0 && i <= (int)strs.size()) ? Trim(Widen(strs[i - 1])) : L"";
        };
        auto W = [&](int off) -> unsigned { return off + 1 < flen ? *(const WORD*)(p + off) : 0; };
        auto D = [&](int off) -> unsigned { return off + 3 < flen ? *(const DWORD*)(p + off) : 0; };

        if (type == 17 && flen >= 0x15) {
            unsigned size = W(0x0C);
            if (size != 0) {
                MemModule m;
                if (size == 0x7FFF) m.sizeMB = D(0x1C) & 0x7FFFFFFF;
                else if (size != 0xFFFF) m.sizeMB = (size & 0x8000) ? (size & 0x7FFF) / 1024 : size;
                m.totalWidth = W(0x08) == 0xFFFF ? 0 : W(0x08);
                m.dataWidth = W(0x0A) == 0xFFFF ? 0 : W(0x0A);
                m.formFactor = p[0x0E];
                m.locator = S(0x10);
                m.bank = S(0x11);
                m.type = p[0x12];
                m.typeName = MemTypeName(m.type);
                m.speed = W(0x15);
                if (m.speed == 0xFFFF) m.speed = D(0x54);
                m.manufacturer = S(0x17);
                m.part = S(0x1A);
                if (flen > 0x1B) m.ranks = p[0x1B] & 0x0F;
                m.confSpeed = W(0x20);
                if (m.confSpeed == 0xFFFF) m.confSpeed = D(0x58);
                // До SMBIOS 3.1 поле «Speed» было в МГц, и старые BIOS (DDR/DDR2/DDR3) пишут
                // реальную частоту шины, а не МТ/с: DDR2-800 → 400. Удваиваем, если значение
                // не выше минимальной стандартной скорости этого типа: сами DDR-200, DDR2-400,
                // DDR3-800, DDR4-1600 почти не встречались, а их «удвоенные» варианты — массовые.
                unsigned minMT = 0;
                switch (m.type) {
                case 0x12: minMT = 200; break;                      // DDR
                case 0x13: case 0x14: minMT = 400; break;           // DDR2
                case 0x18: case 0x1D: minMT = 800; break;           // DDR3, LPDDR3
                case 0x1A: case 0x1E: minMT = 1600; break;          // DDR4, LPDDR4
                case 0x22: case 0x23: minMT = 3200; break;          // DDR5, LPDDR5
                }
                if (m.speed && m.speed <= minMT) m.rawSpeed = m.speed, m.speed *= 2;
                if (m.confSpeed && m.confSpeed <= minMT) m.rawSpeed = m.rawSpeed ? m.rawSpeed : m.confSpeed, m.confSpeed *= 2;
                m.voltage_mV = W(0x26);
                m.channelKey = ChannelKey(m.locator, m.bank);
                mi.modules.push_back(m);
            }
        }
        if (type == 127) break;
        p = next;
    }

    if (mi.modules.empty()) return;
    // общие характеристики
    std::map<std::wstring, int> widthByChannel;
    bool allKeys = true;
    for (auto& m : mi.modules) {
        uint32_t s = m.confSpeed ? m.confSpeed : m.speed;
        if (!mi.speedMT || (s && s < mi.speedMT)) mi.speedMT = s;  // общая скорость = по самому медленному
        if (mi.typeName.empty()) mi.typeName = m.typeName;
        if (m.channelKey.empty()) allKeys = false;
        else widthByChannel[m.channelKey] = std::max(widthByChannel[m.channelKey], m.dataWidth ? m.dataWidth : 64);
    }
    bool lp = mi.modules[0].type == 0x1B || mi.modules[0].type == 0x1C || mi.modules[0].type == 0x1D ||
              mi.modules[0].type == 0x1E || mi.modules[0].type == 0x23;
    if (allKeys) {
        mi.channels = (int)widthByChannel.size();
        for (auto& kv : widthByChannel) mi.busBits += kv.second;
        mi.channelNote = L"по названиям слотов (локаторам) SMBIOS";
    } else if (lp) {
        int sum = 0;
        for (auto& m : mi.modules) sum += m.dataWidth ? m.dataWidth : 16;
        mi.busBits = std::min(sum, 256);
        mi.channels = (int)mi.modules.size();
        mi.channelNote = L"LPDDR: сумма ширины всех устройств (оценка)";
    } else {
        int n = (int)mi.modules.size();
        mi.channels = n >= 2 ? 2 : 1;  // типичная настольная/ноутбучная платформа — 2 канала
        mi.busBits = mi.channels * 64;
        mi.channelNote = L"локаторы не распознаны — принято 2 канала, если модулей ≥ 2 (оценка)";
    }
    mi.peakGBs = mi.speedMT * (mi.busBits / 8.0) / 1000.0;
}

std::wstring OsVersionString()
{
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto fn = (RtlGetVersionPtr)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW v = {sizeof v};
    if (fn) fn(&v);
    std::wstring name = v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10")
                                               : Fmt(L"Windows %lu.%lu", v.dwMajorVersion, v.dwMinorVersion);
    return Fmt(L"%ls (сборка %lu)", name.c_str(), v.dwBuildNumber);
}
