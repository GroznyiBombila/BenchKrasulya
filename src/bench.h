// BenchKrasulya — тесты
#pragma once
#include "kernels.h"
#include "sysinfo.h"
#include <atomic>
#include <string>
#include <vector>

struct BenchConfig {
    int cpu = 0;            // логический CPU для однопоточных тестов
    Isa isa = ISA_SSE2;
    bool ramMode = true;    // true — область больше L3 (настоящая ОЗУ), false — 4 МБ (в пределах L3)
    bool largePages = false;
};

// ---- «Кэш»: скорость от размера буфера
struct CacheResult {
    bool valid = false;
    int cpu = -1;
    std::wstring isaName;
    double ghzBefore = 0, ghzAfter = 0;
    std::vector<double> sizes;
    std::vector<double> read, write, writeNT, code;  // ГБ/с, NaN — ещё не измерено
};

// ---- «Задержка»: случайный обход цепочки указателей
struct LatencyResult {
    bool valid = false;
    int cpu = -1;
    bool largePages = false;
    double ghz = 0;
    std::vector<double> sizes;
    std::vector<double> ns;
};

// ---- «Чтение RAM» / «Запись RAM»: блоки 32..256 Б со скачками
struct JumpResult {
    bool valid = false;
    bool write = false;
    bool ramMode = true;
    int cpu = -1;
    size_t span = 0;
    double ghz = 0;
    std::vector<double> jumps;
    std::vector<double> y[4];       // чтение или запись в обход кэша (NT)
    std::vector<double> yc[4];      // запись через кэш (только для записи)
    double random[4] = {}, randomC[4] = {};
};

// ---- «Многопоток»: масштабирование пропускной способности ОЗУ
struct MTSummary {
    std::wstring name;
    int threads = 0;
    double read = 0, writeNT = 0, write = 0;
};
struct MTResult {
    bool valid = false;
    size_t perThread = 0;
    std::vector<int> order;     // порядок добавления логических CPU
    int cacheOnly = 0;          // точек с суммарным буфером < 4×L3 (пропущены: мерили бы кэш)
    int skipped = 0;            // потоков без памяти (лимит процесса / мало свободной ОЗУ)
    std::vector<double> n;      // число потоков
    std::vector<double> read, writeNT, write;
    std::vector<MTSummary> summary;
};

enum TestKind { TEST_CACHE = 0, TEST_LATENCY, TEST_READ, TEST_WRITE, TEST_MT, TEST_COUNT };

struct BenchState {
    std::atomic<bool> stop{false};
    std::atomic<int> progress{0};      // 0..1000
    std::atomic<bool> running{false};
    CRITICAL_SECTION cs;               // защищает status
    std::wstring status;
    void SetStatus(const std::wstring& s);
    std::wstring GetStatus();
};

extern BenchState g_bench;
extern CpuInfo g_cpu;
extern MemInfo g_mem;
extern CacheResult g_cache;
extern LatencyResult g_lat;
extern JumpResult g_jread, g_jwrite;
extern MTResult g_mt;

void BenchInit();
bool LargePagesAvailable();
double MeasureCoreGHz();               // частота ядра, на котором сейчас идёт поток
void MeasureClusterClocks();           // для окна «Система»
void RunTest(TestKind kind, const BenchConfig& cfg);
size_t RamSpanBytes();                 // размер области для тестов ОЗУ
