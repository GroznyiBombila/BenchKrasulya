// BenchKrasulya — эмуляция чужого железа для проверки (только тестовая сборка, -DBK_EMU).
// Профили собраны по документации Intel/AMD (размеры кэшей, CPUID) и по реальным дампам SMBIOS
// типичных плат (названия слотов, поля type 17 разных версий спецификации).
#include "emu.h"
#include "../src/sysinfo.h"
#include <cpuid.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>

namespace {

struct Block {        // группа одинаковых ядер
    int cores, threads;
    int type;         // CPUID 1Ah: 0x40 Core, 0x20 Atom, 0 — нет
    int eff;          // EfficiencyClass
    int l1d, l1i, l2; // КБ
    int l2share;      // сколько ядер делят L2
    int l3;           // номер L3 из Profile::l3KB, -1 — нет L3
};

struct Dimm {
    const char *loc, *bank;
    int sizeMB, type, speed, conf, dw, tw;
    const char *manuf, *part;
    int ranks;
};

struct Profile {
    const wchar_t* name;
    const wchar_t* desc;
    const char* vendor;
    const char* brand;
    int family, model, stepping;
    unsigned maxLeaf;
    bool avx, avx2, avx512, hybrid, hv;
    std::vector<Block> blocks;
    std::vector<int> l3KB;
    int smbMaj, smbMin, t17len;  // smbMaj = 0 — SMBIOS нет
    std::vector<Dimm> dimms;
    bool win7Glpi = false, glpiFail = false, smbCorrupt = false, pkg2 = false;
};

const char* SAM = "Samsung";
const char* KNG = "Kingston";
const char* MIC = "Micron Technology";
const char* HYN = "SK Hynix";

std::vector<Profile> MakeProfiles()
{
    std::vector<Profile> v;
    // Core 2 Duo E8400 + DDR2-800, SMBIOS 2.5, частота в МГц (400), Windows 7: GroupCount кэша = 0
    v.push_back({L"core2-ddr2", L"Intel Core 2 Duo E8400 + 2× DDR2-800 (SMBIOS 2.5, частота в МГц)", "GenuineIntel",
                 "Intel(R) Core(TM)2 Duo CPU     E8400  @ 3.00GHz", 6, 0x17, 10, 0xD, false, false, false, false, false,
                 {{2, 1, 0, 0, 32, 32, 6144, 2, -1}}, {}, 2, 5, 0x1B,
                 {{"DIMM0", "BANK0", 2048, 0x13, 400, 0, 64, 64, KNG, "KHX6400D2/2G", 0},
                  {"DIMM1", "BANK1", 2048, 0x13, 400, 0, 64, 64, KNG, "KHX6400D2/2G", 0},
                  {"DIMM2", "BANK2", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"DIMM3", "BANK3", 0, 0x02, 0, 0, 0, 0, "", "", 0}}});
    v.back().win7Glpi = true;
    // Athlon 64 (Socket 754): CPUID только до листа 1, один канал DDR-400, SMBIOS 2.3
    v.push_back({L"athlon64-ddr", L"AMD Athlon 64 3200+ (1 ядро, без L3) + DDR-400, SMBIOS 2.3", "AuthenticAMD",
                 "AMD Athlon(tm) 64 Processor 3200+", 0xF, 0x2F, 2, 1, false, false, false, false, false,
                 {{1, 1, 0, 0, 64, 64, 512, 1, -1}}, {}, 2, 3, 0x1B,
                 {{"DIMM0", "", 512, 0x12, 200, 0, 64, 64, "", "", 0}, {"DIMM1", "", 0, 0x02, 0, 0, 0, 0, "", "", 0}}});
    v.back().win7Glpi = true;
    // Ivy Bridge + DDR3: частота модулей 1600, настроено 1333
    v.push_back({L"i7-3770-ddr3", L"Intel Core i7-3770 + 4× DDR3 (1600, работают на 1333)", "GenuineIntel",
                 "Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz", 6, 0x3A, 9, 0xD, true, false, false, false, false,
                 {{4, 2, 0, 0, 32, 32, 256, 1, 0}}, {8192}, 2, 7, 0x22,
                 {{"ChannelA-DIMM0", "BANK 0", 4096, 0x18, 1600, 1333, 64, 64, KNG, "KHX1600C9D3/4G", 2},
                  {"ChannelA-DIMM1", "BANK 1", 4096, 0x18, 1600, 1333, 64, 64, KNG, "KHX1600C9D3/4G", 2},
                  {"ChannelB-DIMM0", "BANK 2", 4096, 0x18, 1600, 1333, 64, 64, SAM, "M378B5273DH0-CK0", 2},
                  {"ChannelB-DIMM1", "BANK 3", 4096, 0x18, 1600, 1333, 64, 64, SAM, "M378B5273DH0-CK0", 2}}});
    // Skylake, один модуль DDR4 — одноканальный режим
    v.push_back({L"i7-6700k-ddr4-1ch", L"Intel Core i7-6700K + 1× DDR4-2133 (один канал)", "GenuineIntel",
                 "Intel(R) Core(TM) i7-6700K CPU @ 4.00GHz", 6, 0x5E, 3, 0x16, true, true, false, false, false,
                 {{4, 2, 0, 0, 32, 32, 256, 1, 0}}, {8192}, 3, 0, 0x28,
                 {{"ChannelA-DIMM0", "BANK 0", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"ChannelA-DIMM1", "BANK 1", 8192, 0x1A, 2133, 2133, 64, 64, HYN, "HMA81GU6AFR8N-UH", 1},
                  {"ChannelB-DIMM0", "BANK 2", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"ChannelB-DIMM1", "BANK 3", 0, 0x02, 0, 0, 0, 0, "", "", 0}}});
    // Raptor Lake: 8P+16E, модули DDR5 по 48 ГБ (размер в расширенном поле SMBIOS)
    v.push_back({L"i9-13900k-ddr5", L"Intel Core i9-13900K (8P+16E) + 2× DDR5-5600 48 ГБ", "GenuineIntel",
                 "13th Gen Intel(R) Core(TM) i9-13900K", 6, 0xB7, 1, 0x20, true, true, false, true, false,
                 {{8, 2, 0x40, 1, 48, 32, 2048, 1, 0}, {16, 1, 0x20, 0, 32, 64, 4096, 4, 0}}, {36864}, 3, 5, 0x5C,
                 {{"Controller0-ChannelA-DIMM0", "BANK 0", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"Controller0-ChannelA-DIMM1", "BANK 0", 49152, 0x22, 5600, 5600, 64, 64, "Corsair", "CMK96GX5M2B5600C40", 2},
                  {"Controller1-ChannelA-DIMM0", "BANK 0", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"Controller1-ChannelA-DIMM1", "BANK 0", 49152, 0x22, 5600, 5600, 64, 64, "Corsair", "CMK96GX5M2B5600C40", 2}}});
    // Meteor Lake: P + E + LP E (SoC-тайл, без L3), LPDDR5X — 8 устройств по 16 бит
    {
        Profile p = {L"ultra7-155h", L"Intel Core Ultra 7 155H (6P+8E+2LPE) + LPDDR5X-7467 16 ГБ", "GenuineIntel",
                     "Intel(R) Core(TM) Ultra 7 155H", 6, 0xAA, 4, 0x23, true, true, false, true, false,
                     {{6, 2, 0x40, 1, 48, 64, 2048, 1, 0}, {8, 1, 0x20, 0, 32, 64, 2048, 4, 0}, {2, 1, 0x20, 0, 32, 64, 2048, 2, -1}},
                     {24576}, 3, 4, 0x5C, {}};
        const char* locs[] = {"Controller0-ChannelA-DIMM0", "Controller0-ChannelB-DIMM0", "Controller0-ChannelC-DIMM0", "Controller0-ChannelD-DIMM0",
                              "Controller1-ChannelA-DIMM0", "Controller1-ChannelB-DIMM0", "Controller1-ChannelC-DIMM0", "Controller1-ChannelD-DIMM0"};
        for (auto l : locs) p.dimms.push_back({l, "BANK 0", 2048, 0x23, 7467, 7467, 16, 16, MIC, "MT62F1G32D2DS-023 WT:B", 1});
        v.push_back(p);
    }
    // Lunar Lake: E-ядра Skymont без L3 (Intel называет их LP E), память в корпусе процессора
    {
        Profile p = {L"ultra7-258v", L"Intel Core Ultra 7 258V (4P+4LPE, Lunar Lake) + LPDDR5X-8533 32 ГБ", "GenuineIntel",
                     "Intel(R) Core(TM) Ultra 7 258V", 6, 0xBD, 1, 0x23, true, true, false, true, false,
                     {{4, 1, 0x40, 1, 48, 64, 2560, 1, 0}, {4, 1, 0x20, 0, 32, 64, 4096, 4, -1}}, {12288}, 3, 6, 0x5C, {}};
        const char* locs[] = {"Controller0-ChannelA", "Controller0-ChannelB", "Controller0-ChannelC", "Controller0-ChannelD",
                              "Controller1-ChannelA", "Controller1-ChannelB", "Controller1-ChannelC", "Controller1-ChannelD"};
        for (auto l : locs) p.dimms.push_back({l, "BANK 0", 4096, 0x23, 8533, 8533, 16, 16, SAM, "K3KL9L90DM-MHCU", 1});
        v.push_back(p);
    }
    // Arrow Lake под Hyper-V/VBS: гипервизор выдаёт E-ядрам кэши P-ядра — программа должна поправить по справочнику
    v.push_back({L"ultra9-285k-hyperv", L"Intel Core Ultra 9 285K (8P+16E) под Hyper-V, кэши E-ядер искажены + DDR5-6400 CUDIMM", "GenuineIntel",
                 "Intel(R) Core(TM) Ultra 9 285K", 6, 0xC6, 2, 0x23, true, true, false, true, true,
                 {{8, 1, 0x40, 1, 48, 64, 3072, 1, 0}, {16, 1, 0x20, 0, 48, 64, 3072, 4, 0}}, {36864}, 3, 7, 0x5C,
                 {{"Controller0-ChannelA-DIMM1", "BANK 0", 24576, 0x22, 6400, 6400, 64, 64, "Kingston", "KF564C32RSK2-48", 1},
                  {"Controller1-ChannelA-DIMM1", "BANK 0", 24576, 0x22, 6400, 6400, 64, 64, "Kingston", "KF564C32RSK2-48", 1}}});
    // Ryzen 9 7950X: два кристалла CCD, у каждого свой L3; JEDEC 4800, EXPO 6000
    v.push_back({L"ryzen9-7950x", L"AMD Ryzen 9 7950X (2 CCD, 2×32 МБ L3) + 2× DDR5-6000 EXPO", "AuthenticAMD",
                 "AMD Ryzen 9 7950X 16-Core Processor", 0x19, 0x61, 2, 0x10, true, true, true, false, false,
                 {{8, 2, 0, 0, 32, 32, 1024, 1, 0}, {8, 2, 0, 0, 32, 32, 1024, 1, 1}}, {32768, 32768}, 3, 5, 0x5C,
                 {{"DIMM 0", "P0 CHANNEL A", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"DIMM 1", "P0 CHANNEL A", 16384, 0x22, 4800, 6000, 64, 64, "G Skill Intl", "F5-6000J3038F16G", 1},
                  {"DIMM 0", "P0 CHANNEL B", 0, 0x02, 0, 0, 0, 0, "", "", 0},
                  {"DIMM 1", "P0 CHANNEL B", 16384, 0x22, 4800, 6000, 64, 64, "G Skill Intl", "F5-6000J3038F16G", 1}}});
    // Ryzen AI 9 HX 370: Zen 5 + Zen 5c, разные L3, гибрид только по EfficiencyClass Windows
    v.push_back({L"ryzen-ai9-hx370", L"AMD Ryzen AI 9 HX 370 (4 Zen 5 + 8 Zen 5c) + LPDDR5X-7500", "AuthenticAMD",
                 "AMD Ryzen AI 9 HX 370 w/ Radeon 890M", 0x1A, 0x24, 0, 0x10, true, true, true, false, false,
                 {{4, 2, 0, 1, 48, 32, 1024, 1, 0}, {8, 2, 0, 0, 48, 32, 1024, 1, 1}}, {16384, 8192}, 3, 6, 0x5C,
                 {{"DIMM 0", "P0 CHANNEL A", 8192, 0x23, 7500, 7500, 32, 32, MIC, "MT62F2G32D4DS-023", 1},
                  {"DIMM 0", "P0 CHANNEL B", 8192, 0x23, 7500, 7500, 32, 32, MIC, "MT62F2G32D4DS-023", 1},
                  {"DIMM 0", "P0 CHANNEL C", 8192, 0x23, 7500, 7500, 32, 32, MIC, "MT62F2G32D4DS-023", 1},
                  {"DIMM 0", "P0 CHANNEL D", 8192, 0x23, 7500, 7500, 32, 32, MIC, "MT62F2G32D4DS-023", 1}}});
    // Threadripper 7980X: 128 потоков = 2 группы процессоров Windows, 8 CCD
    {
        Profile p = {L"threadripper-7980x", L"AMD Threadripper 7980X (64C/128T, 2 группы Windows, 8 CCD) + 4× DDR5-5200 RDIMM ECC",
                     "AuthenticAMD", "AMD Ryzen Threadripper 7980X 64-Cores", 0x19, 0x18, 1, 0x10, true, true, true, false, false,
                     {}, {}, 3, 6, 0x5C, {}};
        for (int i = 0; i < 8; i++) {
            p.blocks.push_back({8, 2, 0, 0, 32, 32, 1024, 1, i});
            p.l3KB.push_back(32768);
        }
        const char* banks[] = {"P0 CHANNEL A", "P0 CHANNEL B", "P0 CHANNEL C", "P0 CHANNEL D"};
        for (auto b : banks) p.dimms.push_back({"DIMM 0", b, 32768, 0x22, 5200, 5200, 64, 80, "Kingston", "KF552R36RB-32", 2});
        v.push_back(p);
    }
    // Два Xeon на плате Supermicro: слоты «P1-DIMMA1», банк «P0_Node0_Channel0_Dimm0»
    {
        Profile p = {L"xeon-2s-ddr4", L"2× Intel Xeon E5-2690 v4 (2×14C/28T) + 8× DDR4-2400 RDIMM ECC (Supermicro)", "GenuineIntel",
                     "Intel(R) Xeon(R) CPU E5-2690 v4 @ 2.60GHz", 6, 0x4F, 1, 0x14, true, true, false, false, false,
                     {{14, 2, 0, 0, 32, 32, 256, 1, 0}, {14, 2, 0, 0, 32, 32, 256, 1, 1}}, {35840, 35840}, 3, 0, 0x28, {}};
        for (int s = 0; s < 2; s++)
            for (int c = 0; c < 4; c++) {
                static char loc[8][24], bank[8][40];
                int i = s * 4 + c;
                snprintf(loc[i], 24, "P%d-DIMM%c1", s + 1, 'A' + c);
                snprintf(bank[i], 40, "P%d_Node%d_Channel%d_Dimm0", s, s, c);
                p.dimms.push_back({loc[i], bank[i], 16384, 0x1A, 2400, 2400, 64, 72, SAM, "M393A2K40BB1-CRC", 2});
            }
        p.pkg2 = true;
        v.push_back(p);
    }
    // Atom-подобный Alder Lake-N: только E-ядра, один канал DDR5
    v.push_back({L"n100-ddr5", L"Intel N100 (4 E-ядра) + 1× DDR5-4800", "GenuineIntel", "Intel(R) N100", 6, 0xBE, 0, 0x20, true,
                 true, false, false, false, {{4, 1, 0, 0, 32, 64, 2048, 4, 0}}, {6144}, 3, 5, 0x5C,
                 {{"Controller0-ChannelA-DIMM0", "BANK 0", 16384, 0x22, 4800, 4800, 64, 64, "Crucial Technology", "CT16G48C40S5.M8A1", 1}}});
    // Виртуальная машина Hyper-V: тип памяти «Other», частота 0
    v.push_back({L"vm-hyperv", L"Виртуальная машина Hyper-V (4 vCPU, тип и частота памяти неизвестны)", "GenuineIntel",
                 "12th Gen Intel(R) Core(TM) i5-12500H", 6, 0x9A, 3, 0x20, true, true, false, false, true,
                 {{4, 1, 0, 0, 48, 32, 1280, 1, 0}}, {18432}, 3, 1, 0x28,
                 {{"M0001", "BANK 0", 4096, 0x01, 0, 0, 0, 0, "Microsoft Corporation", "None", 0}}});
    // ВМ без SMBIOS вообще
    v.push_back({L"vm-no-smbios", L"Виртуальная машина без таблицы SMBIOS (2 vCPU)", "GenuineIntel", "Intel Xeon Processor (Skylake)",
                 6, 0x55, 4, 0xD, true, true, false, false, true, {{2, 1, 0, 0, 32, 32, 4096, 1, 0}}, {16384}, 0, 0, 0, {}});
    // Windows не отдала топологию — запасной путь
    v.push_back({L"glpi-fail", L"Сбой GetLogicalProcessorInformationEx (топология и кэши неизвестны) + DDR4-3200", "GenuineIntel",
                 "Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz", 6, 0xA5, 3, 0x16, true, true, false, false, false,
                 {{6, 2, 0, 0, 32, 32, 256, 1, 0}}, {12288}, 3, 2, 0x28,
                 {{"ChannelA-DIMM0", "BANK 0", 8192, 0x1A, 3200, 2666, 64, 64, KNG, "KF432C16BB/8", 1},
                  {"ChannelB-DIMM0", "BANK 2", 8192, 0x1A, 3200, 2666, 64, 64, KNG, "KF432C16BB/8", 1}}});
    v.back().glpiFail = true;
    // Повреждённая таблица SMBIOS: обрезанная структура, нет завершающих нулей
    v.push_back({L"smbios-corrupt", L"Повреждённая таблица SMBIOS (обрезана посреди записи модуля)", "GenuineIntel",
                 "Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz", 6, 0x3C, 3, 0xD, true, true, false, false, false,
                 {{4, 1, 0, 0, 32, 32, 256, 1, 0}}, {6144}, 2, 8, 0x28,
                 {{"ChannelA-DIMM0", "BANK 0", 4096, 0x18, 1600, 1600, 64, 64, KNG, "KVR16N11/4", 2},
                  {"ChannelB-DIMM0", "BANK 2", 4096, 0x18, 1600, 1600, 64, 64, KNG, "KVR16N11/4", 2}}});
    v.back().smbCorrupt = true;
    return v;
}

std::vector<Profile> g_profiles;
const Profile* g_p = nullptr;
std::vector<BYTE> g_fuzzSmb;  // режим --emu-fuzz: следующая «таблица SMBIOS»
bool g_fuzz = false;

struct Lp { WORD group; BYTE number; int type; };
std::vector<Lp> g_lps;                      // логические CPU профиля
std::map<std::pair<int, int>, int> g_lpIdx; // (group, number) → индекс
thread_local int t_cpu = -1;               // какой CPU профиля «занимает» текущий поток

void LayoutLps()
{
    g_lps.clear();
    g_lpIdx.clear();
    int lp = 0;
    for (auto& b : g_p->blocks)
        for (int c = 0; c < b.cores; c++) {
            if (lp % 64 + b.threads > 64) lp = (lp / 64 + 1) * 64;  // ядро не делится между группами
            for (int t = 0; t < b.threads; t++, lp++) {
                g_lpIdx[{lp / 64, lp % 64}] = (int)g_lps.size();
                g_lps.push_back({(WORD)(lp / 64), (BYTE)(lp % 64), b.type});
            }
        }
}

void Put(std::vector<BYTE>& out, const void* p, size_t n)
{
    out.insert(out.end(), (const BYTE*)p, (const BYTE*)p + n);
}

void BuildGlpi(std::vector<BYTE>& out)
{
    out.clear();
    typedef SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX E;
    struct CoreRec { WORD group; KAFFINITY mask; };
    std::vector<std::vector<CoreRec>> coresByBlock;
    int lp = 0;
    for (auto& b : g_p->blocks) {
        coresByBlock.emplace_back();
        for (int c = 0; c < b.cores; c++) {
            if (lp % 64 + b.threads > 64) lp = (lp / 64 + 1) * 64;
            CoreRec r = {(WORD)(lp / 64), 0};
            for (int t = 0; t < b.threads; t++, lp++) r.mask |= (KAFFINITY)1 << (lp % 64);
            coresByBlock.back().push_back(r);
        }
    }
    int groups = lp ? (lp - 1) / 64 + 1 : 1;
    // Группы процессоров (запись переменной длины — проверяет переход по полю Size)
    {
        std::vector<BYTE> e(sizeof(E) + (groups - 1) * sizeof(PROCESSOR_GROUP_INFO), 0);
        E* g = (E*)e.data();
        g->Relationship = RelationGroup;
        g->Size = (DWORD)e.size();
        g->Group.MaximumGroupCount = g->Group.ActiveGroupCount = (WORD)groups;
        for (int i = 0; i < groups; i++) {
            int n = 0;
            KAFFINITY m = 0;
            for (auto& x : g_lps) if (x.group == i) { n++; m |= (KAFFINITY)1 << x.number; }
            g->Group.GroupInfo[i].MaximumProcessorCount = g->Group.GroupInfo[i].ActiveProcessorCount = (BYTE)n;
            g->Group.GroupInfo[i].ActiveProcessorMask = m;
        }
        Put(out, e.data(), e.size());
    }
    auto cache = [&](int level, PROCESSOR_CACHE_TYPE type, int kb, WORD group, KAFFINITY mask) {
        E e = {};
        e.Relationship = RelationCache;
        e.Size = sizeof e;
        e.Cache.Level = (BYTE)level;
        e.Cache.Associativity = 8;
        e.Cache.LineSize = 64;
        e.Cache.CacheSize = (DWORD)kb << 10;
        e.Cache.Type = type;
        e.Cache.GroupCount = g_p->win7Glpi ? 0 : 1;  // Windows 7–10: поле зарезервировано (0)
        e.Cache.GroupMasks[0].Group = group;
        e.Cache.GroupMasks[0].Mask = mask;
        Put(out, &e, sizeof e);
    };
    std::map<std::pair<int, int>, KAFFINITY> l3;  // (l3, group) → маска
    for (size_t bi = 0; bi < g_p->blocks.size(); bi++) {
        auto& b = g_p->blocks[bi];
        auto& cr = coresByBlock[bi];
        for (auto& r : cr) {
            E e = {};
            e.Relationship = RelationProcessorCore;
            e.Size = sizeof e;
            e.Processor.Flags = b.threads > 1 ? LTP_PC_SMT : 0;
            e.Processor.EfficiencyClass = (BYTE)b.eff;
            e.Processor.GroupCount = 1;
            e.Processor.GroupMask[0].Group = r.group;
            e.Processor.GroupMask[0].Mask = r.mask;
            Put(out, &e, sizeof e);
            cache(1, CacheData, b.l1d, r.group, r.mask);
            cache(1, CacheInstruction, b.l1i, r.group, r.mask);
        }
        for (size_t c = 0; c < cr.size(); c += b.l2share) {
            KAFFINITY m = 0;
            for (size_t k = c; k < c + b.l2share && k < cr.size(); k++) m |= cr[k].mask;
            cache(2, CacheUnified, b.l2, cr[c].group, m);
        }
        if (b.l3 >= 0)
            for (auto& r : cr) l3[{b.l3, r.group}] |= r.mask;
    }
    for (auto& kv : l3) cache(3, CacheUnified, g_p->l3KB[kv.first.first], (WORD)kv.first.second, kv.second);
    // Корпуса процессора (одна или две «ножки») и узел NUMA — должны пропускаться
    int pk = g_p->pkg2 ? 2 : 1;
    for (int i = 0; i < pk; i++) {
        E e = {};
        e.Relationship = RelationProcessorPackage;
        e.Size = sizeof e;
        e.Processor.GroupCount = 1;
        for (size_t k = i * g_lps.size() / pk; k < (i + 1) * g_lps.size() / pk; k++) {
            e.Processor.GroupMask[0].Group = g_lps[k].group;
            e.Processor.GroupMask[0].Mask |= (KAFFINITY)1 << g_lps[k].number;
        }
        Put(out, &e, sizeof e);
        E n = {};
        n.Relationship = RelationNumaNode;
        n.Size = sizeof n;
        n.NumaNode.NodeNumber = i;
        n.NumaNode.GroupMask = e.Processor.GroupMask[0];
        Put(out, &n, sizeof n);
    }
}

void BuildSmbios(const Profile& p, std::vector<BYTE>& out)
{
    out.assign(8, 0);
    out[1] = (BYTE)p.smbMaj;
    out[2] = (BYTE)p.smbMin;
    auto add = [&](BYTE type, std::vector<BYTE> body, const std::vector<std::string>& strs) {
        body[0] = type;
        body[1] = (BYTE)body.size();
        Put(out, body.data(), body.size());
        if (strs.empty()) out.push_back(0);
        for (auto& s : strs) Put(out, s.c_str(), s.size() + 1);
        out.push_back(0);
    };
    // type 0 (BIOS) и type 16 — чтобы проверить пропуск чужих записей
    std::vector<BYTE> b0(0x18, 0);
    b0[4] = 1; b0[5] = 2; b0[8] = 3;
    add(0, b0, {"American Megatrends Inc.", "1.0 emu", "01/01/2024"});
    std::vector<BYTE> b16(0x17, 0);
    add(16, b16, {});
    int handle = 0x40;
    for (auto& d : p.dimms) {
        std::vector<BYTE> r(p.t17len, 0);
        std::vector<std::string> strs;
        auto str = [&](int off, const char* s) {
            if (off >= p.t17len || !s || !*s) return;
            strs.push_back(s);
            r[off] = (BYTE)strs.size();
        };
        auto w = [&](int off, unsigned v) { if (off + 1 < p.t17len) { r[off] = v & 0xFF; r[off + 1] = (v >> 8) & 0xFF; } };
        auto dw = [&](int off, unsigned v) { if (off + 3 < p.t17len) { w(off, v & 0xFFFF); w(off + 2, v >> 16); } };
        w(2, handle++);
        w(8, d.tw ? d.tw : 0xFFFF);
        w(0x0A, d.dw ? d.dw : 0xFFFF);
        if (d.sizeMB > 32767) { w(0x0C, 0x7FFF); dw(0x1C, d.sizeMB); }
        else w(0x0C, d.sizeMB);
        r[0x0E] = 0x09;  // DIMM
        str(0x10, d.loc);
        str(0x11, d.bank);
        r[0x12] = (BYTE)d.type;
        w(0x15, d.speed);
        str(0x17, d.manuf);
        str(0x18, d.sizeMB ? "12345678" : "");
        str(0x1A, d.part);
        if (p.t17len > 0x1B) r[0x1B] = (BYTE)d.ranks;
        w(0x20, d.conf);
        w(0x26, d.type == 0x22 || d.type == 0x23 ? 1100 : d.type == 0x1A ? 1200 : 1500);
        add(17, r, strs);
    }
    add(127, std::vector<BYTE>(4, 0), {});
    if (p.smbCorrupt) {
        // обрезаем посреди второй записи модуля и ломаем длину в заголовке
        size_t cut = out.size() - 4 - 30;
        out.resize(cut);
        *(DWORD*)&out[4] = (DWORD)(out.size() - 8 + 500);
        return;
    }
    *(DWORD*)&out[4] = (DWORD)(out.size() - 8);
}

void RealCpuid(unsigned leaf, unsigned sub, unsigned r[4]) { __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]); }

void WriteUtf8(const wchar_t* path, const std::wstring& text)
{
    if (FILE* f = _wfopen(path, L"wb")) {
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), s.data(), n, nullptr, nullptr);
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
}

void RunFuzz(int n, const wchar_t* out)
{
    // Случайно портим настоящие таблицы профилей и скармливаем разборщику SMBIOS.
    std::mt19937 rng(12345);
    g_fuzz = true;
    size_t modules = 0, ok = 0;
    for (int i = 0; i < n; i++) {
        const Profile& p = g_profiles[i % g_profiles.size()];
        if (!p.smbMaj) continue;
        BuildSmbios(p, g_fuzzSmb);
        int mode = rng() % 4;
        int flips = 1 + rng() % 16;
        for (int k = 0; k < flips; k++) g_fuzzSmb[8 + rng() % (g_fuzzSmb.size() - 8)] = (BYTE)rng();
        if (mode == 1) g_fuzzSmb.resize(8 + rng() % (g_fuzzSmb.size() - 8));
        if (mode == 2) *(DWORD*)&g_fuzzSmb[4] = rng();
        if (mode == 3) for (size_t k = 8; k < g_fuzzSmb.size(); k++) if (rng() % 8 == 0) g_fuzzSmb[k] = (BYTE)rng();
        MemInfo mi;
        QueryMemInfo(mi);
        modules += mi.modules.size();
        if (mi.peakGBs >= 0 && mi.peakGBs < 1e7) ok++;
    }
    WriteUtf8(out, Fmt(L"fuzz: %d таблиц, разобрано модулей: %zu, результатов в разумных пределах: %zu\n", n, modules, ok));
}

} // namespace

bool EmuParseArgs(int argc, wchar_t** argv)
{
    g_profiles = MakeProfiles();
    for (int i = 1; i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--emu")) {
            for (auto& p : g_profiles)
                if (p.name == std::wstring(argv[i + 1])) g_p = &p;
            if (!g_p) {
                MessageBoxW(nullptr, argv[i + 1], L"Нет такого профиля эмуляции", MB_ICONERROR);
                return true;
            }
            LayoutLps();
        }
        if (!wcscmp(argv[i], L"--emu-list")) {
            std::wstring t;
            for (auto& p : g_profiles) t += std::wstring(p.name) + L"\t" + p.desc + L"\n";
            WriteUtf8(argv[i + 1], t);
            return true;
        }
        if (!wcscmp(argv[i], L"--emu-fuzz") && i + 2 < argc) {
            RunFuzz(_wtoi(argv[i + 1]), argv[i + 2]);
            return true;
        }
    }
    return false;
}

bool EmuActive() { return g_p != nullptr || g_fuzz; }

std::wstring EmuTitle() { return g_p ? std::wstring(L"ЭМУЛЯЦИЯ: ") + g_p->desc : L""; }

bool EmuCpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    if (!g_p) return false;
    const Profile& p = *g_p;
    unsigned real[4];
    RealCpuid(leaf, sub, real);
    r[0] = r[1] = r[2] = r[3] = 0;
    if (leaf < 0x80000000 && leaf > p.maxLeaf) return true;
    switch (leaf) {
    case 0:
        r[0] = p.maxLeaf;
        memcpy(&r[1], p.vendor, 4);
        memcpy(&r[3], p.vendor + 4, 4);
        memcpy(&r[2], p.vendor + 8, 4);
        return true;
    case 1: {
        unsigned fb = p.family >= 0xF ? 0xF : p.family, fe = p.family >= 0xF ? p.family - 0xF : 0;
        r[0] = p.stepping | (p.model & 0xF) << 4 | fb << 8 | (p.model >> 4) << 16 | fe << 20;
        r[1] = real[1];
        // AVX/OSXSAVE — только если есть и в профиле, и у настоящего процессора (ядра тестов выполняются по-настоящему)
        r[2] = real[2] & ~((1u << 28) | (1u << 27) | (1u << 31));
        if (p.avx) r[2] |= real[2] & ((1u << 28) | (1u << 27));
        if (p.hv) r[2] |= 1u << 31;
        r[3] = real[3];
        return true;
    }
    case 7:
        if (sub == 0) {
            r[1] = real[1] & ~((1u << 5) | (1u << 16));
            if (p.avx2) r[1] |= real[1] & (1u << 5);
            if (p.avx512) r[1] |= real[1] & (1u << 16);
            r[3] = (real[3] & ~(1u << 15)) | (p.hybrid ? 1u << 15 : 0);
        }
        return true;
    case 0x1A:
        if (t_cpu >= 0 && t_cpu < (int)g_lps.size()) r[0] = (unsigned)g_lps[t_cpu].type << 24;
        return true;
    case 0x80000000:
        r[0] = 0x80000008;
        return true;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        char b[48] = {};
        strncpy(b, p.brand, 47);
        memcpy(r, b + (leaf - 0x80000002) * 16, 16);
        return true;
    }
    case 0x80000007:
        r[3] = 1u << 8;
        return true;
    }
    memcpy(r, real, sizeof real);
    return true;
}

bool EmuGlpi(std::vector<BYTE>& buf, bool& ok)
{
    if (!g_p) return false;
    ok = !g_p->glpiFail;
    if (ok) BuildGlpi(buf);
    return true;
}

bool EmuSmbios(std::vector<BYTE>& buf, bool& ok)
{
    if (g_fuzz) {
        buf = g_fuzzSmb;
        ok = true;
        return true;
    }
    if (!g_p) return false;
    ok = g_p->smbMaj != 0;
    if (ok) BuildSmbios(*g_p, buf);
    return true;
}

bool EmuPin(const LogicalCpu& c, bool& ok)
{
    if (!g_p) return false;
    // CPU профиля → настоящий CPU этого ПК (по кругу)
    auto it = g_lpIdx.find({c.group, c.number});
    int idx = it == g_lpIdx.end() ? (c.group * 64 + c.number) : it->second;
    t_cpu = it == g_lpIdx.end() ? -1 : it->second;
    WORD groups = GetActiveProcessorGroupCount();
    DWORD total = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD k = total ? idx % total : 0;
    GROUP_AFFINITY ga = {};
    for (WORD g = 0; g < groups; g++) {
        DWORD n = GetActiveProcessorCount(g);
        if (k < n) {
            ga.Group = g;
            ga.Mask = (KAFFINITY)1 << k;
            break;
        }
        k -= n;
    }
    ok = SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
    Sleep(0);
    return true;
}

uint64_t EmuTotalMB()
{
    if (!g_p) return 0;
    uint64_t s = 0;
    for (auto& d : g_p->dimms) s += d.sizeMB;
    return s ? s : 4096;
}
