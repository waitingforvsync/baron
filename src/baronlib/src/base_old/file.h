/**
 *  @file   file.h
 * 
 *  Support for loading and saving binary files
 */

#ifndef FILE_H_
#define FILE_H_


#include <stdint.h>
#include "sref.h"

typedef struct file_error_t file_error_t;
typedef struct file_load_result_t file_load_result_t;
typedef struct allocator_t allocator_t;


struct file_error_t {
    uint32_t type;
};


struct file_load_result_t {
    sref_t data;
    file_error_t error;
};


/**
 *  Loads the named file into memory, using the designated allocator to allocate the required space.
 *  
 *  @param  allocator       Allocator to use to allocate the required space for the file
 *  @param  filename        Zero-terminated filename to load
 * 
 *  @return On success, the data field is a sref to the loaded data, and the error field is zero.
 *          On failutre, the data field is invalid, and the error field is a value representing the type of error.
 */
file_load_result_t file_load(const allocator_t *allocator, const char *filename);


/**
 *  Saves the block of data referenced by the sref to the given file
 * 
 *  @param  filenamem       The filename to save to
 *  @param  data            A sref to the data to be saved
 * 
 *  @return Any error which occurred while attempting to save the file
 */
file_error_t file_save(const char *filename, sref_t data);


#endif // ifndef FILE_H_
