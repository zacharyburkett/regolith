#include "regolith/runner_sdl.h"

#include <SDL3/SDL.h>

#include <stdlib.h>

typedef struct rg_sdl_parallel_ctx_s {
    rg_parallel_task_fn task;
    void* task_user_data;
    uint32_t task_count;
    uint32_t next_index;
    SDL_Mutex* mutex;
} rg_sdl_parallel_ctx_t;

typedef struct rg_sdl_worker_args_s {
    rg_sdl_parallel_ctx_t* ctx;
    uint32_t worker_index;
} rg_sdl_worker_args_t;

struct rg_sdl_runner_s {
    rg_runner_t runner;
    uint32_t worker_count;
};

static uint8_t rg_sdl_claim_task(rg_sdl_parallel_ctx_t* ctx, uint32_t* out_task_index)
{
    uint32_t task_index;

    if (ctx == NULL || out_task_index == NULL || ctx->mutex == NULL) {
        return 0u;
    }
    SDL_LockMutex(ctx->mutex);

    task_index = ctx->next_index;
    if (task_index < ctx->task_count) {
        ctx->next_index = task_index + 1u;
    } else {
        task_index = UINT32_MAX;
    }

    SDL_UnlockMutex(ctx->mutex);

    if (task_index == UINT32_MAX) {
        return 0u;
    }

    *out_task_index = task_index;
    return 1u;
}

static int rg_sdl_worker_main(void* user_data)
{
    rg_sdl_worker_args_t* worker_args;
    rg_sdl_parallel_ctx_t* ctx;

    worker_args = (rg_sdl_worker_args_t*)user_data;
    if (worker_args == NULL || worker_args->ctx == NULL || worker_args->ctx->task == NULL) {
        return 0;
    }

    ctx = worker_args->ctx;
    while (1) {
        uint32_t task_index;

        if (rg_sdl_claim_task(ctx, &task_index) == 0u) {
            break;
        }
        ctx->task(task_index, worker_args->worker_index, ctx->task_user_data);
    }

    return 0;
}

static uint32_t rg_sdl_runner_worker_count(void* runner_user)
{
    const rg_sdl_runner_t* runner;

    runner = (const rg_sdl_runner_t*)runner_user;
    if (runner == NULL || runner->worker_count == 0u) {
        return 1u;
    }
    return runner->worker_count;
}

static rg_status_t rg_sdl_runner_parallel_for(
    void* runner_user,
    uint32_t task_count,
    rg_parallel_task_fn task,
    void* task_user_data)
{
    rg_sdl_runner_t* runner;
    uint32_t worker_count;
    uint32_t thread_count;
    rg_sdl_parallel_ctx_t ctx;
    rg_sdl_worker_args_t main_worker_args;
    SDL_Thread** threads;
    rg_sdl_worker_args_t* worker_args;
    uint32_t created_threads;
    uint32_t i;

    if (task == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    if (task_count == 0u) {
        return RG_STATUS_OK;
    }

    runner = (rg_sdl_runner_t*)runner_user;
    if (runner == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }

    worker_count = (runner->worker_count == 0u) ? 1u : runner->worker_count;
    if (worker_count == 1u || task_count == 1u) {
        for (i = 0u; i < task_count; ++i) {
            task(i, 0u, task_user_data);
        }
        return RG_STATUS_OK;
    }

    thread_count = worker_count;
    if (thread_count > task_count) {
        thread_count = task_count;
    }

    threads = NULL;
    worker_args = NULL;
    created_threads = 0u;

    ctx.task = task;
    ctx.task_user_data = task_user_data;
    ctx.task_count = task_count;
    ctx.next_index = 0u;
    ctx.mutex = SDL_CreateMutex();
    if (ctx.mutex == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }

    if (thread_count > 1u) {
        threads = (SDL_Thread**)malloc((size_t)(thread_count - 1u) * sizeof(*threads));
        worker_args = (rg_sdl_worker_args_t*)malloc((size_t)(thread_count - 1u) * sizeof(*worker_args));
        if (threads == NULL || worker_args == NULL) {
            free(threads);
            free(worker_args);
            SDL_DestroyMutex(ctx.mutex);
            return RG_STATUS_ALLOCATION_FAILED;
        }

        for (i = 0u; i < thread_count - 1u; ++i) {
            worker_args[i].ctx = &ctx;
            worker_args[i].worker_index = i + 1u;
            threads[i] = SDL_CreateThread(rg_sdl_worker_main, "rg_worker", &worker_args[i]);
            if (threads[i] == NULL) {
                break;
            }
            created_threads += 1u;
        }

        if (created_threads != (thread_count - 1u)) {
            uint32_t join_index;
            for (join_index = 0u; join_index < created_threads; ++join_index) {
                SDL_WaitThread(threads[join_index], NULL);
            }
            free(threads);
            free(worker_args);
            SDL_DestroyMutex(ctx.mutex);
            return RG_STATUS_ALLOCATION_FAILED;
        }
    }

    main_worker_args.ctx = &ctx;
    main_worker_args.worker_index = 0u;
    (void)rg_sdl_worker_main(&main_worker_args);

    for (i = 0u; i < created_threads; ++i) {
        SDL_WaitThread(threads[i], NULL);
    }

    free(threads);
    free(worker_args);
    SDL_DestroyMutex(ctx.mutex);
    return RG_STATUS_OK;
}

static const rg_runner_vtable_t g_rg_sdl_runner_vtable = {
    rg_sdl_runner_parallel_for,
    rg_sdl_runner_worker_count
};

rg_status_t rg_sdl_runner_create(uint32_t worker_count, rg_sdl_runner_t** out_runner)
{
    rg_sdl_runner_t* runner;

    if (out_runner == NULL) {
        return RG_STATUS_INVALID_ARGUMENT;
    }
    *out_runner = NULL;

    if (worker_count == 0u) {
        worker_count = 1u;
    }

    runner = (rg_sdl_runner_t*)malloc(sizeof(*runner));
    if (runner == NULL) {
        return RG_STATUS_ALLOCATION_FAILED;
    }

    runner->runner.vtable = &g_rg_sdl_runner_vtable;
    runner->runner.user = runner;
    runner->worker_count = worker_count;

    *out_runner = runner;
    return RG_STATUS_OK;
}

void rg_sdl_runner_destroy(rg_sdl_runner_t* runner)
{
    free(runner);
}

const rg_runner_t* rg_sdl_runner_get_runner(const rg_sdl_runner_t* runner)
{
    if (runner == NULL) {
        return NULL;
    }
    return &runner->runner;
}
