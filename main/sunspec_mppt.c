#include "sunspec_mppt.h"

#include <string.h>

static bool is_storage_id(const char *id)
{
    return !strncmp(id, "StCha", 5) || !strncmp(id, "StDisCha", 8);
}

uint16_t sunspec_mppt_pv_mask(const char ids[][17], uint16_t n, bool hybrid)
{
    if (n > SUNSPEC_MPPT_MAX) n = SUNSPEC_MPPT_MAX;
    uint16_t mask = 0;
    bool named = false;
    for (uint16_t i = 0; i < n; i++) {
        if (is_storage_id(ids[i])) named = true;
        else mask |= (uint16_t)(1u << i);
    }
    if (!named && hybrid && n >= 2) mask &= (uint16_t)~(1u << (n - 1));
    return mask;
}
