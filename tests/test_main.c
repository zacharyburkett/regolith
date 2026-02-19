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

    printf("regolith tests passed\n");
    return 0;
}
