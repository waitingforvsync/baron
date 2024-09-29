#include <assert.h>
#include <string.h>
#include "array.h"
#include "allocator.h"


typedef struct array_header_t array_header_t;

struct array_header_t {
    const allocator_t *allocator;
    uint32_t capacity;
    uint32_t element_size;
};

static_assert(sizeof(array_header_t) == 16, "array_header wrong size");


void *array_init_generic(const allocator_t *allocator, uint32_t capacity, uint32_t element_size) {
    uint32_t total_size = capacity * element_size + sizeof(array_header_t);
    array_header_t *header = allocator_alloc(allocator, total_size);
    if (!header) {
        return 0;
    }

    header->allocator = allocator;
    header->capacity = capacity;
    header->element_size = element_size;
    return header + 1;
}


static bool array_reallocate(void **data, array_header_t *header, uint32_t capacity) {
    uint32_t total_size = capacity * header->element_size + sizeof(array_header_t);
    array_header_t *new_header = allocator_realloc(header->allocator, header, total_size);
    if (new_header) {
        new_header->capacity = capacity;
        *data = new_header + 1;
        return true;
    }
    return false;
}


bool array_reserve_generic(void **data, uint32_t capacity) {
    if (!*data) {
        return false;
    }
    array_header_t *header = (array_header_t *)(*data) - 1;
    if (capacity <= header->capacity) {
        return true;
    }
    return array_reallocate(data, header, capacity);
}


bool array_maybe_grow_generic(void **data, uint32_t current_size) {
    if (!*data) {
        return false;
    }
    array_header_t *header = (array_header_t *)(*data) - 1;
    if (current_size < header->capacity) {
        return true;
    }
    return array_reallocate(data, header, (current_size < 8) ? 8 : current_size * 3 / 2);
}


bool array_append_generic(void **dest, const void *src, uint32_t dest_size, uint32_t src_size) {
    if (!*dest || !src) {
        return false;
    }
    array_header_t *header = (array_header_t *)(*dest) - 1;
    uint32_t element_size = header->element_size;

    if (dest_size + src_size > header->capacity && !array_reallocate(dest, header, dest_size + src_size)) {
        return false;
    }

    memcpy((uint8_t *)(*dest) + dest_size * element_size, src, src_size * element_size);
    return true;
}


uint32_t array_get_capacity_generic(const void *data) {
    if (data) {
        const array_header_t *header = (const array_header_t *)data - 1;
        return header->capacity;
    }

    return 0;
}


void array_free_generic(void *data) {
    if (data) {
        array_header_t *header = (array_header_t *)data - 1;
        allocator_free(header->allocator, header);
    }
}
