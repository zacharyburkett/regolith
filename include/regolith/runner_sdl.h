#ifndef REGOLITH_RUNNER_SDL_H
#define REGOLITH_RUNNER_SDL_H

#include <stdint.h>

#include "regolith/world.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rg_sdl_runner_s rg_sdl_runner_t;

rg_status_t rg_sdl_runner_create(uint32_t worker_count, rg_sdl_runner_t** out_runner);
void rg_sdl_runner_destroy(rg_sdl_runner_t* runner);
const rg_runner_t* rg_sdl_runner_get_runner(const rg_sdl_runner_t* runner);

#ifdef __cplusplus
}
#endif

#endif
