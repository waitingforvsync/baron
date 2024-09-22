/**
 *  @file   allocator.h
 * 
 *  Defines the interface for a generic allocator.
 *  Any allocations should always be performed explicitly through the allocator interface.
 */

#ifndef ALLOCATOR_H_
#define ALLOCATOR_H_


#include <stdint.h>

typedef struct allocator_t allocator_t;
typedef struct allocator_vtable_t allocator_vtable_t;


/**
 *  vtable for an allocator type, containing function pointers to the implementations of the three operations
 */
struct allocator_vtable_t {
    void *(*alloc)(uint32_t size, void *context);
    void *(*realloc)(void *ptr, uint32_t size, void *context);
    void  (*free)(void *ptr, void *context);
};


/**
 *  struct representing an allocator
 */
struct allocator_t {
    void *context;
    const allocator_vtable_t *vtable;
};


/**
 *  Call the alloc function on the given allocator.
 * 
 *  @param  allocator       Non-null pointer to the allocator to use
 *  @param  size            Size in bytes to allocate
 * 
 *  @return Pointer to the allocation, or null if it failed.
 */
void *allocator_alloc(const allocator_t *allocator, uint32_t size);


/**
 *  Call the realloc function on the given allocator.
 *  The old allocated data will be copied to the newly allocated buffer if successful.
 * 
 *  @param  allocator       Non-null pointer to the allocator to use
 *  @param  current_ptr     Pointer to the current allocation; if null this acts as a straightforward alloc
 *  @param  new_size        New size in bytes to allocate
 * 
 *  @return Pointer to the new allocation. If it failed, the old allocation is unaffected and null is returned.
 */
void *allocator_realloc(const allocator_t *allocator, void *current_ptr, uint32_t new_size);


/** 
 *  Call the free function on the given allocator
 * 
 *  @param  allocator       Non-null pointer to the allocator to use
 *  @param  ptr             Pointer to the allocation to free. If null, this does nothing.
 */
void allocator_free(const allocator_t *allocator, void *ptr);


/**
 *  Returns a pointer to the default allocator
 */
const allocator_t *allocator_default(void);


#endif // ifndef ALLOCATOR_H_
