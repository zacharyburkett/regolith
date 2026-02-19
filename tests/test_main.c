#include "regolith/regolith.h"

#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(condition)                                                      \
    do {                                                                            \
        if (!(condition)) {                                                         \
            fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                    \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define ASSERT_STATUS(actual, expected)                                             \
    do {                                                                            \
        rg_status_t status_result = (actual);                                       \
        if (status_result != (expected)) {                                          \
            fprintf(stderr, "Unexpected status at %s:%d: got %s expected %s\n",    \
                    __FILE__, __LINE__, rg_status_string(status_result),            \
                    rg_status_string((expected)));                                  \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define RUN_TEST(fn)                                                                \
    do {                                                                            \
        int fn_result = (fn)();                                                     \
        if (fn_result != 0) {                                                       \
            fprintf(stderr, "Test failed: %s\n", #fn);                              \
            return fn_result;                                                       \
        }                                                                           \
    } while (0)

typedef struct test_cell_data_s {
    uint32_t id;
    int32_t temperature;
} test_cell_data_t;

typedef struct test_material_user_s {
    int ctor_count;
    int dtor_count;
    test_cell_data_t ctor_value;
} test_material_user_t;

typedef struct test_runner_state_s {
    uint32_t call_count;
    uint32_t total_task_count;
    uint32_t max_task_count;
} test_runner_state_t;

static rg_status_t test_runner_parallel_for(
    void* runner_user,
    uint32_t task_count,
    rg_parallel_task_fn task,
    void* task_user_data)
{
    test_runner_state_t* state;
    uint32_t i;

    if (task == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    state = (test_runner_state_t*)runner_user;
    if (state != NULL) {
        state->call_count += 1u;
        state->total_task_count += task_count;
        if (task_count > state->max_task_count) {
            state->max_task_count = task_count;
        }
    }

    /* Intentionally reverse order to validate deterministic conflict handling. */
    for (i = task_count; i > 0u; --i) {
        uint32_t task_index;
        uint32_t worker_index;

        task_index = i - 1u;
        worker_index = task_index % 4u;
        task(task_index, worker_index, task_user_data);
    }

    return RG_STATUS_OK;
}

static uint32_t test_runner_worker_count(void* runner_user)
{
    (void)runner_user;
    return 4u;
}

static const rg_runner_vtable_t g_test_runner_vtable = {
    test_runner_parallel_for,
    test_runner_worker_count
};

typedef struct test_custom_move_user_s {
    uint32_t call_count;
} test_custom_move_user_t;

typedef struct test_custom_swap_user_s {
    uint32_t call_count;
} test_custom_swap_user_t;

typedef struct test_custom_transform_user_s {
    uint32_t call_count;
    rg_material_id_t target_material_id;
} test_custom_transform_user_t;

typedef struct test_custom_random_user_s {
    uint32_t call_count;
} test_custom_random_user_t;

static void test_custom_move_update(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t material_id,
    void* instance_data,
    void* user_data)
{
    test_custom_move_user_t* user;
    test_cell_data_t* payload;

    (void)material_id;

    user = (test_custom_move_user_t*)user_data;
    if (user != NULL) {
        user->call_count += 1u;
    }

    payload = (test_cell_data_t*)instance_data;
    if (payload != NULL) {
        payload->temperature += 10;
    }

    (void)rg_ctx_try_move(ctx, cell, (rg_cell_coord_t){cell.x, cell.y + 1});
}

static void test_custom_swap_update(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t material_id,
    void* instance_data,
    void* user_data)
{
    test_custom_swap_user_t* user;

    (void)material_id;
    (void)instance_data;

    user = (test_custom_swap_user_t*)user_data;
    if (user != NULL) {
        user->call_count += 1u;
    }

    (void)rg_ctx_try_swap(ctx, cell, (rg_cell_coord_t){cell.x + 1, cell.y});
}

static void test_custom_transform_update(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t material_id,
    void* instance_data,
    void* user_data)
{
    test_custom_transform_user_t* user;

    (void)material_id;
    (void)instance_data;

    user = (test_custom_transform_user_t*)user_data;
    if (user != NULL) {
        user->call_count += 1u;
        (void)rg_ctx_transform(ctx, cell, user->target_material_id, NULL);
    }
}

static void test_custom_random_update(
    rg_update_ctx_t* ctx,
    rg_cell_coord_t cell,
    rg_material_id_t material_id,
    void* instance_data,
    void* user_data)
{
    test_custom_random_user_t* user;
    test_cell_data_t* payload;

    (void)cell;
    (void)material_id;

    user = (test_custom_random_user_t*)user_data;
    if (user != NULL) {
        user->call_count += 1u;
    }

    payload = (test_cell_data_t*)instance_data;
    if (payload != NULL) {
        payload->id = rg_ctx_random_u32(ctx);
    }
}

static void test_ctor(void* dst, void* user_data)
{
    test_material_user_t* user;
    test_cell_data_t* payload;

    if (dst == NULL || user_data == NULL) {
        return;
    }

    user = (test_material_user_t*)user_data;
    payload = (test_cell_data_t*)dst;
    *payload = user->ctor_value;
    user->ctor_count += 1;
}

static void test_dtor(void* dst, void* user_data)
{
    test_material_user_t* user;

    (void)dst;

    if (user_data == NULL) {
        return;
    }

    user = (test_material_user_t*)user_data;
    user->dtor_count += 1;
}

static int register_material(
    rg_world_t* world,
    const char* name,
    void* user_data,
    rg_material_id_t* out_material_id)
{
    rg_material_desc_t desc;

    memset(&desc, 0, sizeof(desc));
    desc.name = name;
    desc.flags = RG_MATERIAL_POWDER;
    desc.instance_size = (uint16_t)sizeof(test_cell_data_t);
    desc.instance_align = (uint16_t)_Alignof(test_cell_data_t);
    desc.instance_ctor = test_ctor;
    desc.instance_dtor = test_dtor;
    desc.user_data = user_data;
    ASSERT_STATUS(rg_material_register(world, &desc, out_material_id), RG_STATUS_OK);
    return 0;
}

static int register_simple_material(
    rg_world_t* world,
    const char* name,
    uint32_t flags,
    float density,
    rg_material_id_t* out_material_id)
{
    rg_material_desc_t desc;

    memset(&desc, 0, sizeof(desc));
    desc.name = name;
    desc.flags = flags;
    desc.density = density;
    ASSERT_STATUS(rg_material_register(world, &desc, out_material_id), RG_STATUS_OK);
    return 0;
}

static int test_world_create_defaults(void)
{
    rg_world_t* world;
    rg_world_stats_t stats;

    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);
    ASSERT_TRUE(world != NULL);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.loaded_chunks == 0u);
    ASSERT_TRUE(stats.active_chunks == 0u);
    ASSERT_TRUE(stats.live_cells == 0u);

    rg_world_destroy(world);
    return 0;
}

static int test_world_create_invalid_config(void)
{
    rg_world_config_t cfg;
    rg_world_t* world;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = -1;
    cfg.chunk_height = 32;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_INVALID_ARGUMENT);
    return 0;
}

static int test_material_register_and_duplicate_rejection(void)
{
    rg_world_t* world;
    rg_material_desc_t desc;
    rg_material_id_t material_id;

    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "sand";
    desc.flags = RG_MATERIAL_POWDER;
    desc.instance_size = (uint16_t)sizeof(test_cell_data_t);
    desc.instance_align = (uint16_t)_Alignof(test_cell_data_t);
    ASSERT_STATUS(rg_material_register(world, &desc, &material_id), RG_STATUS_OK);
    ASSERT_TRUE(material_id == 1u);

    ASSERT_STATUS(rg_material_register(world, &desc, &material_id), RG_STATUS_ALREADY_EXISTS);

    rg_world_destroy(world);
    return 0;
}

static int test_chunk_load_unload(void)
{
    rg_world_t* world;
    rg_world_stats_t stats;

    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);

    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_ALREADY_EXISTS);
    ASSERT_STATUS(rg_chunk_unload(world, 1, 0), RG_STATUS_NOT_FOUND);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.loaded_chunks == 1u);

    ASSERT_STATUS(rg_chunk_unload(world, 0, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.loaded_chunks == 0u);

    rg_world_destroy(world);
    return 0;
}

static int test_cell_set_get_clear_and_nonfungible_payload(void)
{
    rg_world_t* world;
    rg_material_id_t sand_id;
    test_material_user_t user_data;
    rg_cell_write_t write;
    rg_cell_read_t read;
    test_cell_data_t data_a;
    test_cell_data_t data_b;
    const test_cell_data_t* out_a;
    const test_cell_data_t* out_b;
    rg_world_stats_t stats;

    memset(&user_data, 0, sizeof(user_data));
    user_data.ctor_value.id = 42u;
    user_data.ctor_value.temperature = 11;

    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);
    ASSERT_TRUE(register_material(world, "sand", &user_data, &sand_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    data_a.id = 100u;
    data_a.temperature = 900;
    memset(&write, 0, sizeof(write));
    write.material_id = sand_id;
    write.instance_data = &data_a;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 1}, &write), RG_STATUS_OK);

    data_b.id = 101u;
    data_b.temperature = 500;
    write.instance_data = &data_b;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){2, 1}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == sand_id);
    ASSERT_TRUE(read.instance_data != NULL);
    out_a = (const test_cell_data_t*)read.instance_data;
    ASSERT_TRUE(out_a->id == 100u);
    ASSERT_TRUE(out_a->temperature == 900);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){2, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == sand_id);
    ASSERT_TRUE(read.instance_data != NULL);
    out_b = (const test_cell_data_t*)read.instance_data;
    ASSERT_TRUE(out_b->id == 101u);
    ASSERT_TRUE(out_b->temperature == 500);

    ASSERT_STATUS(rg_cell_clear(world, (rg_cell_coord_t){1, 1}), RG_STATUS_OK);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_TRUE(read.instance_data == NULL);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.live_cells == 1u);
    ASSERT_TRUE(stats.active_chunks == 1u);

    rg_world_destroy(world);
    return 0;
}

static int test_ctor_dtor_behavior(void)
{
    rg_world_t* world;
    rg_material_id_t sand_id;
    test_material_user_t user_data;
    rg_cell_write_t write;
    rg_cell_read_t read;
    const test_cell_data_t* payload;

    memset(&user_data, 0, sizeof(user_data));
    user_data.ctor_value.id = 7u;
    user_data.ctor_value.temperature = 123;

    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);
    ASSERT_TRUE(register_material(world, "sand", &user_data, &sand_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = sand_id;
    write.instance_data = NULL;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 1}, &write), RG_STATUS_OK);
    ASSERT_TRUE(user_data.ctor_count == 1);
    ASSERT_TRUE(user_data.dtor_count == 0);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.instance_data != NULL);
    payload = (const test_cell_data_t*)read.instance_data;
    ASSERT_TRUE(payload->id == 7u);
    ASSERT_TRUE(payload->temperature == 123);

    ASSERT_STATUS(rg_cell_clear(world, (rg_cell_coord_t){1, 1}), RG_STATUS_OK);
    ASSERT_TRUE(user_data.dtor_count == 1);

    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){3, 3}, &write), RG_STATUS_OK);
    ASSERT_TRUE(user_data.ctor_count == 2);
    ASSERT_STATUS(rg_chunk_unload(world, 0, 0), RG_STATUS_OK);
    ASSERT_TRUE(user_data.dtor_count == 2);

    rg_world_destroy(world);
    return 0;
}

static int test_step_and_stats(void)
{
    rg_world_t* world;
    rg_step_options_t step_options;
    rg_world_stats_t stats;

    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);

    memset(&step_options, 0, sizeof(step_options));
    step_options.mode = RG_STEP_MODE_CHUNK_SCAN_SERIAL;
    step_options.substeps = 4u;
    ASSERT_STATUS(rg_world_step(world, &step_options), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.step_index == 4u);
    ASSERT_TRUE(stats.intents_emitted_last_step == 0u);
    ASSERT_TRUE(stats.intent_conflicts_last_step == 0u);

    rg_world_destroy(world);
    return 0;
}

static int test_powder_falls_in_full_scan(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_id_t sand_id;
    rg_cell_write_t write;
    rg_cell_read_t read;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 8;
    cfg.chunk_height = 8;
    cfg.default_step_mode = RG_STEP_MODE_FULL_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 123u;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "sand", RG_MATERIAL_POWDER, 10.0f, &sand_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = sand_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){3, 1}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_step(world, NULL), RG_STATUS_OK);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){3, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){3, 2}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == sand_id);

    rg_world_destroy(world);
    return 0;
}

static int test_liquid_flows_sideways_when_blocked(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_id_t water_id;
    rg_material_id_t stone_id;
    rg_cell_write_t write;
    rg_cell_read_t read;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 8;
    cfg.chunk_height = 8;
    cfg.default_step_mode = RG_STEP_MODE_FULL_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 99u;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "water", RG_MATERIAL_LIQUID, 5.0f, &water_id) == 0);
    ASSERT_TRUE(register_simple_material(world, "stone", RG_MATERIAL_STATIC, 100.0f, &stone_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = stone_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){4, 5}, &write), RG_STATUS_OK);
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){3, 4}, &write), RG_STATUS_OK);

    write.material_id = water_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){4, 4}, &write), RG_STATUS_OK);
    ASSERT_STATUS(rg_world_step(world, NULL), RG_STATUS_OK);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){4, 4}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){5, 4}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == water_id);

    rg_world_destroy(world);
    return 0;
}

static int test_cross_chunk_fall(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_id_t sand_id;
    rg_cell_write_t write;
    rg_cell_read_t read;
    rg_world_stats_t stats;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 4;
    cfg.chunk_height = 4;
    cfg.default_step_mode = RG_STEP_MODE_FULL_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 5u;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "sand", RG_MATERIAL_POWDER, 10.0f, &sand_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 0, 1), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = sand_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 3}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_step(world, NULL), RG_STATUS_OK);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 3}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 4}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == sand_id);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.live_cells == 1u);
    ASSERT_TRUE(stats.active_chunks == 1u);

    rg_world_destroy(world);
    return 0;
}

static int test_chunk_scan_sleep_and_wake(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_id_t stone_id;
    rg_cell_write_t write;
    rg_step_options_t step_options;
    rg_world_stats_t stats;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 8;
    cfg.chunk_height = 8;
    cfg.default_step_mode = RG_STEP_MODE_CHUNK_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 42u;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "stone", RG_MATERIAL_STATIC, 100.0f, &stone_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = stone_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){2, 2}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.active_chunks == 1u);

    memset(&step_options, 0, sizeof(step_options));
    step_options.mode = RG_STEP_MODE_CHUNK_SCAN_SERIAL;
    step_options.substeps = 16u;
    ASSERT_STATUS(rg_world_step(world, &step_options), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.active_chunks == 0u);

    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){3, 2}, &write), RG_STATUS_OK);
    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.active_chunks == 1u);

    rg_world_destroy(world);
    return 0;
}

static int test_unloaded_chunk_cell_access(void)
{
    rg_world_t* world;
    rg_material_id_t sand_id;
    test_material_user_t user_data;
    rg_cell_read_t read;
    rg_cell_write_t write;

    memset(&user_data, 0, sizeof(user_data));
    ASSERT_STATUS(rg_world_create(NULL, &world), RG_STATUS_OK);
    ASSERT_TRUE(register_material(world, "sand", &user_data, &sand_id) == 0);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){0, 0}, &read), RG_STATUS_NOT_FOUND);

    memset(&write, 0, sizeof(write));
    write.material_id = sand_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){0, 0}, &write), RG_STATUS_NOT_FOUND);
    ASSERT_STATUS(rg_cell_clear(world, (rg_cell_coord_t){0, 0}), RG_STATUS_NOT_FOUND);

    rg_world_destroy(world);
    return 0;
}

static int test_checkerboard_parallel_cross_chunk_with_runner(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_id_t sand_id;
    rg_cell_write_t write;
    rg_cell_read_t read;
    rg_step_options_t step_options;
    rg_world_stats_t stats;
    test_runner_state_t runner_state;
    rg_runner_t runner;

    memset(&runner_state, 0, sizeof(runner_state));
    runner.vtable = &g_test_runner_vtable;
    runner.user = &runner_state;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 4;
    cfg.chunk_height = 4;
    cfg.default_step_mode = RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 7u;
    cfg.runner = &runner;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "sand", RG_MATERIAL_POWDER, 10.0f, &sand_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 0, 1), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = sand_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 3}, &write), RG_STATUS_OK);

    memset(&step_options, 0, sizeof(step_options));
    step_options.mode = RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL;
    step_options.substeps = 1u;
    ASSERT_STATUS(rg_world_step(world, &step_options), RG_STATUS_OK);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 3}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 4}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == sand_id);

    ASSERT_TRUE(runner_state.call_count > 0u);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.live_cells == 1u);
    ASSERT_TRUE(stats.active_chunks == 1u);

    rg_world_destroy(world);
    return 0;
}

static int test_checkerboard_parallel_conflict_resolution(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_id_t water_id;
    rg_cell_write_t write;
    rg_cell_read_t read;
    rg_step_options_t step_options;
    rg_world_stats_t stats;
    test_runner_state_t runner_state;
    rg_runner_t runner;

    memset(&runner_state, 0, sizeof(runner_state));
    runner.vtable = &g_test_runner_vtable;
    runner.user = &runner_state;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 1;
    cfg.chunk_height = 1;
    cfg.default_step_mode = RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 100u;
    cfg.runner = &runner;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "water", RG_MATERIAL_LIQUID, 5.0f, &water_id) == 0);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 1, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 2, 0), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = water_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){0, 0}, &write), RG_STATUS_OK);
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){2, 0}, &write), RG_STATUS_OK);

    memset(&step_options, 0, sizeof(step_options));
    step_options.mode = RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL;
    step_options.substeps = 1u;
    ASSERT_STATUS(rg_world_step(world, &step_options), RG_STATUS_OK);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){0, 0}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 0}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == water_id);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){2, 0}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == water_id);

    ASSERT_STATUS(rg_world_get_stats(world, &stats), RG_STATUS_OK);
    ASSERT_TRUE(stats.intent_conflicts_last_step >= 1u);
    ASSERT_TRUE(runner_state.call_count > 0u);

    rg_world_destroy(world);
    return 0;
}

static int test_custom_update_try_move_with_payload(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_desc_t desc;
    rg_material_id_t custom_id;
    rg_cell_write_t write;
    rg_cell_read_t read;
    test_custom_move_user_t move_user;
    test_cell_data_t payload;
    const test_cell_data_t* out_payload;

    memset(&move_user, 0, sizeof(move_user));
    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 4;
    cfg.chunk_height = 4;
    cfg.default_step_mode = RG_STEP_MODE_FULL_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 77u;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "custom_move";
    desc.flags = RG_MATERIAL_CUSTOM_UPDATE;
    desc.instance_size = (uint16_t)sizeof(test_cell_data_t);
    desc.instance_align = (uint16_t)_Alignof(test_cell_data_t);
    desc.update_fn = test_custom_move_update;
    desc.user_data = &move_user;
    ASSERT_STATUS(rg_material_register(world, &desc, &custom_id), RG_STATUS_OK);

    payload.id = 7u;
    payload.temperature = 1;
    memset(&write, 0, sizeof(write));
    write.material_id = custom_id;
    write.instance_data = &payload;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 1}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_step(world, NULL), RG_STATUS_OK);
    ASSERT_TRUE(move_user.call_count == 1u);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 2}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == custom_id);
    ASSERT_TRUE(read.instance_data != NULL);
    out_payload = (const test_cell_data_t*)read.instance_data;
    ASSERT_TRUE(out_payload->id == 7u);
    ASSERT_TRUE(out_payload->temperature == 11);

    rg_world_destroy(world);
    return 0;
}

static int test_custom_update_try_swap(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_desc_t desc;
    rg_material_id_t swapper_id;
    rg_material_id_t neighbor_id;
    rg_cell_write_t write;
    rg_cell_read_t read;
    test_custom_swap_user_t swap_user;

    memset(&swap_user, 0, sizeof(swap_user));
    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 6;
    cfg.chunk_height = 6;
    cfg.default_step_mode = RG_STEP_MODE_FULL_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 11u;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "swapper";
    desc.flags = RG_MATERIAL_CUSTOM_UPDATE;
    desc.update_fn = test_custom_swap_update;
    desc.user_data = &swap_user;
    ASSERT_STATUS(rg_material_register(world, &desc, &swapper_id), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "neighbor", RG_MATERIAL_SOLID, 1.0f, &neighbor_id) == 0);

    memset(&write, 0, sizeof(write));
    write.material_id = swapper_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 1}, &write), RG_STATUS_OK);
    write.material_id = neighbor_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){2, 1}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_step(world, NULL), RG_STATUS_OK);
    ASSERT_TRUE(swap_user.call_count == 1u);

    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == neighbor_id);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){2, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == swapper_id);

    rg_world_destroy(world);
    return 0;
}

static int test_custom_update_transform_in_checkerboard(void)
{
    rg_world_t* world;
    rg_world_config_t cfg;
    rg_material_desc_t desc;
    rg_material_id_t transformer_id;
    rg_material_id_t target_id;
    rg_cell_write_t write;
    rg_cell_read_t read;
    rg_step_options_t step_options;
    test_custom_transform_user_t transform_user;
    test_runner_state_t runner_state;
    rg_runner_t runner;

    memset(&runner_state, 0, sizeof(runner_state));
    runner.vtable = &g_test_runner_vtable;
    runner.user = &runner_state;

    memset(&transform_user, 0, sizeof(transform_user));
    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 4;
    cfg.chunk_height = 4;
    cfg.default_step_mode = RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 123u;
    cfg.runner = &runner;
    ASSERT_STATUS(rg_world_create(&cfg, &world), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world, 0, 0), RG_STATUS_OK);

    ASSERT_TRUE(register_simple_material(world, "target", RG_MATERIAL_SOLID, 20.0f, &target_id) == 0);

    memset(&desc, 0, sizeof(desc));
    desc.name = "transformer";
    desc.flags = RG_MATERIAL_CUSTOM_UPDATE;
    desc.update_fn = test_custom_transform_update;
    transform_user.target_material_id = target_id;
    desc.user_data = &transform_user;
    ASSERT_STATUS(rg_material_register(world, &desc, &transformer_id), RG_STATUS_OK);

    memset(&write, 0, sizeof(write));
    write.material_id = transformer_id;
    ASSERT_STATUS(rg_cell_set(world, (rg_cell_coord_t){1, 1}, &write), RG_STATUS_OK);

    memset(&step_options, 0, sizeof(step_options));
    step_options.mode = RG_STEP_MODE_CHUNK_CHECKERBOARD_PARALLEL;
    step_options.substeps = 1u;
    ASSERT_STATUS(rg_world_step(world, &step_options), RG_STATUS_OK);

    ASSERT_TRUE(transform_user.call_count == 1u);
    ASSERT_TRUE(runner_state.call_count > 0u);
    ASSERT_STATUS(rg_cell_get(world, (rg_cell_coord_t){1, 1}, &read), RG_STATUS_OK);
    ASSERT_TRUE(read.material_id == target_id);

    rg_world_destroy(world);
    return 0;
}

static int test_custom_update_random_is_seeded(void)
{
    rg_world_t* world_a;
    rg_world_t* world_b;
    rg_world_config_t cfg;
    rg_material_desc_t desc;
    rg_material_id_t random_id_a;
    rg_material_id_t random_id_b;
    rg_cell_write_t write;
    rg_cell_read_t read_a;
    rg_cell_read_t read_b;
    test_custom_random_user_t random_user_a;
    test_custom_random_user_t random_user_b;
    test_cell_data_t payload;
    const test_cell_data_t* out_a;
    const test_cell_data_t* out_b;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chunk_width = 4;
    cfg.chunk_height = 4;
    cfg.default_step_mode = RG_STEP_MODE_FULL_SCAN_SERIAL;
    cfg.deterministic_mode = 1u;
    cfg.deterministic_seed = 9001u;

    memset(&random_user_a, 0, sizeof(random_user_a));
    memset(&random_user_b, 0, sizeof(random_user_b));

    ASSERT_STATUS(rg_world_create(&cfg, &world_a), RG_STATUS_OK);
    ASSERT_STATUS(rg_world_create(&cfg, &world_b), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world_a, 0, 0), RG_STATUS_OK);
    ASSERT_STATUS(rg_chunk_load(world_b, 0, 0), RG_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "random_custom";
    desc.flags = RG_MATERIAL_CUSTOM_UPDATE;
    desc.instance_size = (uint16_t)sizeof(test_cell_data_t);
    desc.instance_align = (uint16_t)_Alignof(test_cell_data_t);
    desc.update_fn = test_custom_random_update;
    desc.user_data = &random_user_a;
    ASSERT_STATUS(rg_material_register(world_a, &desc, &random_id_a), RG_STATUS_OK);

    desc.user_data = &random_user_b;
    ASSERT_STATUS(rg_material_register(world_b, &desc, &random_id_b), RG_STATUS_OK);

    payload.id = 0u;
    payload.temperature = 0;
    memset(&write, 0, sizeof(write));
    write.instance_data = &payload;

    write.material_id = random_id_a;
    ASSERT_STATUS(rg_cell_set(world_a, (rg_cell_coord_t){2, 2}, &write), RG_STATUS_OK);
    write.material_id = random_id_b;
    ASSERT_STATUS(rg_cell_set(world_b, (rg_cell_coord_t){2, 2}, &write), RG_STATUS_OK);

    ASSERT_STATUS(rg_world_step(world_a, NULL), RG_STATUS_OK);
    ASSERT_STATUS(rg_world_step(world_b, NULL), RG_STATUS_OK);
    ASSERT_TRUE(random_user_a.call_count == 1u);
    ASSERT_TRUE(random_user_b.call_count == 1u);

    ASSERT_STATUS(rg_cell_get(world_a, (rg_cell_coord_t){2, 2}, &read_a), RG_STATUS_OK);
    ASSERT_STATUS(rg_cell_get(world_b, (rg_cell_coord_t){2, 2}, &read_b), RG_STATUS_OK);
    ASSERT_TRUE(read_a.instance_data != NULL);
    ASSERT_TRUE(read_b.instance_data != NULL);
    out_a = (const test_cell_data_t*)read_a.instance_data;
    out_b = (const test_cell_data_t*)read_b.instance_data;
    ASSERT_TRUE(out_a->id == out_b->id);

    rg_world_destroy(world_a);
    rg_world_destroy(world_b);
    return 0;
}

int main(void)
{
    RUN_TEST(test_world_create_defaults);
    RUN_TEST(test_world_create_invalid_config);
    RUN_TEST(test_material_register_and_duplicate_rejection);
    RUN_TEST(test_chunk_load_unload);
    RUN_TEST(test_cell_set_get_clear_and_nonfungible_payload);
    RUN_TEST(test_ctor_dtor_behavior);
    RUN_TEST(test_step_and_stats);
    RUN_TEST(test_powder_falls_in_full_scan);
    RUN_TEST(test_liquid_flows_sideways_when_blocked);
    RUN_TEST(test_cross_chunk_fall);
    RUN_TEST(test_chunk_scan_sleep_and_wake);
    RUN_TEST(test_unloaded_chunk_cell_access);
    RUN_TEST(test_checkerboard_parallel_cross_chunk_with_runner);
    RUN_TEST(test_checkerboard_parallel_conflict_resolution);
    RUN_TEST(test_custom_update_try_move_with_payload);
    RUN_TEST(test_custom_update_try_swap);
    RUN_TEST(test_custom_update_transform_in_checkerboard);
    RUN_TEST(test_custom_update_random_is_seeded);

    printf("regolith tests passed\n");
    return 0;
}
