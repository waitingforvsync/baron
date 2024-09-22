/**
 *  @file   baron.h
 *
 *  Define public entry points for baronlib
 */

#ifndef BARONLIB_BARON_H_
#define BARONLIB_BARON_H_

#include <stdint.h>
#include <stdlib.h>

typedef struct baron_allocator_fns_t baron_allocator_fns_t;
typedef struct baron_allocator_t baron_allocator_t;
typedef struct baron_desc_t baron_desc_t;
typedef struct baron_assembly_t baron_assembly_t;
typedef struct baron_object_code_t baron_object_code_t;


/**
 *  @struct baron_allocator_fns_t
 * 
 *  This defines the functions which implement the three allocator operations
 */
struct baron_allocator_fns_t {
    void *(*alloc)(size_t size, void *context);
    void *(*realloc)(void *oldptr, size_t newsize, void *context);
    void  (*free)(void *ptr, void *context);
};


/**
 *  @struct baron_allocator_t
 *
 *  This defines a generic allocator used by Baron to allocate objects
 */
struct baron_allocator_t {
    const baron_allocator_fns_t *allocator_fns;
    void *context;
};


/**
 *  @struct baron_desc_t
 *
 *  This describes the environment which will be used to assemble source code
 */
struct baron_desc_t {
    const baron_allocator_t *allocator;
};


/**
 *  @struct baron_object_code_t
 * 
 *  Holds a reference to a block of memory containing assembled object code
 */
struct baron_object_code_t {
    const uint8_t *data;
    size_t size;
};


/**
 *  @enum   
 */
typedef enum baron_value_type_t {
    baron_value_none,
    baron_value_numeric,
    baron_value_string,
    baron_value_list
} baron_value_type_t;


/**
 *  Assemble the given text
 * 
 *  @param  desc        Description of the environment to be used to assemble the text
 *  @param  text        Zero-terminated string to be assembled
 * 
 *  @result A baron_assembly_t from which the object code, symbol table and logs can be obtained
 */
baron_assembly_t *baron_assemble(const baron_desc_t *desc, const char *text);


/**
 *  Assemble from the given file
 *
 *  @param  desc        Description of the environment to be used to assemble the text
 *  @param  filename    Zero-terminated filename of the source file to assemble
 * 
 *  @result A baron_assembly_t from which the object code, symbol table and logs can be obtained
 */
baron_assembly_t *baron_assemble_from_file(const baron_desc_t *desc, const char *filename);


/**
 *  Destroy a baron_assembly_t object
 * 
 *  @param  baron_assembly  The object to destroy
 */
void baron_assembly_destroy(baron_assembly_t *baron_assembly);


/**
 *  Determine the status of the baron_assembly_t object.
 *  A status of 0 indicates a valid, successful assembly.
 *  Any other value indicates an error state.
 * 
 *  @param  baron_assembly  The baron_assembly_t object to inspect
 */
int baron_assembly_status(const baron_assembly_t *baron_assembly);



/**
 *  Get the object code from the baron_assembly corresponding to the given overlay
 *  
 *  @param  baron_assembly  Pointer to the object holding the result of the assembly
 *  @param  overlay_name    Name of the overlay whose object code is required. NULL will return the default overlay.
 * 
 *  @return baron_object_code_t holding a reference to the memory containing the object code
 *          It has the same lifetime as the baron_assembly_t object.
 */
baron_object_code_t baron_assembly_object_code(const baron_assembly_t *baron_assembly, const char *overlay_name);


/**
 *  Get the text corresponding to the error log
 *  
 *  @param  baron_assembly  Pointer to the object holding the result of the assembly
 */
const char *baron_assembly_errors(const baron_assembly_t *baron_assembly);


/**
 *  Get the text corresponding to the nth log channel
 *  
 *  @param  baron_assembly  Pointer to the object holding the result of the assembly
 */
const char *baron_assembly_log(const baron_assembly_t *baron_assembly, int log_number);


/**
 *  Get the type of the named symbol
 *  
 *  @param  baron_assembly  Pointer to the object holding the result of the assembly
 *  @param  symbol_name     Name of the symbol to query. This may be a scope qualified name, separated by periods.
 */
baron_value_type_t baron_assembly_symbol_type(const baron_assembly_t *baron_assembly, const char *symbol_name);


/**
 *  Get a numeric symbol by name, if it exists
 * 
 *  @param  baron_assembly  Pointer to the object holding the result of the assembly
 *  @param  symbol_name     Name of the symbol to query. This may be a scope qualified name, separated by periods.
 */
const double *baron_assembly_symbol_numeric(const baron_assembly_t *baron_assembly, const char *symbol_name);


/**
 *  Get a string symbol by name, if it exists
 * 
 *  @param  baron_assembly  Pointer to the object holding the result of the assembly
 *  @param  symbol_name     Name of the symbol to query. This may be a scope qualified name, separated by periods.
 */
const char *baron_assembly_symbol_string(const baron_assembly_t *baron_assembly, const char *symbol_name);



#endif // ifndef BARONLIB_BARON_H_
