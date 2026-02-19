#ifndef REGOLITH_WORLD_H
#define REGOLITH_WORLD_H

#include <stddef.h>
#include <stdint.h>

#include "regolith/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t rg_material_id_t;

typedef struct rg_world_s rg_world_t;
typedef struct rg_runner_s rg_runner_t;
typedef struct rg_update_ctx_s rg_update_ctx_t;

typedef void* (*rg_alloc_fn)(void* user, size_t size, size_t align);
typedef void (*rg_free_fn)(void* user, void* ptr, size_t size, size_t align);

typedef struct rg_allocator_s {
    rg_alloc_fn alloc;
    rg_free_fn free;
    void* user;
} rg_allocator_t;

typedef void (*rg_parallel_task_fn)(
    uint32_t task_index,
    uint32_t worker_index,
    void* user_data);

typedef rg_status_t (*rg_runner_parallel_for_fn)(
    void* runner_user,
    uint32_t task_count,
    rg_parallel_task_fn task,
    void* task_user_data);

typedef uint32_t (*rg_runner_worker_count_fn)(void* runner_user);

typedef struct rg_runner_vtable_s {
    rg_runner_parallel_for_fn parallel_for;
    rg_runner_worker_count_fn worker_count;
} rg_runner_vtable_t;

struct rg_runner_s {
    const rg_runner_vtable_t* vtable;
    void* user;
};

typedef enum rg_step_mode_e {
    RG_STEP_MODE_FULL_SCAN_SERIAL = 0,
    RG_STEP_MODE_CHUNK_SCAN_SERIAL = 1,
    RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL = 2
} rg_step_mode_t;

typedef enum rg_material_flags_e {
    RG_MATERIAL_STATIC = 1u << 0,
    RG_MATERIAL_SOLID = 1u << 1,
    RG_MATERIAL_POWDER = 1u << 2,
    RG_MATERIAL_LIQUID = 1u << 3,
    RG_MATERIAL_GAS = 1u << 4,
    RG_MATERIAL_CUSTOM_UPDATE = 1u << 5
} rg_material_flags_t;

typedef struct rg_cell_coord_s {
    int32_t x;
    int32_t y;
} rg_cell_coord_t;

typedef struct rg_world_config_s {
    int32_t chunk_width;
    int32_t chunk_height;
    uint16_t inline_payload_bytes;
    uint16_t max_materials;
    uint32_t initial_chunk_capacity;
    uint64_t deterministic_seed;
    uint8_t deterministic_mode;
    rg_step_mode_t default_step_mode;
    rg_allocator_t allocator;
    const rg_runner_t* runner;
} rg_world_config_t;

typedef void (*rg_material_update_fn)(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t material_id,
    void* instance_data,
    void* user_data);

typedef void (*rg_instance_ctor_fn)(void* dst, void* user_data);
typedef void (*rg_instance_dtor_fn)(void* dst, void* user_data);
typedef void (*rg_instance_move_fn)(void* dst, const void* src, void* user_data);

typedef struct rg_material_desc_s {
    const char* name;
    uint32_t flags;
    float density;
    float friction;
    float dispersion;
    uint16_t instance_size;
    uint16_t instance_align;
    rg_instance_ctor_fn instance_ctor;
    rg_instance_dtor_fn instance_dtor;
    rg_instance_move_fn instance_move;
    rg_material_update_fn update_fn;
    void* user_data;
} rg_material_desc_t;

typedef struct rg_cell_read_s {
    rg_material_id_t material_id;
    const void* instance_data;
} rg_cell_read_t;

typedef struct rg_cell_write_s {
    rg_material_id_t material_id;
    const void* instance_data;
} rg_cell_write_t;

typedef struct rg_step_options_s {
    rg_step_mode_t mode;
    uint32_t substeps;
} rg_step_options_t;

typedef struct rg_world_stats_s {
    uint32_t loaded_chunks;
    uint32_t active_chunks;
    uint64_t live_cells;
    uint64_t step_index;
    uint64_t intents_emitted_last_step;
    uint64_t intent_conflicts_last_step;
    uint64_t payload_overflow_allocs;
    uint64_t payload_overflow_frees;
} rg_world_stats_t;

rg_status_t rg_world_create(const rg_world_config_t* cfg, rg_world_t** out_world);
void rg_world_destroy(rg_world_t* world);

rg_status_t rg_material_register(
    rg_world_t* world,
    const rg_material_desc_t* desc,
    rg_material_id_t* out_material_id);

rg_status_t rg_chunk_load(rg_world_t* world, int32_t chunk_x, int32_t chunk_y);
rg_status_t rg_chunk_unload(rg_world_t* world, int32_t chunk_x, int32_t chunk_y);

rg_status_t rg_cell_get(
    const rg_world_t* world,
    rg_cell_coord_t cell,
    rg_cell_read_t* out_cell);

rg_status_t rg_cell_set(
    rg_world_t* world,
    rg_cell_coord_t cell,
    const rg_cell_write_t* value);

rg_status_t rg_cell_clear(rg_world_t* world, rg_cell_coord_t cell);

rg_status_t rg_world_step(rg_world_t* world, const rg_step_options_t* options);

rg_status_t rg_world_get_stats(const rg_world_t* world, rg_world_stats_t* out_stats);

rg_status_t rg_ctx_try_move(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t from,
    rg_cell_coord_t to);

rg_status_t rg_ctx_try_swap(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t a,
    rg_cell_coord_t b);

rg_status_t rg_ctx_transform(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t new_material,
    const void* new_instance_data);

uint32_t rg_ctx_random_u32(rg_update_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif
