#include "regolith/regolith.h"

const char* rg_status_string(rg_status_t status)
{
    switch (status) {
    case RG_STATUS_OK:
        return "ok";
    case RG_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case RG_STATUS_NOT_FOUND:
        return "not found";
    case RG_STATUS_ALREADY_EXISTS:
        return "already exists";
    case RG_STATUS_CAPACITY_REACHED:
        return "capacity reached";
    case RG_STATUS_ALLOCATION_FAILED:
        return "allocation failed";
    case RG_STATUS_OUT_OF_BOUNDS:
        return "out of bounds";
    case RG_STATUS_CONFLICT:
        return "conflict";
    case RG_STATUS_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown status";
    }
}
