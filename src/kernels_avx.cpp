// BenchKrasulya — ядра AVX (256 бит). Компилируется с -mavx, вызывается только если CPU и ОС поддерживают AVX.
#include "kernels.h"
#include <immintrin.h>

#define KNS k_avx
#define KFILL FillKernels_avx
#define VT __m256
#define VB 32
#define VLOAD(p) _mm256_loadu_ps((const float*)(p))
#define VOR(a, b) _mm256_or_ps(a, b)
#define VZERO() _mm256_setzero_ps()
#define VSET() _mm256_castsi256_ps(_mm256_set1_epi32(0x5A5A5A5A))
#define VSTORE(p, v) _mm256_storeu_ps((float*)(p), v)
#define VSTREAM(p, v) _mm256_stream_ps((float*)(p), v)
#include "kernels_impl.inc"
