#ifndef BARON_RANDOM_H_
#define BARON_RANDOM_H_

#include <stdint.h>


// A tiny, fast pseudo-random generator (splitmix32): one word of state, an add and two multiply-mixes per
// draw, good statistical quality, and - unlike xorshift - no bad seeds (any seed, including 0, is fine).
// Self-contained on purpose: a candidate to lift into richc later (a rename away). Not for cryptography.
typedef struct prng {
    uint32_t state;
} prng;

// Seed the generator. Any value works; the same seed always replays the same stream.
void prng_seed(prng *p, uint32_t seed);

// The next 32-bit draw, advancing the state.
uint32_t prng_next(prng *p);


#endif // ifndef BARON_RANDOM_H_
