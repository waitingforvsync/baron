#include "random.h"


void prng_seed(prng *p, uint32_t seed)
{
    p->state = seed;
}

// splitmix32: bump a counter by the golden-ratio odd constant, then run it through two xor-shift-multiply
// mixes. The multiply constants are the well-tested pair from the hash-prospector search - they give a
// near-ideal avalanche, so even a 1-bit change in the state scatters across the whole output.
uint32_t prng_next(prng *p)
{
    uint32_t z = (p->state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(random, deterministic)
{
    // The same seed replays the same stream, byte for byte.
    prng a, b;
    prng_seed(&a, 0x1234abcdu);
    prng_seed(&b, 0x1234abcdu);
    for (int i = 0; i < 16; i++) {
        RC_CHECK(prng_next(&a), ==, prng_next(&b));
    }
}

RC_TEST(random, diverges_and_moves)
{
    // Consecutive draws are not stuck on one value...
    prng p;
    prng_seed(&p, 1);
    uint32_t v0 = prng_next(&p);
    uint32_t v1 = prng_next(&p);
    uint32_t v2 = prng_next(&p);
    RC_CHECK_TRUE(v0 != v1 || v1 != v2);

    // ...and a different seed gives a different stream.
    prng q;
    prng_seed(&q, 2);
    RC_CHECK_TRUE(prng_next(&q) != v0);
}

#endif // BARON_TESTS
