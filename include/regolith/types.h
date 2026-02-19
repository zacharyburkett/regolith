#ifndef REGOLITH_TYPES_H
#define REGOLITH_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rg_status_e {
    RG_STATUS_OK = 0,
    RG_STATUS_INVALID_ARGUMENT = 1,
    RG_STATUS_NOT_FOUND = 2,
    RG_STATUS_ALREADY_EXISTS = 3,
    RG_STATUS_CAPACITY_REACHED = 4,
    RG_STATUS_ALLOCATION_FAILED = 5,
    RG_STATUS_OUT_OF_BOUNDS = 6,
    RG_STATUS_CONFLICT = 7,
    RG_STATUS_UNSUPPORTED = 8
} rg_status_t;

const char* rg_status_string(rg_status_t status);

#ifdef __cplusplus
}
#endif

#endif
