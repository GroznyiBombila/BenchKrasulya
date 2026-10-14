// BenchKrasulya — сведения о процессоре, кэшах и памяти
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct CacheInfo {
    uint64_t size = 0;      // байт
    uint32_t line = 0;      // длина строки
    uint32_t sharedBy = 0;  // сколько логических CPU делят этот кэш
    uint32_t sharedCores = 0;  // сколько физических ядер его делят
};

struct LogicalCpu {
    WORD group = 0;
    BYTE number = 0;
    int core = -1;          // индекс физического ядра
    int threadInCore = 0;   // 0 — первый поток ядра, 1 — HT-брат
    int threadsInCore = 1;
    int effClass = 0;       // EfficiencyClass из Windows (больше — производительнее)
    int hybridType = 0;     // CPUID 1Ah: 0x40 = Core (P), 0x20 = Atom (E)
    CacheInfo l1d, l1i, l2, l3;
    int cluster = -1;
};

struct CpuCluster {
    std::wstring name;      // «P-ядро», «E-ядро», «LP E-ядро», «Ядро»
    std::wstring shortName; // «P», «E», «LPE»
    std::vector<int> cpus;  // логические CPU (все потоки)
    int cores = 0;
    int rank = 0;           // порядок: 0 — самые быстрые ядра
    int rep = -1;           // представитель (первый поток первого ядра)
    CacheInfo l1d, l1i, l2, l3;
    double clockGHz = 0;    // измеренная частота ядра
};

struct CpuInfo {
    std::wstring vendor, brand;
    int family = 0, model = 0, stepping = 0;
    bool sse2 = false, avx = false, avx2 = false, avx512 = false;
    bool hybrid = false, hypervisor = false, invariantTsc = false;
    bool eCacheFromTable = false;  // размеры кэшей E-ядер уточнены по справочнику (гипервизор их скрыл)
    bool topologyUnknown = false;  // Windows не отдала топологию: ядра и кэши не определены
    double tscGHz = 0;
    std::vector<LogicalCpu> cpus;
    std::vector<CpuCluster> clusters;
    int physCores = 0;
    uint64_t totalL3 = 0;   // сумма всех отдельных L3 (на несколько CCD/кристаллов)
    uint64_t maxL3 = 0;
};

struct MemModule {
    std::wstring locator, bank, manufacturer, part;
    uint64_t sizeMB = 0;
    int type = 0;
    std::wstring typeName;
    int formFactor = 0;
    uint32_t speed = 0, confSpeed = 0;  // МТ/с
    uint32_t rawSpeed = 0;              // как записано в SMBIOS, если пришлось пересчитать из МГц
    int dataWidth = 0, totalWidth = 0;
    int ranks = 0;
    int voltage_mV = 0;
    std::wstring channelKey;            // «контроллер:канал», если удалось разобрать
};

struct MemInfo {
    bool smbiosOk = false;
    int smbiosMajor = 0, smbiosMinor = 0;
    std::vector<MemModule> modules;
    uint64_t totalMB = 0, availMB = 0;  // по данным ОС
    std::wstring typeName;
    uint32_t speedMT = 0;
    int channels = 0;
    int busBits = 0;
    double peakGBs = 0;
    std::wstring channelNote;
};

void QueryCpuInfo(CpuInfo& ci);         // топология, кэши, наборы инструкций
void DetectCoreTypes(CpuInfo& ci);      // CPUID 1Ah на каждом ядре + кластеры (запускает поток)
void QueryMemInfo(MemInfo& mi);         // SMBIOS type 17
bool PinCurrentThread(const LogicalCpu& c);
void DisablePowerThrottling();          // просим Windows не душить поток (EcoQoS)
std::wstring OsVersionString();

std::wstring FormatSize(uint64_t bytes);       // «48 КБ», «1.25 МБ»
std::wstring FormatSizeAxis(double bytes);     // «48К», «1М» — для подписей осей
std::wstring Widen(const std::string& s);
std::wstring Fmt(const wchar_t* fmt, ...);
