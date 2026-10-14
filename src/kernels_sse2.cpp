// BenchKrasulya — ядра SSE2 (128 бит). Работают на любом x86-64 процессоре.
#include "kernels.h"
#include <emmintrin.h>

#define KNS k_sse2
#define KFILL FillKernels_sse2
#define VT __m128
#define VB 16
#define VLOAD(p) _mm_loadu_ps((const float*)(p))
#define VOR(a, b) _mm_or_ps(a, b)
#define VZERO() _mm_setzero_ps()
#define VSET() _mm_castsi128_ps(_mm_set1_epi32(0x5A5A5A5A))
#define VSTORE(p, v) _mm_storeu_ps((float*)(p), v)
#define VSTREAM(p, v) _mm_stream_ps((float*)(p), v)
#include "kernels_impl.inc"

KernelSet MakeKernels(Isa isa)
{
    KernelSet k;
    FillKernels_sse2(k);
    k.name = L"SSE2";
    if (isa >= ISA_AVX) {
        FillKernels_avx(k);
        k.name = L"AVX";
    }
    if (isa >= ISA_AVX512) {
        // для блоков 32 Б 512-битный вектор не подходит — тесты со скачками остаются на AVX
        FillKernels_avx512(k);
        k.name = L"AVX-512";
    }
    return k;
}
