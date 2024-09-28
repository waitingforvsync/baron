#ifndef BITARRAY_H_
#define BITARRAY_H_

#include "base/array.h"

typedef struct bitarray_t bitarray_t;

struct bitarray_t {
    array_uint64_t bits;
};


#endif // ifndef BITARRAY_H_
