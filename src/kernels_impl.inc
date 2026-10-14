// BenchKrasulya — шаблон ядер. Подключается из kernels_*.cpp, каждый файл задаёт
// свои макросы под набор инструкций и компилируется со своими флагами (-mavx и т.д.).
//
// Нужны: KNS, KFILL, VT, VB (байт в векторе), VLOAD, VOR, VZERO, VSET, VSTORE, VSTREAM.
// Компиляторный барьер "memory" между проходами не даёт GCC выбросить
// повторное чтение одних и тех же данных.

#define BARRIER() __asm__ volatile("" ::: "memory")

namespace KNS {

static inline uint64_t Reduce(VT v)
{
    alignas(64) uint64_t t[VB / 8];
    VSTORE(t, v);
    uint64_t r = 0;
    for (int i = 0; i < VB / 8; i++) r ^= t[i];
    return r;
}

static uint64_t seqRead(const void* p0, size_t bytes, size_t reps)
{
    VT a0 = VZERO(), a1 = VZERO(), a2 = VZERO(), a3 = VZERO();
    VT a4 = VZERO(), a5 = VZERO(), a6 = VZERO(), a7 = VZERO();
    const uint8_t* p = (const uint8_t*)p0;
    const uint8_t* e = p + bytes;
    for (size_t r = 0; r < reps; r++) {
        for (const uint8_t* q = p; q < e; q += 8 * VB) {
            a0 = VOR(a0, VLOAD(q));
            a1 = VOR(a1, VLOAD(q + VB));
            a2 = VOR(a2, VLOAD(q + 2 * VB));
            a3 = VOR(a3, VLOAD(q + 3 * VB));
            a4 = VOR(a4, VLOAD(q + 4 * VB));
            a5 = VOR(a5, VLOAD(q + 5 * VB));
            a6 = VOR(a6, VLOAD(q + 6 * VB));
            a7 = VOR(a7, VLOAD(q + 7 * VB));
        }
        BARRIER();
    }
    a0 = VOR(VOR(VOR(a0, a1), VOR(a2, a3)), VOR(VOR(a4, a5), VOR(a6, a7)));
    return Reduce(a0);
}

static void seqWrite(void* p0, size_t bytes, size_t reps)
{
    VT v = VSET();
    uint8_t* p = (uint8_t*)p0;
    uint8_t* e = p + bytes;
    for (size_t r = 0; r < reps; r++) {
        for (uint8_t* q = p; q < e; q += 8 * VB) {
            VSTORE(q, v);
            VSTORE(q + VB, v);
            VSTORE(q + 2 * VB, v);
            VSTORE(q + 3 * VB, v);
            VSTORE(q + 4 * VB, v);
            VSTORE(q + 5 * VB, v);
            VSTORE(q + 6 * VB, v);
            VSTORE(q + 7 * VB, v);
        }
        BARRIER();
    }
}

static void seqWriteNT(void* p0, size_t bytes, size_t reps)
{
    VT v = VSET();
    uint8_t* p = (uint8_t*)p0;
    uint8_t* e = p + bytes;
    for (size_t r = 0; r < reps; r++) {
        for (uint8_t* q = p; q < e; q += 8 * VB) {
            VSTREAM(q, v);
            VSTREAM(q + VB, v);
            VSTREAM(q + 2 * VB, v);
            VSTREAM(q + 3 * VB, v);
            VSTREAM(q + 4 * VB, v);
            VSTREAM(q + 5 * VB, v);
            VSTREAM(q + 6 * VB, v);
            VSTREAM(q + 7 * VB, v);
        }
        BARRIER();
    }
    _mm_sfence();
}

#if VB <= 32
// ---- блоки по B байт со скачками: B / VB векторов на блок

template <int B>
static uint64_t jumpRead(const uint8_t* p, ptrdiff_t step, size_t count)
{
    constexpr int N = B / VB;
    VT a0 = VZERO(), a1 = VZERO(), a2 = VZERO(), a3 = VZERO();
    for (size_t i = 0; i < count; i++) {
#pragma GCC unroll 16
        for (int k = 0; k < N; k++) {
            VT x = VLOAD(p + k * VB);
            switch (k & 3) {
            case 0: a0 = VOR(a0, x); break;
            case 1: a1 = VOR(a1, x); break;
            case 2: a2 = VOR(a2, x); break;
            default: a3 = VOR(a3, x); break;
            }
        }
        p += step;
    }
    BARRIER();
    return Reduce(VOR(VOR(a0, a1), VOR(a2, a3)));
}

template <int B>
static void jumpWrite(uint8_t* p, ptrdiff_t step, size_t count)
{
    constexpr int N = B / VB;
    VT v = VSET();
    for (size_t i = 0; i < count; i++) {
#pragma GCC unroll 16
        for (int k = 0; k < N; k++) VSTORE(p + k * VB, v);
        p += step;
    }
    BARRIER();
}

template <int B>
static void jumpWriteNT(uint8_t* p, ptrdiff_t step, size_t count)
{
    constexpr int N = B / VB;
    VT v = VSET();
    for (size_t i = 0; i < count; i++) {
#pragma GCC unroll 16
        for (int k = 0; k < N; k++) VSTREAM(p + k * VB, v);
        p += step;
    }
    _mm_sfence();
    BARRIER();
}

// ---- случайные блоки (ЛКГ-генератор считается в регистрах, адреса независимы)
#define LCG_NEXT(x) ((x) = (x) * 6364136223846793005ull + 1442695040888963407ull)

template <int B>
static uint64_t randRead(const uint8_t* base, size_t mask, size_t count, uint64_t x)
{
    constexpr int N = B / VB;
    VT a0 = VZERO(), a1 = VZERO();
    for (size_t i = 0; i < count; i++) {
        LCG_NEXT(x);
        const uint8_t* p = base + ((x >> 20) & mask) * B;
#pragma GCC unroll 16
        for (int k = 0; k < N; k++) {
            if (k & 1) a1 = VOR(a1, VLOAD(p + k * VB));
            else a0 = VOR(a0, VLOAD(p + k * VB));
        }
    }
    BARRIER();
    return Reduce(VOR(a0, a1));
}

template <int B>
static void randWrite(uint8_t* base, size_t mask, size_t count, uint64_t x)
{
    constexpr int N = B / VB;
    VT v = VSET();
    for (size_t i = 0; i < count; i++) {
        LCG_NEXT(x);
        uint8_t* p = base + ((x >> 20) & mask) * B;
#pragma GCC unroll 16
        for (int k = 0; k < N; k++) VSTORE(p + k * VB, v);
    }
    BARRIER();
}

template <int B>
static void randWriteNT(uint8_t* base, size_t mask, size_t count, uint64_t x)
{
    constexpr int N = B / VB;
    VT v = VSET();
    for (size_t i = 0; i < count; i++) {
        LCG_NEXT(x);
        uint8_t* p = base + ((x >> 20) & mask) * B;
#pragma GCC unroll 16
        for (int k = 0; k < N; k++) VSTREAM(p + k * VB, v);
    }
    _mm_sfence();
    BARRIER();
}
#endif

}  // namespace KNS

void KFILL(KernelSet& k)
{
    k.vecBits = VB * 8;
    k.seqRead = KNS::seqRead;
    k.seqWrite = KNS::seqWrite;
    k.seqWriteNT = KNS::seqWriteNT;
#if VB <= 32
    k.jumpRead[0] = KNS::jumpRead<32>;
    k.jumpRead[1] = KNS::jumpRead<64>;
    k.jumpRead[2] = KNS::jumpRead<128>;
    k.jumpRead[3] = KNS::jumpRead<256>;
    k.jumpWrite[0] = KNS::jumpWrite<32>;
    k.jumpWrite[1] = KNS::jumpWrite<64>;
    k.jumpWrite[2] = KNS::jumpWrite<128>;
    k.jumpWrite[3] = KNS::jumpWrite<256>;
    k.jumpWriteNT[0] = KNS::jumpWriteNT<32>;
    k.jumpWriteNT[1] = KNS::jumpWriteNT<64>;
    k.jumpWriteNT[2] = KNS::jumpWriteNT<128>;
    k.jumpWriteNT[3] = KNS::jumpWriteNT<256>;
    k.randRead[0] = KNS::randRead<32>;
    k.randRead[1] = KNS::randRead<64>;
    k.randRead[2] = KNS::randRead<128>;
    k.randRead[3] = KNS::randRead<256>;
    k.randWrite[0] = KNS::randWrite<32>;
    k.randWrite[1] = KNS::randWrite<64>;
    k.randWrite[2] = KNS::randWrite<128>;
    k.randWrite[3] = KNS::randWrite<256>;
    k.randWriteNT[0] = KNS::randWriteNT<32>;
    k.randWriteNT[1] = KNS::randWriteNT<64>;
    k.randWriteNT[2] = KNS::randWriteNT<128>;
    k.randWriteNT[3] = KNS::randWriteNT<256>;
#endif
}
