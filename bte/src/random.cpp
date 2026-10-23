#include "random.h"
#include <assert.h>
#include <random>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <boost/int128.hpp>

typedef boost::int128::uint128 uint128_t;

// *Really* minimal PCG32 code
struct pcg32_random_t
{
    uint64_t state;
    static constexpr uint64_t inc = 1007983139u; // modified from original pcg
};

static uint32_t pcg32_random_r(pcg32_random_t* rng)
{
    uint64_t oldstate = rng->state;
    // Advance internal state
    rng->state = oldstate * 6364136223846793005ULL + (rng->inc | 1);
    // Calculate output function (XSH RR), uses old state for max ILP
    uint32_t xorshifted = static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0 - rot) & 31));
}
// End of PCG32

// xoroshiro128 code
static inline uint64_t rotl(const uint64_t x, int k)
{
#ifdef _MSC_VER
    return _rotl64(x, k);
#else
    return (x << k) | (x >> (64 - k));
#endif
}

static uint64_t xoroshiro128_random_r(uint64_t s[2])
{
    const uint64_t s0 = s[0];
    uint64_t s1 = s[1];
#if 1 // xoroshiro128+ https://prng.di.unimi.it/xoroshiro128plus.c
    const uint64_t result = s0 + s1;
    s1 ^= s0;
    s[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16); // a, b
    s[1] = rotl(s1, 37); // c
#elif 1 // xoroshiro128++ https://prng.di.unimi.it/xoroshiro128plusplus.c
    const uint64_t result = rotl(s0 + s1, 17) + s0;
    s1 ^= s0;
    s[0] = rotl(s0, 49) ^ s1 ^ (s1 << 21); // a, b
    s[1] = rotl(s1, 28); // c
#else // xoroshiro128** https://prng.di.unimi.it/xoroshiro128starstar.c
    const uint64_t result = rotl(s0 * 5, 7) * 9;
    s1 ^= s0;
    s[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16); // a, b
    s[1] = rotl(s1, 37); // c
#endif
    return result;
}
// end of xoroshiro128

// splitmix64 code https://prng.di.unimi.it/splitmix64.c
static uint64_t splitmix64_random_r(uint64_t s[1])
{
    uint64_t z = (s[0] += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
// end of splitmix64

// RomuDuoJr code https://www.romu-random.org/code.c
static uint64_t romuduojr_random_r(uint64_t s[2])
{
    uint64_t x = s[0], y = s[1];
    s[0] = 15241094284759029579ULL * y;
    s[1] = rotl(y - x, 27);
    return x;
}
// end of RomuDuoJr

// lehmer64 code https://www.romu-random.org/code.c
static uint64_t lehmer64_random_r(uint64_t s[2])
{
    if constexpr (sizeof(uint128_t) == 16 && sizeof(uint128_t) == sizeof(uint64_t[2]))
    {
        uint128_t x;
        memcpy((void*)&x, &s[0], sizeof(x));
        x *= 0xda942042e4dd58b5ULL;
        memcpy(&s[0], (void*)&x, sizeof(x));
        return (uint64_t)(x >> 64);
    }
    else
    {
        uint128_t x = s[1];
        x <<= 64;
        x |= s[0];
        x *= 0xda942042e4dd58b5ULL;
        s[0] = (uint64_t)x;
        s[1] = (uint64_t)(x >> 64);
        return (uint64_t)(x >> 64);
    }
}
// end of lehmer64

static uint64_t getRandomSeed()
{
    std::random_device rnd;
    return ((1ULL * rnd()) << 32) ^ rnd();
}

template<>
uint32_t rand32_r(rand_pcg32_t& state)
{
    pcg32_random_t rnd = { state };
    uint32_t r = pcg32_random_r(&rnd);
    state = rnd.state;
    return r;
}

template<>
uint64_t rand64_r(rand_pcg32_t& state)
{
    pcg32_random_t rnd = { state };
    uint32_t r_l = pcg32_random_r(&rnd);
    uint64_t r_h = pcg32_random_r(&rnd);
    state = rnd.state;
    return (r_h << 32) | r_l;
}

template<>
void srand64_r(rand_pcg32_t& state, uint64_t seed)
{
    state = 0x853c49e6748fea9bULL ^ (seed == 0 ? getRandomSeed() : seed);
    rand32_r(state);
    state = (state ^ 0xda3e39cb94b95bdbULL) | 1;
}

template<>
uint64_t rand64_r(rand_splitmix64_t& state)
{
    return splitmix64_random_r(state.s);
}

template<>
void srand64_r(rand_splitmix64_t& state, uint64_t seed)
{
    srand64_r(state.s[0], seed);
}

template<>
uint64_t rand64_r(rand_romu_t& state)
{
    return romuduojr_random_r(state.s);
}

template<>
uint64_t rand64_r(rand_xoroshiro128_t& state)
{
    return xoroshiro128_random_r(state.s);
}

template<>
uint64_t rand64_r(rand_lehmer64_t& state)
{
    return lehmer64_random_r(state.s);
}

template<typename rand_t>
uint32_t rand32_r(rand_t& state)
{
    return (uint32_t)rand64_r(state);
}

template<typename rand_t>
void srand64_r(rand_t& state, uint64_t seed)
{
    srand64_r(state.s[0], seed);
    state.s[1] = splitmix64_random_r(&state.s[0]);
}

static int64_t randRange32(uint32_t r, int64_t min, int64_t max)
{
    uint64_t x = (max - min + 1ull) * r;
    return static_cast<int64_t>(min + (x >> 32));
}

// return high 64 bits of 128-bit result
static inline uint64_t mullhi_u64(uint64_t x, uint64_t y)
{
#if defined(_MSC_VER) && (!defined(__clang__) || _MSC_VER > 1930) &&  (defined(_M_X64) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC))
    return __umulh(x, y);
#elif defined(__SIZEOF_INT128__)
    return (uint64_t)(((__uint128_t)x * y) >> 64);
#else
    uint32_t x0y0_hi = ((uint64_t)(uint32_t)(x) * (uint32_t)(y)) >> 32;
    uint64_t x0y1 = (uint32_t)(y >> 32) * (uint64_t)(uint32_t)(x);
    uint64_t x1y0 = (uint32_t)(x >> 32) * (uint64_t)(uint32_t)(y);
    uint64_t x1y1 = (uint32_t)(x >> 32) * (uint64_t)(uint32_t)(y >> 32);
    uint64_t temp = x1y0 + x0y0_hi;
    uint64_t temp_lo = temp & 0xFFFFFFFFu;
    uint64_t temp_hi = temp >> 32;
    return x1y1 + temp_hi + ((temp_lo + x0y1) >> 32);
#endif
}

static int64_t randRange64(uint64_t r, int64_t min, int64_t max)
{
    // uncommon case for large [min, max] ranges
    uint64_t x = max - min + 1ull;
    if (x == 0) // [INT64_MIN, INT64_MAX] range
        return static_cast<int64_t>(r);
    else
        return static_cast<int64_t>(min + mullhi_u64(r, x));
}

template<typename rand_t>
int64_t randRange_r(rand_t& state, int64_t min, int64_t max)
{
    assert(min <= max);
    if ((uint64_t)(max - min) < 0xfffffffe) // if difference between [min, max] does not exceed 32 bits
        return randRange32(rand32_r(state), min, max);
    else
        return randRange64(rand64_r(state), min, max);
}

template uint32_t rand32_r(rand_splitmix64_t& state);
template uint32_t rand32_r(rand_romu_t& state);
template uint32_t rand32_r(rand_xoroshiro128_t& state);
template uint32_t rand32_r(rand_lehmer64_t& state);

template void srand64_r(rand_romu_t& state, uint64_t seed);
template void srand64_r(rand_xoroshiro128_t& state, uint64_t seed);
template void srand64_r(rand_lehmer64_t& state, uint64_t seed);

template int64_t randRange_r(rand_pcg32_t& state, int64_t min, int64_t max);
template int64_t randRange_r(rand_splitmix64_t& state, int64_t min, int64_t max);
template int64_t randRange_r(rand_romu_t& state, int64_t min, int64_t max);
template int64_t randRange_r(rand_xoroshiro128_t& state, int64_t min, int64_t max);
template int64_t randRange_r(rand_lehmer64_t& state, int64_t min, int64_t max);

static rand_default_t gRandState = {};

template<typename rand_t>
inline rand_t getRandomSeedT()
{
    rand_t ret;
    for (size_t i = 0; i < std::size(ret.s); ++i)
        ret.s[i] = getRandomSeed();
    return ret;
}

template<>
inline rand_pcg32_t getRandomSeedT<rand_pcg32_t>()
{
    return getRandomSeed();
}

static inline rand_default_t& randState()
{
    constexpr rand_default_t null = {};
    if (0 == memcmp(&gRandState, &null, sizeof(null)))
        gRandState = getRandomSeedT<rand_default_t>();
    return gRandState;
}

uint32_t rand32()
{
    return rand32_r(randState());
}

uint64_t rand64()
{
    return rand64_r(randState());
}

void srand64(uint64_t seed)
{
    srand64_r(gRandState, seed);
}

int64_t randRange(int64_t min, int64_t max)
{
    return randRange_r(randState(), min, max);
}
