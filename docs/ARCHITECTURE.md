# Regolith Architecture (Draft)

This is the proposed runtime architecture before implementation.

## Repository Layout (Target)

- `include/regolith/`: public API headers
- `src/`: runtime implementation
- `tests/`: unit and regression tests
- `apps/bench/`: benchmark harness and scenario runner
- `docs/`: proposal, architecture, threading, API, and plan

## Core Runtime Objects

- `rg_world_t`: owns simulation state, registries, and stepping config.
- `rg_chunk_t`: storage and metadata for one fixed-size cell block.
- `rg_material_id_t`: dense runtime ID assigned at material registration.
- `rg_material_desc_t`: immutable material metadata + behavior hooks.
- `rg_runner_t`: pluggable parallel execution backend.

## World Model

Regolith uses a sparse loaded-chunk world:

- World coordinates are integer cell coordinates.
- Chunks are addressed by `(chunk_x, chunk_y)`.
- Only loaded chunks are allocated and updated.
- Each chunk tracks activity so idle chunks can be skipped.

This model supports both bounded maps and large streamed worlds.

## Chunk Data Layout

Each chunk stores cells in structure-of-arrays form:

- `material_id[cell_count]`
- `cell_flags[cell_count]`
- `inline_payload[cell_count * inline_payload_bytes]`
- `overflow_handle[cell_count]` (`0` means inline/none)
- `updated_mask[cell_count/8]` (optional pass-local visitation bitset)

Chunk metadata:

- activity state (`active`, `sleeping`, wake generation)
- dirty border masks (for neighbor wakeups)
- deterministic iteration key (Morton/hash order key)

The SoA shape keeps hot fields contiguous and branch-light in scan loops.

## Per-Cell User Data (Non-Fungible Cells)

Each cell has material type plus instance state:

- Materials declare `instance_size` and `instance_align`.
- If `instance_size <= inline_payload_bytes`, data is stored inline.
- Larger instance data is stored in overflow pools and referenced by handle.
- Material lifecycle hooks manage ctor/dtor/move between storage locations.

This keeps per-cell identity and custom state without per-cell heap allocations
in hot update paths.

## Material System

Material registration captures:

- debug name and ID
- high-level category flags (solid/powder/liquid/gas/static/custom)
- physical coefficients (density, friction, dispersion, etc.)
- reaction table entries (optional data-driven transforms)
- optional update callback for advanced custom behavior
- instance payload layout and lifecycle hooks

Common materials should be configurable through data only; callbacks are for
behavior that cannot be expressed by standard movement/reaction rules.

## Update Pipeline

Regolith provides three stepping backends under one API:

1. **Full Scan Serial**
   - Iterates all loaded chunks/cells in deterministic order.
   - Baseline reference mode for correctness and debug.

2. **Chunk Scan Serial**
   - Iterates only active chunks and optionally active rows.
   - Wakes neighbor chunks when border interactions occur.

3. **Chunk Checkerboard Parallel**
   - Uses 4-color chunk parity (`(x&1, y&1)`) to avoid adjacent chunk conflicts.
   - Processes one color phase at a time; chunks in phase run in parallel.
   - Cross-chunk moves are emitted as intents and merged deterministically at
     phase barriers.

## Cross-Chunk Move Handling

Within a chunk update:

- Intra-chunk moves commit immediately.
- Cross-chunk writes are recorded as move intents (target chunk + target cell).

At each phase barrier:

- Intents are grouped by target chunk.
- Intent order is canonicalized (target cell, source chunk key, source cell).
- Winners are applied; losers are dropped or converted to no-op based on policy.

This removes race conditions while keeping deterministic behavior independent of
thread scheduling.

## Determinism Strategy

Deterministic mode requires:

- Stable chunk iteration order.
- Stable cell traversal order (with explicit parity/phase policy).
- Deterministic intent conflict resolution.
- RNG derived from `(world_seed, tick, chunk_coord, cell_index)` instead of
  thread-local generator state.

When deterministic mode is disabled, backends may allow faster non-stable work
distribution.

## Threading and Runner Integration

Core runtime does not depend on SDL directly.

- `rg_runner_t` exposes a synchronous `parallel_for` primitive.
- A serial fallback runner is included by default.
- SDL3 integration is provided by an adapter layer that implements the runner
  contract using SDL thread/mutex/cond primitives.

Runner details and guarantees are specified in `docs/THREADING.md`.

## Memory Strategy

- World-level allocator hooks (no global allocator assumptions).
- Chunk pools for fast load/unload and reuse.
- Overflow payload pools bucketed by material and/or size class.
- Optional reserve APIs for chunk table/material capacity.

## Observability

Stats and trace counters should include:

- loaded chunks, active chunks, sleeping chunks
- live non-empty cells
- per-step intent counts and conflict counts
- payload overflow allocations/frees
- step time split by phase and backend

These counters support profiling and backend comparisons.

## Complexity Targets

- Cell read/write by coordinate: amortized O(1)
- Material set/replace: O(1) average + lifecycle hook cost
- Chunk stepping: O(active_cells) in active-set modes
- Cross-chunk merge: O(intent_count log intent_count_per_target) worst-case
