/**
 *  @file   sref.h
 * 
 *  Support for unmanaged references to strings.
 *  Represented as a pointer and length tuple.
 */

#ifndef SREF_H_
#define SREF_H_


#include <stdint.h>

typedef struct sref_t sref_t;


struct sref_t {
    const char *ptr;
    uint32_t length;
};





#endif // SREF_H_
