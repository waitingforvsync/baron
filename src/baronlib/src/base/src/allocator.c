#include <stdlib.h>
#include "allocator.h"
#include "defines.h"


void *allocator_alloc(const allocator_t *allocator, uint32_t size) {
    if (!allocator) {
        allocator = allocator_default();
    }
    ASSERT(allocator->vtable);
    return allocator->vtable->alloc(size, allocator->context);
}


void *allocator_realloc(const allocator_t *allocator, void *current_ptr, uint32_t new_size) {
    if (!allocator) {
        allocator = allocator_default();
    }
    ASSERT(allocator->vtable);
    return allocator->vtable->realloc(current_ptr, new_size, allocator->context);
}


void allocator_free(const allocator_t *allocator, void *ptr) {
    if (!allocator) {
        allocator = allocator_default();
    }
    ASSERT(allocator->vtable);
    allocator->vtable->free(ptr, allocator->context);
}


static void *allocator_default_alloc(uint32_t size, void *context) {
    UNUSED(context);
    return malloc((size_t)size);
}


static void *allocator_default_realloc(void *ptr, uint32_t size, void *context) {
    UNUSED(context);
    return realloc(ptr, (size_t)size);
}


static void allocator_default_free(void *ptr, void *context) {
    UNUSED(context);
    free(ptr);
}


const allocator_t *allocator_default(void) {

    static const allocator_vtable_t default_vtable = {
        allocator_default_alloc,
        allocator_default_realloc,
        allocator_default_free
    };

    static const allocator_t default_allocator = {
        0,
        &default_vtable
    };

    return &default_allocator;
}
