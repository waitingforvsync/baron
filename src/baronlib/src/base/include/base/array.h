/**
 *  @file   array.h
 * 
 *  Representation of generic arrays and slices.
 */

#ifndef ARRAY_H_
#define ARRAY_H_


#include <stdint.h>
#include <stdbool.h>

typedef struct allocator_t allocator_t;


/**
 *  Convenience macro for defining arbitrary slice types.
 */
#define def_slice(type) \
    typedef struct slice_const_##type { const type *data; uint32_t size; } slice_const_##type; \
    typedef struct slice_##type { union { struct { type *data; uint32_t size; }; slice_const_##type const_slice; }; } slice_##type; \
    typedef struct array_##type { union { struct { type *data; uint32_t size; }; slice_const_##type const_slice; slice_##type slice; }; } array_##type; \
    typedef struct slice_const_ptr_##type { const type **data; uint32_t size; } slice_const_ptr_##type; \
    typedef struct slice_ptr_##type { union { struct { type **data; uint32_t size; }; slice_const_ptr_##type const_slice; }; } slice_ptr_##type; \
    typedef struct array_ptr_##type { union { struct { type **data; uint32_t size; }; slice_const_ptr_##type const_slice; slice_ptr_##type slice; }; } array_ptr_##type

def_slice(uint8_t);
def_slice(uint16_t);
def_slice(uint32_t);
def_slice(uint64_t);
def_slice(int8_t);
def_slice(int16_t);
def_slice(int32_t);
def_slice(int64_t);
def_slice(char);
def_slice(float);
def_slice(double);


/**
 *  Return an array initializer literal, initializing the array with the given capacity
 * 
 *  Example of use:
 *    array_int32_t numbers = make_array(int32_t, allocator_default(), 256);
 *    if (array_is_valid(&numbers)) {
 *       // ...
 *    }
 *
 *  @param  type        Element type of the array
 *  @param  allocator   Pointer to the allocator to use and associate with the array
 *  @param  capacity    Initial capacity of the array (reserved element count)
 */
#define make_array(type, allocator, capacity) (array_##type){.data = array_init_generic(allocator, capacity, (uint32_t)sizeof(type)), .size = 0}


/**
 *  Return whether the given array is valid, i.e. has been initialized
 * 
 *  @param  array       Pointer to array to check validity
 * 
 *  @return bool whether valid or not
 */
#define array_is_valid(array) (!!(array)->data)


/**
 *  Reserve the given capacity in an already initialized array.
 *  If the array has not been previously initialized, this will fail.
 * 
 *  @param  array       Pointer to array to reserve space for
 *  @param  capacity    Number of elements to reserve space for
 * 
 *  @return Success true/false
 */
#define array_reserve(array, capacity) array_reserve_generic((void **)&(array)->data, capacity)


/**
 *  Determine if the array is empty
 * 
 *  @param  array       Pointer to the array to query
 * 
 *  @return Empty true/false
 */
#define array_is_empty(array) (!(array)->size)


/**
 *  Get the capacity of the array
 * 
 *  @param  array       Pointer to the array to query
 * 
 *  @return Reserved number of elements
 */
#define array_capacity(array) array_get_capacity_generic((array)->data)


/**
 *  Resize the array to the given size.
 *  If the size is greater than the currently allocated capacity, it will be reallocated.
 * 
 *  @param  array       Pointer to array to reserve space for
 *  @param  new_size    Number of elements in the array
 * 
 *  @return Success true/false
 */
#define array_resize(array, new_size) (array_reserve_generic((void **)&(array)->data, new_size) && (((array)->size = new_size), true))


/**
 *  Reset the array to empty, without deallocating existing allocations.
 * 
 *  @param  array       Pointer to array to reset
 */
#define array_reset(array) ((array)->size = 0)


/**
 *  Check whether an array contains a slice
 * 
 *  @param  array       Pointer to the array to check
 *  @param  slice       Slice to check against
 * 
 *  @return true/false
 */
#define array_contains_slice(array, slice) ((slice).data - (array)->data >= 0 && (slice).data - (array)->data < (array)->size)


/**
 *  Append a slice to the array, allocating more space if necessary
 *  
 *  @param  array       Pointer to the array to append the slice to
 *  @param  slice       Slice to be appended
 * 
 *  @return Success true/false
 * 
 *  @note   It is not allowed to append any part of an array to itself, as potential reallocation would render the slice invalid.
 */
#define array_append(array, slice) (!array_contains_slice(array, slice) && array_append_generic((void **)&(array)->data, (slice).data, (array)->size, (slice).size) && ((array)->size += (slice).size, true))


/**
 *  Add an element to the end of the array, allocating more space if necessary
 * 
 *  @param  array       Pointer to the array to add an item to
 *  @param  value       The value to be added
 * 
 *  @return Success true/false
 */
#define array_add(array, value) (array_maybe_grow_generic((void **)&(array)->data, (array)->size) && (((array)->data[((array)->size)++] = (value)), true))


/**
 *  Deinitialize the array, freeing the allocated memory
 * 
 *  @param  array       Pointer to the array to deinitialize
 */
#define array_deinit(array) (array_free_generic((array)->data), (array)->data = 0, (array)->size = 0)


/**
 *  Initialize an array, allocating memory for it.
 *  A special header is allocated immediately in front of the data buffer containing details relating to the array.
 *  
 *  @param  allocator       The allocator used by this array
 *  @param  capacity        The initial capacity to allocate
 *  @param  element_size    The size of a single array element in bytes
 * 
 *  @return Pointer to the allocated memory, or null if it failed
 */
void *array_init_generic(const allocator_t *allocator, uint32_t capacity, uint32_t element_size);


/**
 *  Change the reserved allocation for the array.
 *  
 *  @param  data            Pointer to the data member of the array slice. This will be rewritten with the new buffer address.
 *  @param  capacity        New capacity to allocate.
 * 
 *  @return Success true/false
 */
bool array_reserve_generic(void **data, uint32_t capacity);


/**
 *  Maybe grow the array exponentially.
 * 
 *  @param  data            Pointer to the data member of the array slice.
 *                          This will be rewritten with the new buffer address if reallocation occurs.
 *  @param  current_size    Number of active elements currently held in the array.
 * 
 *  @return Success true/false
 */
bool array_maybe_grow_generic(void **data, uint32_t current_size);


/**
 *  Append the given slice to the array.
 * 
 *  @param  dest            Pointer to the data member of the array slice.
 *                          This will be rewritten with the new buffer address if reallocation occurs.
 *  @param  src             Pointer to the data to be appended.
 *  @param  dest_size       Number of items in the array.
 *  @param  src_size        Number of items in the slice to be appended.
 *  
 *  @return Success true/false
 * 
 *  @note   It is incumbent on the caller to ensure that the slice holds the same element type as the array.
 */
bool array_append_generic(void **dest, const void *src, uint32_t dest_size, uint32_t src_size);


/**
 *  Get the allocated capacity for a given array
 * 
 *  @param  data            Pointer to the data of the array slice
 * 
 *  @return The capacity of the array, in elements (not bytes)
 */
uint32_t array_get_capacity_generic(const void *data);


/**
 *  Free allocations made by the array.
 * 
 *  @param  data            Buffer to free
 */
void array_free_generic(void *data);


#endif // ifndef ARRAY_H_
