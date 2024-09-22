#include <assert.h>
#include "array.h"
#include "allocator.h"


typedef struct array_header_t array_header_t;


void *array_allocate_generic(const allocator_t *allocator, uint32_t capacity, uint32_t element_size) {
    uint32_t total_size = capacity * element_size + 16;
    array_header_t *header = allocator_alloc(allocator, total_size);
    header->allocator = allocator;
    header->capacity = capacity;
    header->element_size = element_size;
    return header + 1;
}


int array_reserve_generic(void **data, uint32_t capacity) {
    if (*data) {
        array_header_t *header = (array_header_t *)(*data) - 1;
        if (header->capacity < capacity) {
            uint32_t total_size = capacity * header->element_size + 16;
            array_header_t *new_header = allocator_realloc(header->allocator, header, total_size);
            if (new_header) {
                new_header->capacity = capacity;
                *data = new_header + 1;
                return 0;
            }
        }
    }
    return 1;   // failed
}


void array_free_generic(void *data) {
    if (data) {
        array_header_t *header = (array_header_t *)data - 1;
        allocator_free(header->allocator, header);
    }
}
