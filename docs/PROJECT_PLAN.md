# Regolith Project Plan (Draft)

This plan front-loads correctness and determinism before aggressive
optimization.

## Phase 0: Contract Lock-In

Deliverables:

- Proposal, architecture, and API sketch review.
- Default chunk sizing and payload policy decisions.
- Benchmark scene definitions and target metrics.

Exit criteria:

- Design docs accepted for v1 implementation.

## Phase 1: Core World Skeleton

Deliverables:

- `rg_world_t` lifecycle + allocator integration.
- Sparse loaded-chunk table.
- Basic cell read/write APIs.

Exit criteria:

- Unit tests for load/unload/get/set/clear behavior.
- Bounds and invalid-argument handling validated.

## Phase 2: Material Registry + Instance Data

Deliverables:

- Material registration with metadata validation.
- Inline per-cell payload storage path.
- Instance lifecycle hooks (ctor/dtor/move).

Exit criteria:

- Tests for material set/replace with payload preservation.
- Leak checks for ctor/dtor call balance.

## Phase 3: Serial Steppers

Deliverables:

- Full scan serial backend.
- Active chunk serial backend.
- Activity wake/sleep bookkeeping.

Exit criteria:

- Deterministic replay tests pass.
- Sparse-scene benchmark shows improvement of chunk serial vs full scan.

## Phase 4: Cross-Chunk Intents + Parallel Checkerboard

Deliverables:

- Intent buffering and deterministic merge policy.
- 4-color checkerboard scheduler.
- Runner-driven `parallel_for` integration with serial fallback.

Exit criteria:

- Parallel correctness parity with serial reference scenes.
- Verified multicore speedup in contention-light benchmark scenes.

## Phase 5: Overflow Payload Path

Deliverables:

- Overflow payload pools and handle lifecycle.
- Mixed inline/overflow material transitions.

Exit criteria:

- Tests for overflow allocation/free and stale-handle rejection.
- Stable performance under high churn payload scenarios.

## Phase 6: Diagnostics and Tooling

Deliverables:

- World stats and step counters.
- Benchmark harness with CSV or JSON output.
- Trace hooks for step phases and intent conflicts.

Exit criteria:

- Bench runs produce repeatable baseline reports.
- Debug counters match expected invariants in tests.

## Phase 7: Release Hardening

Deliverables:

- CI build + unit tests + benchmark smoke checks.
- Sanitizer runs (ASan/UBSan) for core tests.
- Doc/API synchronization pass.

Exit criteria:

- CI checks are green on pull requests.
- No known correctness regressions in baseline scenes.

## Initial Test Strategy

- Unit tests: chunk lifecycle, material lifecycle, cell APIs, bounds checks.
- Determinism tests: identical outcomes across repeated seeded runs.
- Stress tests: randomized insert/remove/transform sequences.
- Performance tests: dense fall scene, sparse cave scene, reaction-heavy scene.
