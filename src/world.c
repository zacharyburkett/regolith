#include "regolith/world.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

enum {
    RG_DEFAULT_CHUNK_WIDTH = 64,
    RG_DEFAULT_CHUNK_HEIGHT = 64,
    RG_DEFAULT_INLINE_PAYLOAD_BYTES = 16,
    RG_DEFAULT_MAX_MATERIALS = 256,
    RG_DEFAULT_INITIAL_CHUNKS = 16,
    RG_CHUNK_SLEEP_TICKS = 8
};

typedef struct rg_material_record_s {
    char* name;
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
    uint8_t used;
} rg_material_record_t;

typedef struct rg_chunk_s {
    uint16_t* material_ids;
    uint8_t* inline_payload;
    uint8_t* updated_mask;
    uint32_t live_cells;
    uint32_t idle_steps;
    uint8_t awake;
} rg_chunk_t;

typedef struct rg_chunk_entry_s {
    int32_t chunk_x;
    int32_t chunk_y;
    rg_chunk_t* chunk;
} rg_chunk_entry_t;

struct rg_world_s {
    rg_allocator_t allocator;
    const rg_runner_t* runner;
    rg_step_mode_t default_step_mode;
    uint64_t deterministic_seed;
    uint8_t deterministic_mode;

    int32_t chunk_width;
    int32_t chunk_height;
    uint32_t cells_per_chunk;
    uint16_t inline_payload_bytes;
    uint16_t max_materials;
    uint8_t* swap_payload;

    rg_material_record_t* materials;
    rg_material_id_t material_count;

    rg_chunk_entry_t* chunks;
    uint32_t chunk_count;
    uint32_t chunk_capacity;

    uint32_t active_chunk_count;
    uint64_t live_cells;
    uint64_t step_index;
    uint64_t intents_emitted_last_step;
    uint64_t intent_conflicts_last_step;
    uint64_t payload_overflow_allocs;
    uint64_t payload_overflow_frees;
};

typedef struct rg_cross_intent_s {
    uint32_t source_chunk_index;
    uint32_t target_chunk_index;
    uint32_t source_cell_index;
    uint32_t target_cell_index;
    rg_material_id_t source_material_id;
    rg_material_id_t target_material_id;
} rg_cross_intent_t;

typedef struct rg_task_output_s {
    rg_cross_intent_t* intents;
    uint32_t intent_count;
    uint32_t intent_capacity;
    uint64_t emitted_move_count;
    uint8_t changed;
} rg_task_output_t;

typedef struct rg_checkerboard_task_ctx_s {
    rg_world_t* world;
    uint64_t tick;
    const uint32_t* chunk_indices;
    uint32_t chunk_count;
    rg_task_output_t* outputs;
} rg_checkerboard_task_ctx_t;

struct rg_update_ctx_s {
    rg_world_t* world;
    uint64_t tick;
    uint32_t source_chunk_index;
    rg_chunk_entry_t* source_entry;
    int32_t source_local_x;
    int32_t source_local_y;
    uint32_t source_cell_index;
    rg_cell_coord_t source_cell;
    uint8_t emit_cross_intents;
    rg_task_output_t* task_output;
    uint32_t random_counter;
    uint8_t operation_done;
    uint8_t changed;
};

static int rg_is_power_of_two_u32(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static uint64_t rg_mix_u64(uint64_t value)
{
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return value;
}

static int32_t rg_abs_i32(int32_t value)
{
    return (value < 0) ? -value : value;
}

static void* rg_default_alloc(void* user, size_t size, size_t align)
{
    void* ptr;

    (void)user;

    if (size == 0u) {
        return NULL;
    }

    if (align <= sizeof(void*)) {
        return malloc(size);
    }

#if defined(_MSC_VER)
    ptr = _aligned_malloc(size, align);
    return ptr;
#else
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    if (!rg_is_power_of_two_u32((uint32_t)align)) {
        return NULL;
    }
    if (posix_memalign(&ptr, align, size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

static void rg_default_free(void* user, void* ptr, size_t size, size_t align)
{
    (void)user;
    (void)size;
    (void)align;

#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static rg_status_t rg_prepare_allocator(
    const rg_allocator_t* allocator,
    rg_allocator_t* out_allocator)
{
    if (out_allocator == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    memset(out_allocator, 0, sizeof(*out_allocator));
    if (allocator != NULL) {
        *out_allocator = *allocator;
    }

    if ((out_allocator->alloc == NULL) != (out_allocator->free == NULL)) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    if (out_allocator->alloc == NULL) {
        out_allocator->alloc = rg_default_alloc;
        out_allocator->free = rg_default_free;
    }

    return RG_STATUS_OK;
}

static void* rg_alloc_bytes(rg_allocator_t* allocator, size_t size, size_t align)
{
    if (allocator == NULL || allocator->alloc == NULL || size == 0u) {
        return NULL;
    }
    return allocator->alloc(allocator->user, size, align);
}

static void rg_free_bytes(rg_allocator_t* allocator, void* ptr, size_t size, size_t align)
{
    if (allocator == NULL || allocator->free == NULL || ptr == NULL) {
        return;
    }
    allocator->free(allocator->user, ptr, size, align);
}

static char* rg_strdup_with_allocator(rg_allocator_t* allocator, const char* text)
{
    size_t len;
    char* copy;

    if (allocator == NULL || text == NULL) {
        return NULL;
    }

    len = strlen(text);
    copy = (char*)rg_alloc_bytes(allocator, len + 1u, 1u);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, len + 1u);
    return copy;
}

static void rg_split_coord(
    int32_t value,
    int32_t chunk_extent,
    int32_t* out_chunk_coord,
    int32_t* out_local_coord)
{
    int32_t local;

    local = value % chunk_extent;
    if (local < 0) {
        local += chunk_extent;
    }

    if (out_local_coord != NULL) {
        *out_local_coord = local;
    }
    if (out_chunk_coord != NULL) {
        *out_chunk_coord = (value - local) / chunk_extent;
    }
}

static uint8_t rg_chunk_coord_less(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    if (ay < by) {
        return 1u;
    }
    if (ay > by) {
        return 0u;
    }
    return (uint8_t)(ax < bx);
}

static uint32_t rg_chunk_find_index(const rg_world_t* world, int32_t chunk_x, int32_t chunk_y)
{
    uint32_t i;

    if (world == NULL) {
        return UINT32_MAX;
    }

    for (i = 0u; i < world->chunk_count; ++i) {
        if (world->chunks[i].chunk_x == chunk_x && world->chunks[i].chunk_y == chunk_y) {
            return i;
        }
    }

    return UINT32_MAX;
}

static rg_status_t rg_chunk_reserve(rg_world_t* world, uint32_t min_capacity)
{
    uint32_t new_capacity;
    rg_chunk_entry_t* new_entries;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    if (world->chunk_capacity >= min_capacity) {
        return RG_STATUS_OK;
    }

    new_capacity = (world->chunk_capacity == 0u) ? RG_DEFAULT_INITIAL_CHUNKS : world->chunk_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2u) {
            return RG_STATUS_CAPACITY_REACHED;
        }
        new_capacity *= 2u;
    }

    new_entries = (rg_chunk_entry_t*)rg_alloc_bytes(
        &world->allocator,
        (size_t)new_capacity * sizeof(*new_entries),
        _Alignof(rg_chunk_entry_t));
    if (new_entries == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }

    if (world->chunk_count > 0u) {
        memcpy(new_entries, world->chunks, (size_t)world->chunk_count * sizeof(*new_entries));
    }

    rg_free_bytes(
        &world->allocator,
        world->chunks,
        (size_t)world->chunk_capacity * sizeof(*new_entries),
        _Alignof(rg_chunk_entry_t));

    world->chunks = new_entries;
    world->chunk_capacity = new_capacity;
    return RG_STATUS_OK;
}

static void rg_chunk_set_awake(rg_world_t* world, rg_chunk_t* chunk, uint8_t awake)
{
    if (world == NULL || chunk == NULL) {
        return;
    }

    awake = (uint8_t)(awake != 0u);
    if (chunk->awake == awake) {
        return;
    }

    chunk->awake = awake;
    if (awake != 0u) {
        world->active_chunk_count += 1u;
    } else if (world->active_chunk_count > 0u) {
        world->active_chunk_count -= 1u;
    }
}

static void rg_recompute_active_chunk_count(rg_world_t* world)
{
    uint32_t count;
    uint32_t i;

    if (world == NULL) {
        return;
    }

    count = 0u;
    for (i = 0u; i < world->chunk_count; ++i) {
        rg_chunk_t* chunk;

        chunk = world->chunks[i].chunk;
        if (chunk != NULL && chunk->awake != 0u) {
            count += 1u;
        }
    }

    world->active_chunk_count = count;
}

static void rg_set_chunk_awake_for_mode(
    rg_world_t* world,
    rg_chunk_t* chunk,
    uint8_t awake,
    rg_task_output_t* task_output)
{
    if (chunk == NULL) {
        return;
    }

    if (task_output != NULL) {
        chunk->awake = (uint8_t)(awake != 0u);
    } else {
        rg_chunk_set_awake(world, chunk, awake);
    }
}

static uint8_t rg_mask_test(const rg_chunk_t* chunk, uint32_t cell_index)
{
    uint32_t byte_index;
    uint32_t bit_index;

    if (chunk == NULL || chunk->updated_mask == NULL) {
        return 0u;
    }

    byte_index = cell_index >> 3u;
    bit_index = cell_index & 7u;
    return (uint8_t)((chunk->updated_mask[byte_index] >> bit_index) & 1u);
}

static void rg_mask_set(rg_chunk_t* chunk, uint32_t cell_index)
{
    uint32_t byte_index;
    uint32_t bit_index;

    if (chunk == NULL || chunk->updated_mask == NULL) {
        return;
    }

    byte_index = cell_index >> 3u;
    bit_index = cell_index & 7u;
    chunk->updated_mask[byte_index] = (uint8_t)(chunk->updated_mask[byte_index] | (uint8_t)(1u << bit_index));
}

static void rg_mask_clear_all(rg_world_t* world, rg_chunk_t* chunk)
{
    size_t mask_bytes;

    if (world == NULL || chunk == NULL || chunk->updated_mask == NULL) {
        return;
    }

    mask_bytes = ((size_t)world->cells_per_chunk + 7u) / 8u;
    memset(chunk->updated_mask, 0, mask_bytes);
}

static void rg_prepare_step_masks(rg_world_t* world)
{
    uint32_t i;

    if (world == NULL) {
        return;
    }

    for (i = 0u; i < world->chunk_count; ++i) {
        rg_mask_clear_all(world, world->chunks[i].chunk);
    }
}

static const rg_material_record_t* rg_material_get(
    const rg_world_t* world,
    rg_material_id_t material_id)
{
    if (world == NULL || material_id == 0u || material_id > world->material_count) {
        return NULL;
    }
    if (world->materials[material_id].used == 0u) {
        return NULL;
    }
    return &world->materials[material_id];
}

static void* rg_chunk_payload_ptr(rg_world_t* world, rg_chunk_t* chunk, uint32_t cell_index)
{
    if (world == NULL || chunk == NULL || chunk->inline_payload == NULL || world->inline_payload_bytes == 0u) {
        return NULL;
    }
    return chunk->inline_payload + ((size_t)cell_index * (size_t)world->inline_payload_bytes);
}

static const void* rg_chunk_payload_ptr_const(
    const rg_world_t* world,
    const rg_chunk_t* chunk,
    uint32_t cell_index)
{
    if (world == NULL || chunk == NULL || chunk->inline_payload == NULL || world->inline_payload_bytes == 0u) {
        return NULL;
    }
    return chunk->inline_payload + ((size_t)cell_index * (size_t)world->inline_payload_bytes);
}

static void rg_release_cell_instance(
    rg_world_t* world,
    rg_chunk_t* chunk,
    uint32_t cell_index,
    const rg_material_record_t* material)
{
    void* payload;

    if (world == NULL || chunk == NULL || material == NULL || material->instance_size == 0u) {
        return;
    }

    payload = rg_chunk_payload_ptr(world, chunk, cell_index);
    if (payload == NULL) {
        return;
    }

    if (material->instance_dtor != NULL) {
        material->instance_dtor(payload, material->user_data);
    }
    memset(payload, 0, (size_t)world->inline_payload_bytes);
}

static rg_status_t rg_write_cell_instance(
    rg_world_t* world,
    rg_chunk_t* chunk,
    uint32_t cell_index,
    const rg_material_record_t* material,
    const void* instance_data)
{
    void* payload;

    if (world == NULL || chunk == NULL || material == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    if (material->instance_size == 0u) {
        return RG_STATUS_OK;
    }

    payload = rg_chunk_payload_ptr(world, chunk, cell_index);
    if (payload == NULL) {
        return RG_STATUS_UNSUPPORTED;
    }

    memset(payload, 0, (size_t)world->inline_payload_bytes);
    if (instance_data != NULL) {
        memmove(payload, instance_data, material->instance_size);
    } else if (material->instance_ctor != NULL) {
        material->instance_ctor(payload, material->user_data);
    }

    return RG_STATUS_OK;
}

static void rg_update_live_counts(
    rg_world_t* world,
    rg_chunk_t* chunk,
    rg_material_id_t old_material,
    rg_material_id_t new_material)
{
    if (world == NULL || chunk == NULL) {
        return;
    }

    if (old_material == 0u && new_material != 0u) {
        world->live_cells += 1u;
        chunk->live_cells += 1u;
        chunk->idle_steps = 0u;
        rg_chunk_set_awake(world, chunk, 1u);
        return;
    }

    if (old_material != 0u && new_material == 0u) {
        if (world->live_cells > 0u) {
            world->live_cells -= 1u;
        }
        if (chunk->live_cells > 0u) {
            chunk->live_cells -= 1u;
        }

        chunk->idle_steps = 0u;
        if (chunk->live_cells == 0u) {
            rg_chunk_set_awake(world, chunk, 0u);
        } else {
            rg_chunk_set_awake(world, chunk, 1u);
        }
        return;
    }

    if (old_material != 0u && new_material != 0u) {
        chunk->idle_steps = 0u;
        rg_chunk_set_awake(world, chunk, 1u);
    }
}

static rg_status_t rg_locate_cell(
    const rg_world_t* world,
    rg_cell_coord_t cell,
    uint32_t* out_chunk_index,
    uint32_t* out_cell_index)
{
    int32_t local_x;
    int32_t local_y;
    int32_t chunk_x;
    int32_t chunk_y;
    uint32_t chunk_index;

    if (world == NULL || out_chunk_index == NULL || out_cell_index == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    rg_split_coord(cell.x, world->chunk_width, &chunk_x, &local_x);
    rg_split_coord(cell.y, world->chunk_height, &chunk_y, &local_y);

    chunk_index = rg_chunk_find_index(world, chunk_x, chunk_y);
    if (chunk_index == UINT32_MAX) {
        return RG_STATUS_NOT_FOUND;
    }

    *out_chunk_index = chunk_index;
    *out_cell_index = ((uint32_t)local_y * (uint32_t)world->chunk_width) + (uint32_t)local_x;
    return RG_STATUS_OK;
}

static rg_status_t rg_chunk_create(rg_world_t* world, rg_chunk_t** out_chunk)
{
    rg_chunk_t* chunk;
    size_t material_bytes;
    size_t payload_bytes;
    size_t mask_bytes;

    if (world == NULL || out_chunk == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    *out_chunk = NULL;

    chunk = (rg_chunk_t*)rg_alloc_bytes(&world->allocator, sizeof(*chunk), _Alignof(rg_chunk_t));
    if (chunk == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }
    memset(chunk, 0, sizeof(*chunk));

    material_bytes = (size_t)world->cells_per_chunk * sizeof(*chunk->material_ids);
    chunk->material_ids = (uint16_t*)rg_alloc_bytes(
        &world->allocator,
        material_bytes,
        _Alignof(uint16_t));
    if (chunk->material_ids == NULL) {
        rg_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(rg_chunk_t));
        return RG_STATUS_ALLOCATION_FAILED;
    }
    memset(chunk->material_ids, 0, material_bytes);

    payload_bytes = (size_t)world->cells_per_chunk * (size_t)world->inline_payload_bytes;
    if (payload_bytes > 0u) {
        chunk->inline_payload = (uint8_t*)rg_alloc_bytes(&world->allocator, payload_bytes, 1u);
        if (chunk->inline_payload == NULL) {
            rg_free_bytes(&world->allocator, chunk->material_ids, material_bytes, _Alignof(uint16_t));
            rg_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(rg_chunk_t));
            return RG_STATUS_ALLOCATION_FAILED;
        }
        memset(chunk->inline_payload, 0, payload_bytes);
    }

    mask_bytes = ((size_t)world->cells_per_chunk + 7u) / 8u;
    chunk->updated_mask = (uint8_t*)rg_alloc_bytes(&world->allocator, mask_bytes, 1u);
    if (chunk->updated_mask == NULL) {
        rg_free_bytes(&world->allocator, chunk->inline_payload, payload_bytes, 1u);
        rg_free_bytes(&world->allocator, chunk->material_ids, material_bytes, _Alignof(uint16_t));
        rg_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(rg_chunk_t));
        return RG_STATUS_ALLOCATION_FAILED;
    }
    memset(chunk->updated_mask, 0, mask_bytes);
    chunk->live_cells = 0u;
    chunk->idle_steps = 0u;
    chunk->awake = 0u;

    *out_chunk = chunk;
    return RG_STATUS_OK;
}

static void rg_chunk_destroy(rg_world_t* world, rg_chunk_t* chunk)
{
    size_t material_bytes;
    size_t payload_bytes;
    size_t mask_bytes;
    uint32_t i;

    if (world == NULL || chunk == NULL) {
        return;
    }

    for (i = 0u; i < world->cells_per_chunk; ++i) {
        rg_material_id_t material_id;
        const rg_material_record_t* material;

        material_id = chunk->material_ids[i];
        if (material_id == 0u) {
            continue;
        }

        material = rg_material_get(world, material_id);
        if (material != NULL) {
            rg_release_cell_instance(world, chunk, i, material);
        }
    }

    material_bytes = (size_t)world->cells_per_chunk * sizeof(*chunk->material_ids);
    payload_bytes = (size_t)world->cells_per_chunk * (size_t)world->inline_payload_bytes;
    mask_bytes = ((size_t)world->cells_per_chunk + 7u) / 8u;

    rg_free_bytes(&world->allocator, chunk->updated_mask, mask_bytes, 1u);
    rg_free_bytes(&world->allocator, chunk->inline_payload, payload_bytes, 1u);
    rg_free_bytes(&world->allocator, chunk->material_ids, material_bytes, _Alignof(uint16_t));
    rg_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(rg_chunk_t));
}

static uint32_t rg_chunk_insert_index(const rg_world_t* world, int32_t chunk_x, int32_t chunk_y)
{
    uint32_t i;

    if (world == NULL) {
        return 0u;
    }

    for (i = 0u; i < world->chunk_count; ++i) {
        if (rg_chunk_coord_less(chunk_x, chunk_y, world->chunks[i].chunk_x, world->chunks[i].chunk_y) != 0u) {
            return i;
        }
    }
    return world->chunk_count;
}

static uint32_t rg_step_random(
    const rg_world_t* world,
    uint64_t tick,
    int32_t chunk_x,
    int32_t chunk_y,
    int32_t local_x,
    int32_t local_y,
    uint32_t salt)
{
    uint64_t seed;
    uint64_t key;

    if (world == NULL) {
        return 0u;
    }

    seed = world->deterministic_seed;
    if (world->deterministic_mode == 0u) {
        seed ^= (uint64_t)(uintptr_t)world;
    }

    key = seed;
    key ^= tick * 0x9e3779b97f4a7c15ull;
    key ^= ((uint64_t)(uint32_t)chunk_x << 32u) ^ (uint64_t)(uint32_t)chunk_y;
    key ^= ((uint64_t)(uint32_t)local_x << 32u) ^ (uint64_t)(uint32_t)local_y;
    key ^= (uint64_t)salt * 0xd6e8feb86659fd93ull;

    return (uint32_t)rg_mix_u64(key);
}

static rg_status_t rg_resolve_target(
    const rg_world_t* world,
    const rg_chunk_entry_t* source_entry,
    int32_t source_local_x,
    int32_t source_local_y,
    int32_t dx,
    int32_t dy,
    rg_chunk_entry_t** out_target_entry,
    uint32_t* out_target_chunk_index,
    uint32_t* out_target_index,
    int32_t* out_target_local_x,
    int32_t* out_target_local_y)
{
    int32_t target_chunk_x;
    int32_t target_chunk_y;
    int32_t target_local_x;
    int32_t target_local_y;
    uint32_t chunk_index;

    if (world == NULL ||
        source_entry == NULL ||
        out_target_entry == NULL ||
        out_target_chunk_index == NULL ||
        out_target_index == NULL ||
        out_target_local_x == NULL ||
        out_target_local_y == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    target_chunk_x = source_entry->chunk_x;
    target_chunk_y = source_entry->chunk_y;
    target_local_x = source_local_x + dx;
    target_local_y = source_local_y + dy;

    if (target_local_x < 0) {
        target_chunk_x -= 1;
        target_local_x += world->chunk_width;
    } else if (target_local_x >= world->chunk_width) {
        target_chunk_x += 1;
        target_local_x -= world->chunk_width;
    }

    if (target_local_y < 0) {
        target_chunk_y -= 1;
        target_local_y += world->chunk_height;
    } else if (target_local_y >= world->chunk_height) {
        target_chunk_y += 1;
        target_local_y -= world->chunk_height;
    }

    chunk_index = rg_chunk_find_index(world, target_chunk_x, target_chunk_y);
    if (chunk_index == UINT32_MAX) {
        return RG_STATUS_NOT_FOUND;
    }

    *out_target_entry = &((rg_world_t*)world)->chunks[chunk_index];
    *out_target_chunk_index = chunk_index;
    *out_target_local_x = target_local_x;
    *out_target_local_y = target_local_y;
    *out_target_index = ((uint32_t)target_local_y * (uint32_t)world->chunk_width) + (uint32_t)target_local_x;
    return RG_STATUS_OK;
}

static uint8_t rg_can_displace(
    const rg_material_record_t* source_material,
    const rg_material_record_t* target_material,
    int32_t dy,
    uint8_t allow_lateral_displace)
{
    if (source_material == NULL || target_material == NULL) {
        return 0u;
    }
    if ((target_material->flags & RG_MATERIAL_STATIC) != 0u) {
        return 0u;
    }

    if (dy > 0) {
        return (uint8_t)(source_material->density > target_material->density);
    }
    if (dy < 0) {
        return (uint8_t)(source_material->density < target_material->density);
    }
    if (allow_lateral_displace != 0u) {
        return (uint8_t)(source_material->density != target_material->density);
    }
    return 0u;
}

static uint8_t rg_task_output_push_intent(rg_task_output_t* output, const rg_cross_intent_t* intent)
{
    rg_cross_intent_t* new_intents;
    uint32_t new_capacity;

    if (output == NULL || intent == NULL) {
        return 0u;
    }

    if (output->intent_count >= output->intent_capacity) {
        new_capacity = (output->intent_capacity == 0u) ? 16u : (output->intent_capacity * 2u);
        new_intents = (rg_cross_intent_t*)realloc(output->intents, (size_t)new_capacity * sizeof(*new_intents));
        if (new_intents == NULL) {
            return 0u;
        }
        output->intents = new_intents;
        output->intent_capacity = new_capacity;
    }

    output->intents[output->intent_count] = *intent;
    output->intent_count += 1u;
    return 1u;
}

static void rg_task_output_release(rg_task_output_t* output)
{
    if (output == NULL) {
        return;
    }

    free(output->intents);
    memset(output, 0, sizeof(*output));
}

static int rg_intent_compare_by_target(const void* lhs_void, const void* rhs_void)
{
    const rg_cross_intent_t* lhs;
    const rg_cross_intent_t* rhs;

    lhs = (const rg_cross_intent_t*)lhs_void;
    rhs = (const rg_cross_intent_t*)rhs_void;

    if (lhs->target_chunk_index < rhs->target_chunk_index) {
        return -1;
    }
    if (lhs->target_chunk_index > rhs->target_chunk_index) {
        return 1;
    }
    if (lhs->target_cell_index < rhs->target_cell_index) {
        return -1;
    }
    if (lhs->target_cell_index > rhs->target_cell_index) {
        return 1;
    }
    if (lhs->source_chunk_index < rhs->source_chunk_index) {
        return -1;
    }
    if (lhs->source_chunk_index > rhs->source_chunk_index) {
        return 1;
    }
    if (lhs->source_cell_index < rhs->source_cell_index) {
        return -1;
    }
    if (lhs->source_cell_index > rhs->source_cell_index) {
        return 1;
    }
    return 0;
}

static void rg_payload_swap(
    rg_world_t* world,
    rg_chunk_t* chunk_a,
    uint32_t index_a,
    rg_chunk_t* chunk_b,
    uint32_t index_b)
{
    void* payload_a;
    void* payload_b;

    if (world == NULL ||
        world->inline_payload_bytes == 0u ||
        world->swap_payload == NULL ||
        chunk_a == NULL ||
        chunk_b == NULL) {
        return;
    }

    payload_a = rg_chunk_payload_ptr(world, chunk_a, index_a);
    payload_b = rg_chunk_payload_ptr(world, chunk_b, index_b);
    if (payload_a == NULL || payload_b == NULL) {
        return;
    }

    memmove(world->swap_payload, payload_a, (size_t)world->inline_payload_bytes);
    memmove(payload_a, payload_b, (size_t)world->inline_payload_bytes);
    memmove(payload_b, world->swap_payload, (size_t)world->inline_payload_bytes);
}

static void rg_payload_move(
    rg_world_t* world,
    rg_chunk_t* source_chunk,
    uint32_t source_index,
    rg_chunk_t* target_chunk,
    uint32_t target_index,
    const rg_material_record_t* material)
{
    void* source_payload;
    void* target_payload;

    if (world == NULL || material == NULL || material->instance_size == 0u) {
        return;
    }

    source_payload = rg_chunk_payload_ptr(world, source_chunk, source_index);
    target_payload = rg_chunk_payload_ptr(world, target_chunk, target_index);
    if (source_payload == NULL || target_payload == NULL) {
        return;
    }

    memset(target_payload, 0, (size_t)world->inline_payload_bytes);
    if (material->instance_move != NULL) {
        material->instance_move(target_payload, source_payload, material->user_data);
    } else {
        memmove(target_payload, source_payload, material->instance_size);
    }
    memset(source_payload, 0, (size_t)world->inline_payload_bytes);
}

static uint8_t rg_apply_cross_intent(rg_world_t* world, const rg_cross_intent_t* intent)
{
    rg_chunk_entry_t* source_entry;
    rg_chunk_entry_t* target_entry;
    rg_chunk_t* source_chunk;
    rg_chunk_t* target_chunk;
    rg_material_id_t source_material_id;
    rg_material_id_t target_material_id;
    const rg_material_record_t* source_material;

    if (world == NULL || intent == NULL) {
        return 0u;
    }
    if (intent->source_chunk_index >= world->chunk_count ||
        intent->target_chunk_index >= world->chunk_count ||
        intent->source_cell_index >= world->cells_per_chunk ||
        intent->target_cell_index >= world->cells_per_chunk) {
        return 0u;
    }

    source_entry = &world->chunks[intent->source_chunk_index];
    target_entry = &world->chunks[intent->target_chunk_index];
    source_chunk = source_entry->chunk;
    target_chunk = target_entry->chunk;
    if (source_chunk == NULL || target_chunk == NULL) {
        return 0u;
    }

    source_material_id = source_chunk->material_ids[intent->source_cell_index];
    target_material_id = target_chunk->material_ids[intent->target_cell_index];
    if (source_material_id != intent->source_material_id ||
        target_material_id != intent->target_material_id) {
        return 0u;
    }

    if (intent->target_material_id == 0u) {
        source_material = rg_material_get(world, source_material_id);
        if (source_material == NULL) {
            return 0u;
        }

        target_chunk->material_ids[intent->target_cell_index] = source_material_id;
        source_chunk->material_ids[intent->source_cell_index] = 0u;
        rg_payload_move(
            world,
            source_chunk,
            intent->source_cell_index,
            target_chunk,
            intent->target_cell_index,
            source_material);

        if (source_chunk != target_chunk) {
            if (source_chunk->live_cells > 0u) {
                source_chunk->live_cells -= 1u;
            }
            target_chunk->live_cells += 1u;
        }
    } else {
        target_chunk->material_ids[intent->target_cell_index] = source_material_id;
        source_chunk->material_ids[intent->source_cell_index] = intent->target_material_id;
        rg_payload_swap(
            world,
            source_chunk,
            intent->source_cell_index,
            target_chunk,
            intent->target_cell_index);
    }

    rg_mask_set(target_chunk, intent->target_cell_index);
    source_chunk->idle_steps = 0u;
    target_chunk->idle_steps = 0u;
    source_chunk->awake = (uint8_t)(source_chunk->live_cells > 0u);
    target_chunk->awake = (uint8_t)(target_chunk->live_cells > 0u);
    return 1u;
}

static rg_status_t rg_merge_cross_intents(
    rg_world_t* world,
    rg_task_output_t* outputs,
    uint32_t output_count)
{
    rg_cross_intent_t* merged;
    uint32_t total_intents;
    uint32_t write_cursor;
    uint32_t i;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (outputs == NULL || output_count == 0u) {
        return RG_STATUS_OK;
    }

    total_intents = 0u;
    for (i = 0u; i < output_count; ++i) {
        if (outputs[i].intent_count > UINT32_MAX - total_intents) {
            return RG_STATUS_CAPACITY_REACHED;
        }
        total_intents += outputs[i].intent_count;
    }

    if (total_intents == 0u) {
        return RG_STATUS_OK;
    }

    merged = (rg_cross_intent_t*)malloc((size_t)total_intents * sizeof(*merged));
    if (merged == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }

    write_cursor = 0u;
    for (i = 0u; i < output_count; ++i) {
        if (outputs[i].intent_count == 0u) {
            continue;
        }
        memcpy(
            &merged[write_cursor],
            outputs[i].intents,
            (size_t)outputs[i].intent_count * sizeof(*merged));
        write_cursor += outputs[i].intent_count;
    }

    qsort(merged, (size_t)total_intents, sizeof(*merged), rg_intent_compare_by_target);

    i = 0u;
    while (i < total_intents) {
        uint32_t j;
        uint8_t applied;

        j = i + 1u;
        while (j < total_intents &&
               merged[j].target_chunk_index == merged[i].target_chunk_index &&
               merged[j].target_cell_index == merged[i].target_cell_index) {
            j += 1u;
        }

        if (j > i + 1u) {
            world->intent_conflicts_last_step += (uint64_t)(j - i - 1u);
        }

        applied = 0u;
        while (i < j) {
            if (applied == 0u && rg_apply_cross_intent(world, &merged[i]) != 0u) {
                applied = 1u;
            }
            i += 1u;
        }
    }

    free(merged);
    return RG_STATUS_OK;
}

static uint8_t rg_attempt_move(
    rg_world_t* world,
    uint32_t source_chunk_index,
    rg_chunk_entry_t* source_entry,
    int32_t source_local_x,
    int32_t source_local_y,
    uint32_t source_index,
    rg_material_id_t source_material_id,
    const rg_material_record_t* source_material,
    int32_t dx,
    int32_t dy,
    uint8_t allow_lateral_displace,
    uint8_t emit_cross_intents,
    rg_task_output_t* task_output)
{
    rg_status_t status;
    rg_chunk_entry_t* target_entry;
    uint32_t target_chunk_index;
    int32_t target_local_x;
    int32_t target_local_y;
    uint32_t target_index;
    rg_chunk_t* source_chunk;
    rg_chunk_t* target_chunk;
    rg_material_id_t target_material_id;
    const rg_material_record_t* target_material;

    if (world == NULL || source_entry == NULL || source_material == NULL || source_material_id == 0u) {
        return 0u;
    }

    status = rg_resolve_target(
        world,
        source_entry,
        source_local_x,
        source_local_y,
        dx,
        dy,
        &target_entry,
        &target_chunk_index,
        &target_index,
        &target_local_x,
        &target_local_y);
    if (status != RG_STATUS_OK) {
        return 0u;
    }

    source_chunk = source_entry->chunk;
    target_chunk = target_entry->chunk;
    if (source_chunk == NULL || target_chunk == NULL) {
        return 0u;
    }

    target_material_id = target_chunk->material_ids[target_index];
    if (target_material_id != 0u) {
        target_material = rg_material_get(world, target_material_id);
        if (target_material == NULL) {
            return 0u;
        }
        if (rg_can_displace(source_material, target_material, dy, allow_lateral_displace) == 0u) {
            return 0u;
        }

        if (emit_cross_intents != 0u && target_chunk_index != source_chunk_index) {
            rg_cross_intent_t intent;

            if (task_output == NULL) {
                return 0u;
            }

            intent.source_chunk_index = source_chunk_index;
            intent.target_chunk_index = target_chunk_index;
            intent.source_cell_index = source_index;
            intent.target_cell_index = target_index;
            intent.source_material_id = source_material_id;
            intent.target_material_id = target_material_id;
            if (rg_task_output_push_intent(task_output, &intent) == 0u) {
                return 0u;
            }
            task_output->emitted_move_count += 1u;
            return 1u;
        }

        target_chunk->material_ids[target_index] = source_material_id;
        source_chunk->material_ids[source_index] = target_material_id;
        rg_payload_swap(world, source_chunk, source_index, target_chunk, target_index);
    } else {
        if (emit_cross_intents != 0u && target_chunk_index != source_chunk_index) {
            rg_cross_intent_t intent;

            if (task_output == NULL) {
                return 0u;
            }

            intent.source_chunk_index = source_chunk_index;
            intent.target_chunk_index = target_chunk_index;
            intent.source_cell_index = source_index;
            intent.target_cell_index = target_index;
            intent.source_material_id = source_material_id;
            intent.target_material_id = 0u;
            if (rg_task_output_push_intent(task_output, &intent) == 0u) {
                return 0u;
            }
            task_output->emitted_move_count += 1u;
            return 1u;
        }

        target_chunk->material_ids[target_index] = source_material_id;
        source_chunk->material_ids[source_index] = 0u;
        rg_payload_move(world, source_chunk, source_index, target_chunk, target_index, source_material);

        if (source_chunk != target_chunk) {
            if (source_chunk->live_cells > 0u) {
                source_chunk->live_cells -= 1u;
            }
            target_chunk->live_cells += 1u;

            if (source_chunk->live_cells == 0u) {
                source_chunk->idle_steps = 0u;
                rg_chunk_set_awake(world, source_chunk, 0u);
            }
        }
    }

    source_chunk->idle_steps = 0u;
    target_chunk->idle_steps = 0u;
    rg_set_chunk_awake_for_mode(
        world,
        source_chunk,
        (uint8_t)(source_chunk->live_cells > 0u),
        task_output);
    rg_set_chunk_awake_for_mode(
        world,
        target_chunk,
        (uint8_t)(target_chunk->live_cells > 0u),
        task_output);
    rg_mask_set(target_chunk, target_index);

    if (task_output != NULL) {
        task_output->emitted_move_count += 1u;
    } else {
        world->intents_emitted_last_step += 1u;
    }
    (void)target_local_x;
    (void)target_local_y;
    return 1u;
}

static uint8_t rg_step_powder(
    rg_world_t* world,
    uint32_t source_chunk_index,
    rg_chunk_entry_t* source_entry,
    int32_t source_local_x,
    int32_t source_local_y,
    uint32_t source_index,
    rg_material_id_t source_material_id,
    const rg_material_record_t* source_material,
    uint8_t primary_left,
    uint8_t emit_cross_intents,
    rg_task_output_t* task_output)
{
    int32_t first_dx;
    int32_t second_dx;

    first_dx = (primary_left != 0u) ? -1 : 1;
    second_dx = -first_dx;

    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            0,
            1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            first_dx,
            1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            second_dx,
            1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }

    return 0u;
}

static uint8_t rg_step_liquid(
    rg_world_t* world,
    uint32_t source_chunk_index,
    rg_chunk_entry_t* source_entry,
    int32_t source_local_x,
    int32_t source_local_y,
    uint32_t source_index,
    rg_material_id_t source_material_id,
    const rg_material_record_t* source_material,
    uint8_t primary_left,
    uint8_t emit_cross_intents,
    rg_task_output_t* task_output)
{
    int32_t first_dx;
    int32_t second_dx;

    first_dx = (primary_left != 0u) ? -1 : 1;
    second_dx = -first_dx;

    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            0,
            1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            first_dx,
            0,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            second_dx,
            0,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            first_dx,
            1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            second_dx,
            1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }

    return 0u;
}

static uint8_t rg_step_gas(
    rg_world_t* world,
    uint32_t source_chunk_index,
    rg_chunk_entry_t* source_entry,
    int32_t source_local_x,
    int32_t source_local_y,
    uint32_t source_index,
    rg_material_id_t source_material_id,
    const rg_material_record_t* source_material,
    uint8_t primary_left,
    uint8_t emit_cross_intents,
    rg_task_output_t* task_output)
{
    int32_t first_dx;
    int32_t second_dx;

    first_dx = (primary_left != 0u) ? -1 : 1;
    second_dx = -first_dx;

    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            0,
            -1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            first_dx,
            0,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            second_dx,
            0,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            first_dx,
            -1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }
    if (rg_attempt_move(
            world,
            source_chunk_index,
            source_entry,
            source_local_x,
            source_local_y,
            source_index,
            source_material_id,
            source_material,
            second_dx,
            -1,
            0u,
            emit_cross_intents,
            task_output) != 0u) {
        return 1u;
    }

    return 0u;
}

static uint8_t rg_step_chunk_serial(
    rg_world_t* world,
    uint32_t source_chunk_index,
    uint64_t tick,
    uint8_t emit_cross_intents,
    rg_task_output_t* task_output)
{
    rg_chunk_entry_t* entry;
    rg_chunk_t* chunk;
    int32_t y;
    uint8_t changed;

    if (world == NULL || source_chunk_index >= world->chunk_count) {
        return 0u;
    }

    entry = &world->chunks[source_chunk_index];
    if (entry->chunk == NULL) {
        return 0u;
    }

    chunk = entry->chunk;
    if (chunk->live_cells == 0u) {
        chunk->idle_steps = 0u;
        rg_chunk_set_awake(world, chunk, 0u);
        return 0u;
    }

    changed = 0u;

    for (y = world->chunk_height - 1; y >= 0; --y) {
        int32_t x_step;
        uint8_t left_to_right;

        left_to_right = (uint8_t)(rg_step_random(world, tick, entry->chunk_x, entry->chunk_y, 0, y, 0x71u) & 1u);
        for (x_step = 0; x_step < world->chunk_width; ++x_step) {
            int32_t x;
            uint32_t index;
            rg_material_id_t material_id;
            const rg_material_record_t* material;
            uint8_t primary_left;
            uint8_t moved;

            x = (left_to_right != 0u) ? x_step : (world->chunk_width - 1 - x_step);
            index = ((uint32_t)y * (uint32_t)world->chunk_width) + (uint32_t)x;

            if (rg_mask_test(chunk, index) != 0u) {
                continue;
            }

            material_id = chunk->material_ids[index];
            if (material_id == 0u) {
                continue;
            }

            material = rg_material_get(world, material_id);
            if (material == NULL) {
                continue;
            }
            if ((material->flags & RG_MATERIAL_STATIC) != 0u) {
                continue;
            }

            primary_left = (uint8_t)(rg_step_random(world, tick, entry->chunk_x, entry->chunk_y, x, y, 0xabu) & 1u);
            moved = 0u;
            if (material->update_fn != NULL) {
                rg_update_ctx_t update_ctx;
                void* instance_data;

                memset(&update_ctx, 0, sizeof(update_ctx));
                update_ctx.world = world;
                update_ctx.tick = tick;
                update_ctx.source_chunk_index = source_chunk_index;
                update_ctx.source_entry = entry;
                update_ctx.source_local_x = x;
                update_ctx.source_local_y = y;
                update_ctx.source_cell_index = index;
                update_ctx.source_cell.x = (entry->chunk_x * world->chunk_width) + x;
                update_ctx.source_cell.y = (entry->chunk_y * world->chunk_height) + y;
                update_ctx.emit_cross_intents = emit_cross_intents;
                update_ctx.task_output = task_output;

                instance_data = NULL;
                if (material->instance_size > 0u) {
                    instance_data = rg_chunk_payload_ptr(world, chunk, index);
                }

                material->update_fn(
                    &update_ctx,
                    update_ctx.source_cell,
                    material_id,
                    instance_data,
                    material->user_data);

                moved = update_ctx.changed;
            } else if ((material->flags & RG_MATERIAL_GAS) != 0u) {
                moved = rg_step_gas(
                    world,
                    source_chunk_index,
                    entry,
                    x,
                    y,
                    index,
                    material_id,
                    material,
                    primary_left,
                    emit_cross_intents,
                    task_output);
            } else if ((material->flags & RG_MATERIAL_LIQUID) != 0u) {
                moved = rg_step_liquid(
                    world,
                    source_chunk_index,
                    entry,
                    x,
                    y,
                    index,
                    material_id,
                    material,
                    primary_left,
                    emit_cross_intents,
                    task_output);
            } else if ((material->flags & RG_MATERIAL_POWDER) != 0u) {
                moved = rg_step_powder(
                    world,
                    source_chunk_index,
                    entry,
                    x,
                    y,
                    index,
                    material_id,
                    material,
                    primary_left,
                    emit_cross_intents,
                    task_output);
            }

            if (moved != 0u) {
                changed = 1u;
            }
        }
    }

    if (emit_cross_intents != 0u && task_output != NULL && task_output->intent_count > 0u) {
        changed = 1u;
    }

    if (chunk->live_cells == 0u) {
        chunk->idle_steps = 0u;
        rg_set_chunk_awake_for_mode(world, chunk, 0u, task_output);
    } else if (changed != 0u) {
        chunk->idle_steps = 0u;
        rg_set_chunk_awake_for_mode(world, chunk, 1u, task_output);
    } else {
        if (chunk->idle_steps < UINT32_MAX) {
            chunk->idle_steps += 1u;
        }
        if (chunk->idle_steps >= RG_CHUNK_SLEEP_TICKS) {
            rg_set_chunk_awake_for_mode(world, chunk, 0u, task_output);
        }
    }

    if (task_output != NULL) {
        task_output->changed = changed;
    }
    return changed;
}

static rg_status_t rg_step_full_scan_serial(rg_world_t* world, uint64_t tick)
{
    uint32_t i;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    rg_prepare_step_masks(world);

    for (i = 0u; i < world->chunk_count; ++i) {
        (void)rg_step_chunk_serial(world, i, tick, 0u, NULL);
    }

    return RG_STATUS_OK;
}

static rg_status_t rg_step_chunk_scan_serial(rg_world_t* world, uint64_t tick)
{
    uint32_t i;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    rg_prepare_step_masks(world);

    for (i = 0u; i < world->chunk_count; ++i) {
        rg_chunk_t* chunk;

        chunk = world->chunks[i].chunk;
        if (chunk == NULL || chunk->awake == 0u) {
            continue;
        }
        (void)rg_step_chunk_serial(world, i, tick, 0u, NULL);
    }

    return RG_STATUS_OK;
}

static uint8_t rg_has_parallel_runner(const rg_world_t* world)
{
    if (world == NULL || world->runner == NULL || world->runner->vtable == NULL) {
        return 0u;
    }
    return (uint8_t)(world->runner->vtable->parallel_for != NULL);
}

static void rg_checkerboard_task_callback(uint32_t task_index, uint32_t worker_index, void* user_data)
{
    rg_checkerboard_task_ctx_t* ctx;

    (void)worker_index;

    ctx = (rg_checkerboard_task_ctx_t*)user_data;
    if (ctx == NULL || task_index >= ctx->chunk_count || ctx->outputs == NULL) {
        return;
    }

    (void)rg_step_chunk_serial(
        ctx->world,
        ctx->chunk_indices[task_index],
        ctx->tick,
        1u,
        &ctx->outputs[task_index]);
}

static rg_status_t rg_execute_checkerboard_phase(
    rg_world_t* world,
    uint64_t tick,
    uint32_t color_x,
    uint32_t color_y)
{
    uint32_t* chunk_indices;
    rg_task_output_t* outputs;
    rg_checkerboard_task_ctx_t task_ctx;
    uint32_t task_count;
    uint32_t i;
    rg_status_t status;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    chunk_indices = NULL;
    outputs = NULL;
    task_count = 0u;
    status = RG_STATUS_OK;

    if (world->chunk_count > 0u) {
        chunk_indices = (uint32_t*)malloc((size_t)world->chunk_count * sizeof(*chunk_indices));
        if (chunk_indices == NULL) {
            return RG_STATUS_ALLOCATION_FAILED;
        }
    }

    for (i = 0u; i < world->chunk_count; ++i) {
        rg_chunk_entry_t* entry;
        rg_chunk_t* chunk;
        uint32_t x_parity;
        uint32_t y_parity;

        entry = &world->chunks[i];
        chunk = entry->chunk;
        if (chunk == NULL || chunk->live_cells == 0u || chunk->awake == 0u) {
            continue;
        }

        x_parity = ((uint32_t)entry->chunk_x) & 1u;
        y_parity = ((uint32_t)entry->chunk_y) & 1u;
        if (x_parity != color_x || y_parity != color_y) {
            continue;
        }

        chunk_indices[task_count] = i;
        task_count += 1u;
    }

    if (task_count == 0u) {
        free(chunk_indices);
        return RG_STATUS_OK;
    }

    outputs = (rg_task_output_t*)calloc((size_t)task_count, sizeof(*outputs));
    if (outputs == NULL) {
        free(chunk_indices);
        return RG_STATUS_ALLOCATION_FAILED;
    }

    memset(&task_ctx, 0, sizeof(task_ctx));
    task_ctx.world = world;
    task_ctx.tick = tick;
    task_ctx.chunk_indices = chunk_indices;
    task_ctx.chunk_count = task_count;
    task_ctx.outputs = outputs;

    if (rg_has_parallel_runner(world) != 0u) {
        status = world->runner->vtable->parallel_for(
            world->runner->user,
            task_count,
            rg_checkerboard_task_callback,
            &task_ctx);
    } else {
        for (i = 0u; i < task_count; ++i) {
            rg_checkerboard_task_callback(i, 0u, &task_ctx);
        }
    }

    if (status == RG_STATUS_OK) {
        for (i = 0u; i < task_count; ++i) {
            world->intents_emitted_last_step += outputs[i].emitted_move_count;
        }
        status = rg_merge_cross_intents(world, outputs, task_count);
        rg_recompute_active_chunk_count(world);
    } else {
        rg_recompute_active_chunk_count(world);
    }

    for (i = 0u; i < task_count; ++i) {
        rg_task_output_release(&outputs[i]);
    }
    free(outputs);
    free(chunk_indices);
    return status;
}

static rg_status_t rg_step_checkerboard_parallel(rg_world_t* world, uint64_t tick)
{
    uint32_t color_index;
    rg_status_t status;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    rg_prepare_step_masks(world);

    for (color_index = 0u; color_index < 4u; ++color_index) {
        uint32_t color_x;
        uint32_t color_y;

        color_x = color_index & 1u;
        color_y = (color_index >> 1u) & 1u;

        status = rg_execute_checkerboard_phase(world, tick, color_x, color_y);
        if (status != RG_STATUS_OK) {
            return status;
        }
    }

    return RG_STATUS_OK;
}

rg_status_t rg_world_create(const rg_world_config_t* cfg, rg_world_t** out_world)
{
    rg_world_config_t resolved_cfg;
    rg_allocator_t allocator;
    rg_world_t* world;
    size_t material_capacity;
    rg_status_t status;

    if (out_world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    *out_world = NULL;

    memset(&resolved_cfg, 0, sizeof(resolved_cfg));
    if (cfg != NULL) {
        resolved_cfg = *cfg;
    }

    if (resolved_cfg.chunk_width == 0) {
        resolved_cfg.chunk_width = RG_DEFAULT_CHUNK_WIDTH;
    }
    if (resolved_cfg.chunk_height == 0) {
        resolved_cfg.chunk_height = RG_DEFAULT_CHUNK_HEIGHT;
    }
    if (resolved_cfg.inline_payload_bytes == 0u) {
        resolved_cfg.inline_payload_bytes = RG_DEFAULT_INLINE_PAYLOAD_BYTES;
    }
    if (resolved_cfg.max_materials == 0u) {
        resolved_cfg.max_materials = RG_DEFAULT_MAX_MATERIALS;
    }
    if (resolved_cfg.initial_chunk_capacity == 0u) {
        resolved_cfg.initial_chunk_capacity = RG_DEFAULT_INITIAL_CHUNKS;
    }

    if (resolved_cfg.chunk_width <= 0 || resolved_cfg.chunk_height <= 0) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if ((uint32_t)resolved_cfg.default_step_mode > (uint32_t)RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if ((uint64_t)resolved_cfg.chunk_width * (uint64_t)resolved_cfg.chunk_height > UINT32_MAX) {
        return RG_STATUS_CAPACITY_REACHED;
    }

    status = rg_prepare_allocator(&resolved_cfg.allocator, &allocator);
    if (status != RG_STATUS_OK) {
        return status;
    }

    world = (rg_world_t*)rg_alloc_bytes(&allocator, sizeof(*world), _Alignof(rg_world_t));
    if (world == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }
    memset(world, 0, sizeof(*world));

    world->allocator = allocator;
    world->runner = resolved_cfg.runner;
    world->default_step_mode = resolved_cfg.default_step_mode;
    world->deterministic_seed = resolved_cfg.deterministic_seed;
    world->deterministic_mode = resolved_cfg.deterministic_mode;
    world->chunk_width = resolved_cfg.chunk_width;
    world->chunk_height = resolved_cfg.chunk_height;
    world->cells_per_chunk = (uint32_t)((uint64_t)resolved_cfg.chunk_width * (uint64_t)resolved_cfg.chunk_height);
    world->inline_payload_bytes = resolved_cfg.inline_payload_bytes;
    world->max_materials = resolved_cfg.max_materials;

    if (world->inline_payload_bytes > 0u) {
        world->swap_payload = (uint8_t*)rg_alloc_bytes(
            &world->allocator,
            (size_t)world->inline_payload_bytes,
            1u);
        if (world->swap_payload == NULL) {
            rg_free_bytes(&world->allocator, world, sizeof(*world), _Alignof(rg_world_t));
            return RG_STATUS_ALLOCATION_FAILED;
        }
        memset(world->swap_payload, 0, (size_t)world->inline_payload_bytes);
    }

    material_capacity = ((size_t)world->max_materials + 1u) * sizeof(*world->materials);
    world->materials = (rg_material_record_t*)rg_alloc_bytes(
        &world->allocator,
        material_capacity,
        _Alignof(rg_material_record_t));
    if (world->materials == NULL) {
        rg_free_bytes(&world->allocator, world->swap_payload, (size_t)world->inline_payload_bytes, 1u);
        rg_free_bytes(&world->allocator, world, sizeof(*world), _Alignof(rg_world_t));
        return RG_STATUS_ALLOCATION_FAILED;
    }
    memset(world->materials, 0, material_capacity);

    status = rg_chunk_reserve(world, resolved_cfg.initial_chunk_capacity);
    if (status != RG_STATUS_OK) {
        rg_free_bytes(&world->allocator, world->materials, material_capacity, _Alignof(rg_material_record_t));
        rg_free_bytes(&world->allocator, world->swap_payload, (size_t)world->inline_payload_bytes, 1u);
        rg_free_bytes(&world->allocator, world, sizeof(*world), _Alignof(rg_world_t));
        return status;
    }

    *out_world = world;
    return RG_STATUS_OK;
}

void rg_world_destroy(rg_world_t* world)
{
    size_t material_capacity;
    uint32_t i;

    if (world == NULL) {
        return;
    }

    for (i = 0u; i < world->chunk_count; ++i) {
        rg_chunk_destroy(world, world->chunks[i].chunk);
    }
    rg_free_bytes(
        &world->allocator,
        world->chunks,
        (size_t)world->chunk_capacity * sizeof(*world->chunks),
        _Alignof(rg_chunk_entry_t));

    for (i = 1u; i <= world->material_count; ++i) {
        if (world->materials[i].used != 0u && world->materials[i].name != NULL) {
            size_t name_len;
            name_len = strlen(world->materials[i].name) + 1u;
            rg_free_bytes(&world->allocator, world->materials[i].name, name_len, 1u);
        }
    }

    material_capacity = ((size_t)world->max_materials + 1u) * sizeof(*world->materials);
    rg_free_bytes(&world->allocator, world->materials, material_capacity, _Alignof(rg_material_record_t));
    rg_free_bytes(&world->allocator, world->swap_payload, (size_t)world->inline_payload_bytes, 1u);
    rg_free_bytes(&world->allocator, world, sizeof(*world), _Alignof(rg_world_t));
}

rg_status_t rg_material_register(
    rg_world_t* world,
    const rg_material_desc_t* desc,
    rg_material_id_t* out_material_id)
{
    rg_material_id_t new_id;
    rg_material_record_t* record;
    uint16_t instance_align;
    uint32_t i;

    if (world == NULL || desc == NULL || out_material_id == NULL || desc->name == NULL || desc->name[0] == '\0') {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (world->material_count >= world->max_materials) {
        return RG_STATUS_CAPACITY_REACHED;
    }

    for (i = 1u; i <= world->material_count; ++i) {
        if (world->materials[i].used != 0u && strcmp(world->materials[i].name, desc->name) == 0) {
            return RG_STATUS_ALREADY_EXISTS;
        }
    }

    instance_align = desc->instance_align;
    if (instance_align == 0u) {
        instance_align = 1u;
    }
    if (!rg_is_power_of_two_u32(instance_align)) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (desc->instance_size > world->inline_payload_bytes) {
        return RG_STATUS_UNSUPPORTED;
    }

    new_id = world->material_count + 1u;
    record = &world->materials[new_id];
    memset(record, 0, sizeof(*record));

    record->name = rg_strdup_with_allocator(&world->allocator, desc->name);
    if (record->name == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }

    record->flags = desc->flags;
    record->density = desc->density;
    record->friction = desc->friction;
    record->dispersion = desc->dispersion;
    record->instance_size = desc->instance_size;
    record->instance_align = instance_align;
    record->instance_ctor = desc->instance_ctor;
    record->instance_dtor = desc->instance_dtor;
    record->instance_move = desc->instance_move;
    record->update_fn = desc->update_fn;
    record->user_data = desc->user_data;
    record->used = 1u;

    world->material_count = new_id;
    *out_material_id = new_id;
    return RG_STATUS_OK;
}

rg_status_t rg_chunk_load(rg_world_t* world, int32_t chunk_x, int32_t chunk_y)
{
    rg_chunk_t* chunk;
    rg_status_t status;
    uint32_t insert_index;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (rg_chunk_find_index(world, chunk_x, chunk_y) != UINT32_MAX) {
        return RG_STATUS_ALREADY_EXISTS;
    }

    status = rg_chunk_reserve(world, world->chunk_count + 1u);
    if (status != RG_STATUS_OK) {
        return status;
    }

    status = rg_chunk_create(world, &chunk);
    if (status != RG_STATUS_OK) {
        return status;
    }

    insert_index = rg_chunk_insert_index(world, chunk_x, chunk_y);
    if (insert_index < world->chunk_count) {
        memmove(
            &world->chunks[insert_index + 1u],
            &world->chunks[insert_index],
            (size_t)(world->chunk_count - insert_index) * sizeof(*world->chunks));
    }

    world->chunks[insert_index].chunk_x = chunk_x;
    world->chunks[insert_index].chunk_y = chunk_y;
    world->chunks[insert_index].chunk = chunk;
    world->chunk_count += 1u;
    return RG_STATUS_OK;
}

rg_status_t rg_chunk_unload(rg_world_t* world, int32_t chunk_x, int32_t chunk_y)
{
    uint32_t index;
    rg_chunk_t* chunk;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    index = rg_chunk_find_index(world, chunk_x, chunk_y);
    if (index == UINT32_MAX) {
        return RG_STATUS_NOT_FOUND;
    }

    chunk = world->chunks[index].chunk;
    if (chunk != NULL) {
        if (world->live_cells >= chunk->live_cells) {
            world->live_cells -= chunk->live_cells;
        } else {
            world->live_cells = 0u;
        }
        if (chunk->awake != 0u && world->active_chunk_count > 0u) {
            world->active_chunk_count -= 1u;
        }
        rg_chunk_destroy(world, chunk);
    }

    if (index + 1u < world->chunk_count) {
        memmove(
            &world->chunks[index],
            &world->chunks[index + 1u],
            (size_t)(world->chunk_count - index - 1u) * sizeof(*world->chunks));
    }
    world->chunk_count -= 1u;
    return RG_STATUS_OK;
}

rg_status_t rg_cell_get(
    const rg_world_t* world,
    rg_cell_coord_t cell,
    rg_cell_read_t* out_cell)
{
    rg_status_t status;
    uint32_t chunk_index;
    uint32_t cell_index;
    const rg_chunk_t* chunk;
    rg_material_id_t material_id;
    const rg_material_record_t* material;

    if (world == NULL || out_cell == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    status = rg_locate_cell(world, cell, &chunk_index, &cell_index);
    if (status != RG_STATUS_OK) {
        return status;
    }

    chunk = world->chunks[chunk_index].chunk;
    material_id = chunk->material_ids[cell_index];

    out_cell->material_id = material_id;
    out_cell->instance_data = NULL;
    if (material_id == 0u) {
        return RG_STATUS_OK;
    }

    material = rg_material_get(world, material_id);
    if (material == NULL) {
        return RG_STATUS_NOT_FOUND;
    }
    if (material->instance_size > 0u) {
        out_cell->instance_data = rg_chunk_payload_ptr_const(world, chunk, cell_index);
    }

    return RG_STATUS_OK;
}

rg_status_t rg_cell_set(
    rg_world_t* world,
    rg_cell_coord_t cell,
    const rg_cell_write_t* value)
{
    rg_status_t status;
    uint32_t chunk_index;
    uint32_t cell_index;
    rg_chunk_t* chunk;
    rg_material_id_t old_material_id;
    rg_material_id_t new_material_id;
    const rg_material_record_t* old_material;
    const rg_material_record_t* new_material;

    if (world == NULL || value == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (value->material_id == 0u) {
        return rg_cell_clear(world, cell);
    }

    new_material = rg_material_get(world, value->material_id);
    if (new_material == NULL) {
        return RG_STATUS_NOT_FOUND;
    }

    status = rg_locate_cell(world, cell, &chunk_index, &cell_index);
    if (status != RG_STATUS_OK) {
        return status;
    }

    chunk = world->chunks[chunk_index].chunk;
    old_material_id = chunk->material_ids[cell_index];
    new_material_id = value->material_id;

    old_material = rg_material_get(world, old_material_id);
    if (old_material != NULL) {
        rg_release_cell_instance(world, chunk, cell_index, old_material);
    }

    status = rg_write_cell_instance(world, chunk, cell_index, new_material, value->instance_data);
    if (status != RG_STATUS_OK) {
        return status;
    }

    chunk->material_ids[cell_index] = new_material_id;
    rg_update_live_counts(world, chunk, old_material_id, new_material_id);
    chunk->idle_steps = 0u;
    rg_chunk_set_awake(world, chunk, (uint8_t)(chunk->live_cells > 0u));
    return RG_STATUS_OK;
}

rg_status_t rg_cell_clear(rg_world_t* world, rg_cell_coord_t cell)
{
    rg_status_t status;
    uint32_t chunk_index;
    uint32_t cell_index;
    rg_chunk_t* chunk;
    rg_material_id_t old_material_id;
    const rg_material_record_t* old_material;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    status = rg_locate_cell(world, cell, &chunk_index, &cell_index);
    if (status != RG_STATUS_OK) {
        return status;
    }

    chunk = world->chunks[chunk_index].chunk;
    old_material_id = chunk->material_ids[cell_index];
    if (old_material_id == 0u) {
        return RG_STATUS_OK;
    }

    old_material = rg_material_get(world, old_material_id);
    if (old_material != NULL) {
        rg_release_cell_instance(world, chunk, cell_index, old_material);
    }

    chunk->material_ids[cell_index] = 0u;
    rg_update_live_counts(world, chunk, old_material_id, 0u);
    return RG_STATUS_OK;
}

rg_status_t rg_world_step(rg_world_t* world, const rg_step_options_t* options)
{
    rg_step_mode_t mode;
    uint32_t substeps;
    uint32_t substep_index;
    rg_status_t status;

    if (world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    mode = world->default_step_mode;
    substeps = 1u;
    if (options != NULL) {
        mode = options->mode;
        if (options->substeps > 0u) {
            substeps = options->substeps;
        }
    }

    if ((uint32_t)mode > (uint32_t)RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    world->intents_emitted_last_step = 0u;
    world->intent_conflicts_last_step = 0u;

    for (substep_index = 0u; substep_index < substeps; ++substep_index) {
        uint64_t tick;

        tick = world->step_index + (uint64_t)substep_index + 1u;
        switch (mode) {
        case RG_STEP_MODE_FULL_SCAN_SERIAL:
            status = rg_step_full_scan_serial(world, tick);
            break;
        case RG_STEP_MODE_CHUNK_SCAN_SERIAL:
            status = rg_step_chunk_scan_serial(world, tick);
            break;
        case RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL:
            status = rg_step_checkerboard_parallel(world, tick);
            break;
        default:
            status = RG_STATUS_INVALID_ARGUMENT;
            break;
        }

        if (status != RG_STATUS_OK) {
            return status;
        }
    }

    world->step_index += (uint64_t)substeps;
    return RG_STATUS_OK;
}

rg_status_t rg_world_get_stats(const rg_world_t* world, rg_world_stats_t* out_stats)
{
    if (world == NULL || out_stats == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->loaded_chunks = world->chunk_count;
    out_stats->active_chunks = world->active_chunk_count;
    out_stats->live_cells = world->live_cells;
    out_stats->step_index = world->step_index;
    out_stats->intents_emitted_last_step = world->intents_emitted_last_step;
    out_stats->intent_conflicts_last_step = world->intent_conflicts_last_step;
    out_stats->payload_overflow_allocs = world->payload_overflow_allocs;
    out_stats->payload_overflow_frees = world->payload_overflow_frees;
    return RG_STATUS_OK;
}

static uint8_t rg_cell_coord_equal(rg_cell_coord_t lhs, rg_cell_coord_t rhs)
{
    return (uint8_t)(lhs.x == rhs.x && lhs.y == rhs.y);
}

static rg_status_t rg_ctx_validate(rg_update_ctx_t* ctx)
{
    if (ctx == NULL || ctx->world == NULL || ctx->source_entry == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (ctx->source_chunk_index >= ctx->world->chunk_count) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (ctx->operation_done != 0u) {
        return RG_STATUS_CONFLICT;
    }
    return RG_STATUS_OK;
}

static rg_status_t rg_ctx_transform_current_cell(
    rg_update_ctx_t* ctx,
    rg_material_id_t new_material,
    const void* new_instance_data)
{
    rg_world_t* world;
    rg_chunk_entry_t* source_entry;
    rg_chunk_t* source_chunk;
    rg_material_id_t old_material_id;
    const rg_material_record_t* old_material;
    const rg_material_record_t* new_material_record;
    rg_status_t status;

    if (ctx == NULL || ctx->world == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    world = ctx->world;
    source_entry = &world->chunks[ctx->source_chunk_index];
    source_chunk = source_entry->chunk;
    if (source_chunk == NULL || ctx->source_cell_index >= world->cells_per_chunk) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    old_material_id = source_chunk->material_ids[ctx->source_cell_index];
    if (old_material_id == 0u) {
        return RG_STATUS_NOT_FOUND;
    }

    old_material = rg_material_get(world, old_material_id);
    if (old_material == NULL) {
        return RG_STATUS_NOT_FOUND;
    }

    if (new_material == 0u) {
        rg_release_cell_instance(world, source_chunk, ctx->source_cell_index, old_material);
        source_chunk->material_ids[ctx->source_cell_index] = 0u;
        rg_update_live_counts(world, source_chunk, old_material_id, 0u);
        source_chunk->idle_steps = 0u;
        rg_set_chunk_awake_for_mode(
            world,
            source_chunk,
            (uint8_t)(source_chunk->live_cells > 0u),
            ctx->task_output);
        rg_mask_set(source_chunk, ctx->source_cell_index);
        return RG_STATUS_OK;
    }

    new_material_record = rg_material_get(world, new_material);
    if (new_material_record == NULL) {
        return RG_STATUS_NOT_FOUND;
    }

    if (new_material != old_material_id) {
        rg_release_cell_instance(world, source_chunk, ctx->source_cell_index, old_material);
    } else if (new_instance_data == NULL) {
        rg_mask_set(source_chunk, ctx->source_cell_index);
        return RG_STATUS_OK;
    }

    status = rg_write_cell_instance(
        world,
        source_chunk,
        ctx->source_cell_index,
        new_material_record,
        new_instance_data);
    if (status != RG_STATUS_OK) {
        return status;
    }

    source_chunk->material_ids[ctx->source_cell_index] = new_material;
    rg_update_live_counts(world, source_chunk, old_material_id, new_material);
    source_chunk->idle_steps = 0u;
    rg_set_chunk_awake_for_mode(
        world,
        source_chunk,
        (uint8_t)(source_chunk->live_cells > 0u),
        ctx->task_output);
    rg_mask_set(source_chunk, ctx->source_cell_index);
    return RG_STATUS_OK;
}

rg_status_t rg_ctx_try_move(rg_update_ctx_t* ctx, rg_cell_coord_t from, rg_cell_coord_t to)
{
    rg_status_t status;
    rg_world_t* world;
    rg_chunk_entry_t* source_entry;
    rg_chunk_t* source_chunk;
    rg_material_id_t source_material_id;
    const rg_material_record_t* source_material;
    int32_t dx;
    int32_t dy;
    uint32_t target_chunk_index;
    rg_chunk_entry_t* target_entry;
    uint32_t target_index;
    int32_t target_local_x;
    int32_t target_local_y;

    status = rg_ctx_validate(ctx);
    if (status != RG_STATUS_OK) {
        return status;
    }
    if (rg_cell_coord_equal(from, ctx->source_cell) == 0u) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    dx = to.x - from.x;
    dy = to.y - from.y;
    if ((dx == 0 && dy == 0) || rg_abs_i32(dx) > 1 || rg_abs_i32(dy) > 1) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    world = ctx->world;
    source_entry = &world->chunks[ctx->source_chunk_index];
    source_chunk = source_entry->chunk;
    if (source_chunk == NULL || ctx->source_cell_index >= world->cells_per_chunk) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    source_material_id = source_chunk->material_ids[ctx->source_cell_index];
    if (source_material_id == 0u) {
        return RG_STATUS_NOT_FOUND;
    }
    source_material = rg_material_get(world, source_material_id);
    if (source_material == NULL) {
        return RG_STATUS_NOT_FOUND;
    }

    status = rg_resolve_target(
        world,
        source_entry,
        ctx->source_local_x,
        ctx->source_local_y,
        dx,
        dy,
        &target_entry,
        &target_chunk_index,
        &target_index,
        &target_local_x,
        &target_local_y);
    if (status != RG_STATUS_OK) {
        return status;
    }
    (void)target_entry;
    (void)target_chunk_index;
    (void)target_index;
    (void)target_local_x;
    (void)target_local_y;

    if (rg_attempt_move(
            world,
            ctx->source_chunk_index,
            source_entry,
            ctx->source_local_x,
            ctx->source_local_y,
            ctx->source_cell_index,
            source_material_id,
            source_material,
            dx,
            dy,
            1u,
            ctx->emit_cross_intents,
            ctx->task_output) == 0u) {
        return RG_STATUS_CONFLICT;
    }

    ctx->operation_done = 1u;
    ctx->changed = 1u;
    return RG_STATUS_OK;
}

rg_status_t rg_ctx_try_swap(rg_update_ctx_t* ctx, rg_cell_coord_t a, rg_cell_coord_t b)
{
    rg_status_t status;
    rg_world_t* world;
    rg_chunk_entry_t* source_entry;
    rg_chunk_t* source_chunk;
    rg_material_id_t source_material_id;
    rg_chunk_entry_t* target_entry;
    uint32_t target_chunk_index;
    uint32_t target_index;
    int32_t target_local_x;
    int32_t target_local_y;
    int32_t dx;
    int32_t dy;
    rg_chunk_t* target_chunk;
    rg_material_id_t target_material_id;
    const rg_material_record_t* target_material;

    status = rg_ctx_validate(ctx);
    if (status != RG_STATUS_OK) {
        return status;
    }
    if (rg_cell_coord_equal(a, ctx->source_cell) == 0u) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    dx = b.x - a.x;
    dy = b.y - a.y;
    if ((dx == 0 && dy == 0) || rg_abs_i32(dx) > 1 || rg_abs_i32(dy) > 1) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    world = ctx->world;
    source_entry = &world->chunks[ctx->source_chunk_index];
    source_chunk = source_entry->chunk;
    if (source_chunk == NULL || ctx->source_cell_index >= world->cells_per_chunk) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    source_material_id = source_chunk->material_ids[ctx->source_cell_index];
    if (source_material_id == 0u) {
        return RG_STATUS_NOT_FOUND;
    }

    status = rg_resolve_target(
        world,
        source_entry,
        ctx->source_local_x,
        ctx->source_local_y,
        dx,
        dy,
        &target_entry,
        &target_chunk_index,
        &target_index,
        &target_local_x,
        &target_local_y);
    if (status != RG_STATUS_OK) {
        return status;
    }
    (void)target_local_x;
    (void)target_local_y;

    target_chunk = target_entry->chunk;
    if (target_chunk == NULL) {
        return RG_STATUS_NOT_FOUND;
    }
    target_material_id = target_chunk->material_ids[target_index];
    if (target_material_id == 0u) {
        return RG_STATUS_CONFLICT;
    }

    target_material = rg_material_get(world, target_material_id);
    if (target_material == NULL) {
        return RG_STATUS_NOT_FOUND;
    }
    if ((target_material->flags & RG_MATERIAL_STATIC) != 0u) {
        return RG_STATUS_CONFLICT;
    }

    if (ctx->emit_cross_intents != 0u && target_chunk_index != ctx->source_chunk_index) {
        rg_cross_intent_t intent;

        if (ctx->task_output == NULL) {
            return RG_STATUS_INVALID_ARGUMENT;
        }

        intent.source_chunk_index = ctx->source_chunk_index;
        intent.target_chunk_index = target_chunk_index;
        intent.source_cell_index = ctx->source_cell_index;
        intent.target_cell_index = target_index;
        intent.source_material_id = source_material_id;
        intent.target_material_id = target_material_id;
        if (rg_task_output_push_intent(ctx->task_output, &intent) == 0u) {
            return RG_STATUS_ALLOCATION_FAILED;
        }
        ctx->task_output->emitted_move_count += 1u;
    } else {
        target_chunk->material_ids[target_index] = source_material_id;
        source_chunk->material_ids[ctx->source_cell_index] = target_material_id;
        rg_payload_swap(world, source_chunk, ctx->source_cell_index, target_chunk, target_index);

        source_chunk->idle_steps = 0u;
        target_chunk->idle_steps = 0u;
        rg_set_chunk_awake_for_mode(
            world,
            source_chunk,
            (uint8_t)(source_chunk->live_cells > 0u),
            ctx->task_output);
        rg_set_chunk_awake_for_mode(
            world,
            target_chunk,
            (uint8_t)(target_chunk->live_cells > 0u),
            ctx->task_output);
        rg_mask_set(target_chunk, target_index);

        if (ctx->task_output != NULL) {
            ctx->task_output->emitted_move_count += 1u;
        } else {
            world->intents_emitted_last_step += 1u;
        }
    }

    ctx->operation_done = 1u;
    ctx->changed = 1u;
    return RG_STATUS_OK;
}

rg_status_t rg_ctx_transform(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t new_material,
    const void* new_instance_data)
{
    rg_status_t status;

    status = rg_ctx_validate(ctx);
    if (status != RG_STATUS_OK) {
        return status;
    }
    if (rg_cell_coord_equal(cell, ctx->source_cell) == 0u) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    status = rg_ctx_transform_current_cell(ctx, new_material, new_instance_data);
    if (status != RG_STATUS_OK) {
        return status;
    }

    ctx->operation_done = 1u;
    ctx->changed = 1u;
    return RG_STATUS_OK;
}

uint32_t rg_ctx_random_u32(rg_update_ctx_t* ctx)
{
    uint32_t salt;

    if (ctx == NULL || ctx->world == NULL || ctx->source_entry == NULL) {
        return 0u;
    }

    salt = 0xC001u + ctx->random_counter;
    ctx->random_counter += 1u;
    return rg_step_random(
        ctx->world,
        ctx->tick,
        ctx->source_entry->chunk_x,
        ctx->source_entry->chunk_y,
        ctx->source_local_x,
        ctx->source_local_y,
        salt);
}
