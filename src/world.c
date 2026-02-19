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
    RG_DEFAULT_INITIAL_CHUNKS = 16
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
    uint32_t live_cells;
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

static int rg_is_power_of_two_u32(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
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
    if (world == NULL || chunk == NULL || old_material == new_material) {
        return;
    }

    if (old_material == 0u && new_material != 0u) {
        world->live_cells += 1u;
        chunk->live_cells += 1u;
        if (chunk->live_cells == 1u) {
            world->active_chunk_count += 1u;
        }
        return;
    }

    if (old_material != 0u && new_material == 0u) {
        if (world->live_cells > 0u) {
            world->live_cells -= 1u;
        }
        if (chunk->live_cells > 0u) {
            chunk->live_cells -= 1u;
            if (chunk->live_cells == 0u && world->active_chunk_count > 0u) {
                world->active_chunk_count -= 1u;
            }
        }
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

    *out_chunk = chunk;
    return RG_STATUS_OK;
}

static void rg_chunk_destroy(rg_world_t* world, rg_chunk_t* chunk)
{
    size_t material_bytes;
    size_t payload_bytes;
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

    rg_free_bytes(&world->allocator, chunk->inline_payload, payload_bytes, 1u);
    rg_free_bytes(&world->allocator, chunk->material_ids, material_bytes, _Alignof(uint16_t));
    rg_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(rg_chunk_t));
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

    material_capacity = ((size_t)world->max_materials + 1u) * sizeof(*world->materials);
    world->materials = (rg_material_record_t*)rg_alloc_bytes(
        &world->allocator,
        material_capacity,
        _Alignof(rg_material_record_t));
    if (world->materials == NULL) {
        rg_free_bytes(&world->allocator, world, sizeof(*world), _Alignof(rg_world_t));
        return RG_STATUS_ALLOCATION_FAILED;
    }
    memset(world->materials, 0, material_capacity);

    status = rg_chunk_reserve(world, resolved_cfg.initial_chunk_capacity);
    if (status != RG_STATUS_OK) {
        rg_free_bytes(&world->allocator, world->materials, material_capacity, _Alignof(rg_material_record_t));
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

    world->chunks[world->chunk_count].chunk_x = chunk_x;
    world->chunks[world->chunk_count].chunk_y = chunk_y;
    world->chunks[world->chunk_count].chunk = chunk;
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
        if (chunk->live_cells > 0u) {
            if (world->live_cells >= chunk->live_cells) {
                world->live_cells -= chunk->live_cells;
            } else {
                world->live_cells = 0u;
            }
            if (world->active_chunk_count > 0u) {
                world->active_chunk_count -= 1u;
            }
        }
        rg_chunk_destroy(world, chunk);
    }

    if (index + 1u < world->chunk_count) {
        world->chunks[index] = world->chunks[world->chunk_count - 1u];
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

    world->step_index += (uint64_t)substeps;
    world->intents_emitted_last_step = 0u;
    world->intent_conflicts_last_step = 0u;

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

rg_status_t rg_ctx_try_move(rg_update_ctx_t* ctx, rg_cell_coord_t from, rg_cell_coord_t to)
{
    (void)ctx;
    (void)from;
    (void)to;
    return RG_STATUS_UNSUPPORTED;
}

rg_status_t rg_ctx_try_swap(rg_update_ctx_t* ctx, rg_cell_coord_t a, rg_cell_coord_t b)
{
    (void)ctx;
    (void)a;
    (void)b;
    return RG_STATUS_UNSUPPORTED;
}

rg_status_t rg_ctx_transform(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t new_material,
    const void* new_instance_data)
{
    (void)ctx;
    (void)cell;
    (void)new_material;
    (void)new_instance_data;
    return RG_STATUS_UNSUPPORTED;
}

uint32_t rg_ctx_random_u32(rg_update_ctx_t* ctx)
{
    (void)ctx;
    return 0u;
}
