#include "zeropage.h"

#include "richc/macros.h"


void zeropage_init(zeropage *zp, rc_arena *permanent)
{
    RC_ASSERT(zp != NULL && permanent != NULL);
    zp->arena    = permanent;
    zp->reserved = (rc_bitset) {0};
    rc_bitset_resize(&zp->reserved, zeropage_size, permanent);   // 256 addressable, all zero
    zp->enabled  = false;
}

void zeropage_reset(zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    rc_bitset_reset(&zp->reserved);   // clears every bit, keeps num/cap
    zp->enabled = false;
}

void zeropage_enable(zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    zp->enabled = true;
}

void zeropage_reserve(zeropage *zp, uint32_t byte)
{
    RC_ASSERT(zp != NULL && byte < zeropage_size);
    rc_bitset_set(&zp->reserved, byte);
    zp->enabled = true;   // reserving a byte implies the feature is on, even before an explicit enable
}

bool zeropage_is_enabled(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return zp->enabled;
}

bool zeropage_is_reserved(const zeropage *zp, uint32_t byte)
{
    RC_ASSERT(zp != NULL);
    return byte < zeropage_size && rc_bitset_is_set(&zp->reserved, byte);
}

uint32_t zeropage_reserved_count(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    uint32_t count = 0;
    for (uint32_t b = rc_bitset_get_first_set(&zp->reserved);
         b != RC_INDEX_NONE;
         b = rc_bitset_get_next_set(&zp->reserved, b + 1)) {
        count++;
    }
    return count;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(zeropage, reserve_and_query)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    // A fresh set is empty and dormant.
    RC_CHECK_FALSE(zeropage_is_enabled(&zp));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 0u);
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x70));

    // Reserving a byte switches the feature on and records exactly that byte.
    zeropage_reserve(&zp, 0x70);
    RC_CHECK_TRUE(zeropage_is_enabled(&zp));
    RC_CHECK_TRUE(zeropage_is_reserved(&zp, 0x70));
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x71));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 1u);

    // Idempotent: re-reserving the same byte does not double-count.
    zeropage_reserve(&zp, 0x70);
    RC_CHECK(zeropage_reserved_count(&zp), ==, 1u);

    // A span of bytes, and the boundary byte 0xFF.
    for (uint32_t b = 0x80; b <= 0x8F; b++) {
        zeropage_reserve(&zp, b);
    }
    zeropage_reserve(&zp, 0xFF);
    RC_CHECK(zeropage_reserved_count(&zp), ==, 1u + 16u + 1u);
    RC_CHECK_TRUE(zeropage_is_reserved(&zp, 0x8F));
    RC_CHECK_TRUE(zeropage_is_reserved(&zp, 0xFF));

    // Out-of-page queries are simply false, never a trap.
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, zeropage_size));
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x1000));

    rc_arena_deinit(&arena);
}

RC_TEST(zeropage, enable_without_bytes)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    // RESERVE with an empty list enables the feature but reserves nothing.
    zeropage_enable(&zp);
    RC_CHECK_TRUE(zeropage_is_enabled(&zp));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 0u);

    rc_arena_deinit(&arena);
}

RC_TEST(zeropage, reset_clears_for_next_pass)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    zeropage_reserve(&zp, 0x70);
    zeropage_reserve(&zp, 0x71);
    RC_CHECK(zeropage_reserved_count(&zp), ==, 2u);

    // A new pass starts from an empty, dormant set (the backing is kept).
    zeropage_reset(&zp);
    RC_CHECK_FALSE(zeropage_is_enabled(&zp));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 0u);
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x70));

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
