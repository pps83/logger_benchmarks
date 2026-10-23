#pragma once
#include <stdint.h>

typedef uint64_t rand_pcg32_t;
struct rand_splitmix64_t
{
    uint64_t s[1];
};
struct rand_romu_t
{
    uint64_t s[2];
};
struct rand_xoroshiro128_t
{
    uint64_t s[2];
};
struct rand_lehmer64_t
{
    uint64_t s[2];
};
typedef rand_pcg32_t rand_default_t;

template<typename rand_t>
uint32_t rand32_r(rand_t& state); // return random uint32_t (full 32 bits)

template<typename rand_t>
uint64_t rand64_r(rand_t& state); // return random uint64_t (full 64 bits)

template<typename rand_t>
void srand64_r(rand_t& state, uint64_t seed); // seed rnd state to reproduce random sequences. If seed is 0, random number will be generated

template<typename rand_t>
int64_t randRange_r(rand_t& state, int64_t min, int64_t max); // random number in [min, max] range

uint32_t rand32(); // uses global shared state
uint64_t rand64(); // uses global shared state
void srand64(uint64_t seed); // seed global shared state to reproduce random sequences. If seed is 0, std::random_device will be used for seeding
int64_t randRange(int64_t min, int64_t max); // random number in [min, max] range, uses global shared state
