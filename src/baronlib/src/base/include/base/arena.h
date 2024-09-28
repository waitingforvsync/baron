/**
 *  @file   arena.h
 * 
 *  An arena is a chain of regions of memory from which allocations are made, sequentially from the lowest address upwards.
 *  When a region runs out of space, a new region is created for subsequent allocations.
 * 
 *  Allocations made from an arena are never freed individually; instead the arena itself has a lifetime, and
 *  all allocations made within it are freed in one go when requested.
 */

#ifndef ARENA_H_
#define ARENA_H_


#include <stdint.h>

typedef struct arena_t arena_t;
typedef struct allocator_t allocator_t;


struct arena_t {
    const allocator_t *child_allocator;
    struct arena_region_t *_region;
};


/**
 *  Make an arena
 */
arena_t make_arena(const allocator_t *allocator, uint32_t initial_size);


/**
 *  Initialize an existing arena
 */
void arena_init(arena_t *arena, const allocator_t *allocator, uint32_t initial_size);


/**
 *  Deinitialize an arena
 */
void arena_deinit(arena_t *arena);


/**
 *  Allocate from an arena
 */
void *arena_alloc(arena_t *arena, uint32_t size);


/**
 *  Reset an arena, deallocating everything
 */
void arena_reset(arena_t *arena);


/**
 *  Return an allocator which works with the given arena
 */
allocator_t arena_allocator(arena_t *arena);


#endif // ifndef ARENA_H_
