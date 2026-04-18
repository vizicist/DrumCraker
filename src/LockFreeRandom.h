#pragma once
#include <cstdint>

// xorshift32 RNG safe for real-time audio threads: no allocations, no locks,
// no thread-local storage lookup. One instance per audio-thread consumer.
class LockFreeRandom
{
public:
    LockFreeRandom() { seed(0x9E3779B9u); }

    void seed(uint32_t s) noexcept
    {
        state = s ? s : 0x9E3779B9u;
    }

    uint32_t nextUInt() noexcept
    {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    // Uniform float in [0, 1)
    float nextFloat() noexcept
    {
        return (nextUInt() >> 8) * (1.0f / 16777216.0f);
    }

private:
    uint32_t state = 0x9E3779B9u;
};
