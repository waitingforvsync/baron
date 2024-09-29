#include <stdio.h>
#include "defines.h"
#include "file.h"


file_load_result_t file_load(const allocator_t *allocator, const char *filename) {
    UNUSED(allocator);
    UNUSED(filename);
    file_load_result_t result = {0};
    return result;
}


file_error_t file_save(const char *filename, slice_const_uint8_t data) {
    UNUSED(filename);
    UNUSED(data);
    file_error_t result = {0};
    return result;
}
