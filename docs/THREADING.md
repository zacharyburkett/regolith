# Regolith Threading Contract (Draft)

This document defines thread-safety guarantees and runner integration.

## Current Guarantee (Target v1)

- World mutation APIs are not externally thread-safe.
- One `rg_world_t` is stepped by one caller at a time.
- Internal parallelism is used only inside `rg_world_step` when mode is
  checkerboard parallel and a runner is configured.
- Read-only inspection APIs (stats/cell reads) are safe only when no step is in
  progress unless the caller provides external synchronization.

## Runner Contract

`rg_runner_t` is an adapter object owned by the caller.

Required semantics for `parallel_for`:

- Executes `task_count` logical tasks.
- Invokes each `task_index` exactly once.
- May execute tasks concurrently on multiple workers.
- Is synchronous: returns only after all task invocations complete.
- Propagates failure as `rg_status_t` when backend reports one.

Optional semantics:

- `worker_count` may return `1` if unknown.
- Implementations may use a fixed pool, ad hoc threads, or caller-owned job
  system.

## SDL3 Compatibility Strategy

Regolith core does not include SDL headers or symbols.

- Provide a separate adapter module (for example `regolith_sdl_runner`) that
  implements `rg_runner_t` with SDL threading primitives.
- Core only depends on runner callbacks, so SDL and non-SDL hosts use the same
  simulation library binary interface.

## Checkerboard Parallel Behavior

- Chunk colors are processed in fixed sequence: `00`, `01`, `10`, `11`.
- Each color phase dispatches eligible chunks through `parallel_for`.
- A barrier exists at end of each phase (implicit by synchronous `parallel_for`).
- Cross-chunk intents emitted in phase `N` are merged before phase `N+1`.

This prevents neighboring chunk write races while allowing multicore execution.

## Determinism Notes

Deterministic mode is defined by stable outcomes across runs with the same:

- initial world state
- deterministic seed
- step mode
- chunk size/configuration

Determinism is preserved by:

- stable chunk ordering inside each color phase
- deterministic intent merge ordering
- RNG keyed by world/step/chunk/cell identifiers (not worker scheduling)

## Material Callback Safety Rules

Material callbacks must:

- treat `rg_update_ctx_t` as the only mutation surface
- avoid direct mutation of global/shared mutable state unless externally guarded
- assume they may run concurrently on different chunks in parallel mode

Callback reentrancy requirements:

- callbacks are reentrant across distinct cells/chunks
- callbacks are not concurrently invoked for the same cell

## Lifecycle Constraints

- Destroying a world must not race with any API call on that world.
- Runner lifetime must outlive any in-flight `rg_world_step` call.
- If a runner has its own shutdown/flush API, callers must coordinate shutdown
  before world destruction.
