// BenchKrasulya — ядра AVX-512 (512 бит). Компилируется с -mavx512f, вызывается только при поддержке.
#include "kernels.h"
#include <immintrin.h>

#define KNS k_avx512
#define KFILL FillKernels_avx512
#define VT __m512i
#define VB 64
#define VLOAD(p) _mm512_loadu_si512((const void*)(p))
#define VOR(a, b) _mm512_or_si512(a, b)
#define VZERO() _mm512_setzero_si512()
#define VSET() _mm512_set1_epi32(0x5A5A5A5A)
#define VSTORE(p, v) _mm512_storeu_si512((void*)(p), v)
#define VSTREAM(p, v) _mm512_stream_si512((__m512i*)(p), v)
#include "kernels_impl.inc"
