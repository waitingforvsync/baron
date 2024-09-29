#include <string.h>
#include "arena.h"
#include "allocator.h"
#include "defines.h"


#define ARENA_REGION_SIZE 0x4000

typedef struct arena_region_t arena_region_t;
typedef struct arena_region_alloc_header_t arena_region_alloc_header_t;

struct arena_region_t {
    arena_region_t *next;
    uint32_t start;
    uint32_t end;
};

struct arena_region_alloc_header_t {
    uint32_t prev;
};


static uint32_t get_aligned_size(uint32_t size) {
    return ((size + 0x0F) & ~0x0F) + 0x10;
}


arena_t make_arena(const allocator_t *allocator, uint32_t region_size) {
    return (arena_t){
        .child_allocator = allocator,
        ._region = allocator_alloc(allocator, get_aligned_size(region_size))
    };
}


void arena_init(arena_t *arena, const allocator_t *allocator, uint32_t initial_size) {
    ASSERT(arena);
    UNUSED(initial_size);
    arena_deinit(arena);
    arena->child_allocator = allocator;
}


void arena_deinit(arena_t *arena) {
    ASSERT(arena);
}


void *arena_alloc(arena_t *arena, uint32_t size) {
    ASSERT(arena);

    size = ((size + 0x0F) & ~0x0F) + 0x10;
    if (!arena->_region || arena->_region->end - arena->_region->start < size) {

        uint32_t region_size = (size > ARENA_REGION_SIZE) ? size : ARENA_REGION_SIZE;
        arena_region_t *new_region = allocator_alloc(arena->child_allocator, region_size);
        if (!new_region) {
            return 0;
        }

        new_region->next = arena->_region;
        new_region->start = (sizeof(arena_region_t) + 0x0F) & ~0x0F;
        new_region->end = region_size;
    }

    return 0; //todo
}


void arena_reset(arena_t *arena) {
    ASSERT(arena);
    if (arena->_region) {
        arena_region_t *ptr = arena->_region->next;
        while (ptr) {
            arena_region_t *to_free = ptr;
            ptr = ptr->next;
            allocator_free(arena->child_allocator, to_free);
        }
    }
}


static void *arena_allocator_alloc(uint32_t size, void *context) {
    return arena_alloc(context, size);
}


static void *arena_allocator_realloc(void *ptr, uint32_t size, void *context) {
    void *new_ptr = arena_alloc(context, size);
    if (new_ptr) {
        if (ptr) {
            memcpy(new_ptr, ptr, size);
        }
        return new_ptr;
    }
    else {
        return 0;
    }
}


static void arena_allocator_free(void *ptr, void *context) {
    UNUSED(ptr);
    UNUSED(context);
    // do nothing!
}


allocator_t arena_allocator(arena_t *arena) {
    ASSERT(arena);

    static const allocator_vtable_t allocator_vtable = {
        arena_allocator_alloc,
        arena_allocator_realloc,
        arena_allocator_free
    };

    return (allocator_t){
        arena,
        &allocator_vtable
    };
}
