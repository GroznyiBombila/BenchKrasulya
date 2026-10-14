// BenchKrasulya — вычислительные ядра тестов (SSE2 / AVX / AVX-512)
#pragma once
#include <cstddef>
#include <cstdint>

enum Isa { ISA_SSE2 = 0, ISA_AVX = 1, ISA_AVX512 = 2 };

// Индексы размеров блока в тестах «Чтение/Запись RAM»: 32, 64, 128, 256 байт
constexpr int kBlockSizes[4] = {32, 64, 128, 256};

struct KernelSet {
    const wchar_t* name = L"";
    int vecBits = 0;
    // Последовательный проход по буферу (bytes кратно 512), reps раз
    uint64_t (*seqRead)(const void* p, size_t bytes, size_t reps) = nullptr;
    void (*seqWrite)(void* p, size_t bytes, size_t reps) = nullptr;
    void (*seqWriteNT)(void* p, size_t bytes, size_t reps) = nullptr;  // в обход кэша (MOVNT*)
    // Тест со скачками: count блоков, адрес следующего блока = текущий + step
    uint64_t (*jumpRead[4])(const uint8_t* p, ptrdiff_t step, size_t count) = {};
    void (*jumpWrite[4])(uint8_t* p, ptrdiff_t step, size_t count) = {};
    void (*jumpWriteNT[4])(uint8_t* p, ptrdiff_t step, size_t count) = {};
    // Случайные блоки: адрес = base + (ГПСЧ & mask) * B
    uint64_t (*randRead[4])(const uint8_t* base, size_t mask, size_t count, uint64_t seed) = {};
    void (*randWrite[4])(uint8_t* base, size_t mask, size_t count, uint64_t seed) = {};
    void (*randWriteNT[4])(uint8_t* base, size_t mask, size_t count, uint64_t seed) = {};
};

KernelSet MakeKernels(Isa isa);

void FillKernels_sse2(KernelSet& k);
void FillKernels_avx(KernelSet& k);
void FillKernels_avx512(KernelSet& k);
