/**
 *  @file   array.h
 * 
 *  Representation of generic arrays and slices.
 */

#ifndef ARRAY_H_
#define ARRAY_H_


#include <stdint.h>
#include <assert.h>

typedef struct allocator_t allocator_t;

struct array_header_t {
    const allocator_t *allocator;
    uint32_t capacity;
    uint32_t element_size;
};

static_assert(sizeof(struct array_header_t) == 16, "array_header wrong size");


/**
 *  Convenience macro for defining arbitrary slice types.
 */
#define def_slice(type, name) \
    typedef struct array_##name##_t { type *data; uint32_t size; } array_##name##_t; \
    typedef struct array_##name##_ptr_t { type **data; uint32_t size; } array_##name##_ptr_t; \
    typedef array_##name##_t slice_##name##_t; \
    typedef struct slice_const_##name##_t { const type *data; uint32_t size; } slice_const_##name##_t; \
    typedef array_##name##_ptr_t slice_##name##_ptr_t; \
    typedef struct slice_const_##name##_ptr_t { const type **data; uint32_t size; } slice_const_##name##_ptr_t

def_slice(uint8_t, uint8);
def_slice(uint16_t, uint16);
def_slice(uint32_t, uint32);
def_slice(uint64_t, uint64);
def_slice(int8_t, int8);
def_slice(int16_t, int16);
def_slice(int32_t, int32);
def_slice(int64_t, int64);
def_slice(char, char);
def_slice(float, float);
def_slice(double, double);


/**
 *  Get a const slice from an existing array or slice
 */
#define const_slice(type, name) (slice_const_##type##_t){name.data, name.size}


/**
 *  Return an array initializer literal, initializing the array with the given capacity
 * 
 *  Example of use:
 *    array_int32_t numbers = make_array(int32_t, allocator_default(), 256);
 *
 *  @param  type        Element type of the array
 *  @param  allocator   Pointer to the allocator to use and associate with the array
 *  @param  capacity    Initial capacity of the array (reserved element count)
 */
#define make_array(type, allocator, capacity) {array_allocate_generic(allocator, capacity, (uint32_t)sizeof(type)), 0}


/**
 *  Reserve the given capacity in an already initialized array.
 *  If the array has not been previously initialized, this will fail.
 * 
 *  @param  array       Pointer to array to reserve space for
 *  @param  capacity    Number of elements to reserve space for
 * 
 *  @return 0 if successful, otherwise 1
 */
#define array_reserve(array, capacity) (array_reserve_generic((void **)&((array)->data), capacity))


/**
 *  Get the capacity of the array
 * 
 *  @param  array       Pointer to the array to query
 * 
 *  @return Reserved number of elements
 */
#define array_capacity(array) ((((const struct array_header_t *)((array)->data)) - 1)->capacity)


/**
 *  Resize the array to the given size.
 *  If the size is greater than the currently allocated capacity, it will be reallocated.
 * 
 *  @param  array       Pointer to array to reserve space for
 *  @param  new_size    Number of elements in the array
 * 
 *  @return 0 if successful, otherwise 1
 */
#define array_resize(array, new_size) (((new_size) >= array_capacity(array) && (array_reserve_generic((void **)&((array)->data), new_size))) || (((array)->size = new_size), 0))


/**
 *  Add an element to the end of the array, allocating more space if necessary
 * 
 *  @param  array       Pointer to the array to add an item to
 *  @param  value       The value to be added
 * 
 *  @return 0 if successful, otherwise 1
 */
#define array_add(array, value) ((((array)->size >= array_capacity(array)) && array_reserve(array, (array_capacity(array) < 8) ? 8 : array_capacity(array) * 3 / 2)) || (((array)->data[((array)->size)++] = (value)), 0))


/**
 *  Deinitialize the array, freeing the allocated memory
 * 
 *  @param  array       Pointer to the array to deinitialize
 */
#define array_deinit(array) (array_free_generic(array->data), array->data = 0, array->size = 0)


/**
 *  Allocate memory for the array.
 *  A special header is allocated immediately in front of the data buffer containing details relating to the array.
 *  
 *  @param  allocator       The allocator used by this array
 *  @param  capacity        The initial capacity to allocate
 *  @param  element_size    The size of a single array element in bytes
 * 
 *  @return Pointer to the allocated memory, or null if it failed
 */
void *array_allocate_generic(const allocator_t *allocator, uint32_t capacity, uint32_t element_size);


/**
 *  Change the reserved allocation for the array.
 *  
 *  @param  data            Pointer to the data member of the array slice. This will be rewritten with the new buffer address.
 *  @param  capacity        New capacity to allocate.
 * 
 *  @return 0 if successful, otherwise 1
 */
int array_reserve_generic(void **data, uint32_t capacity);


/**
 *  Free allocations made by the array.
 * 
 *  @param  data            Buffer to free
 */
void array_free_generic(void *data);


#endif // ifndef ARRAY_H_
