# Regolith Proposal

This document defines the initial direction for Regolith before implementation.

## Problem Statement

The engine ecosystem needs a dedicated pixel-material simulation module that is:

1. Fast enough for large numbers of active cells.
2. Configurable enough for game-specific material behavior.
3. Flexible enough to support per-cell instance data (cells are not fungible).
4. Portable enough to use different threading backends without hard-coding SDL.

Ad hoc, game-local sand logic is difficult to optimize, hard to test, and
usually collapses under scale when behavior complexity grows.

## Goals

- High-throughput simulation for common powder/liquid/gas interactions.
- User-defined materials with clean registration/config APIs.
- Per-instance cell user data with predictable ownership/lifetime.
- Multiple update backends:
  - Full scan serial (baseline and correctness reference).
  - Chunk scan serial (active chunk acceleration).
  - Chunk checkerboard parallel (multicore scaling).
- Deterministic stepping option for debugging/replay.
- No mandatory SDL dependency in core runtime.

## Non-Goals (Initial Milestone)

- GPU/compute-shader simulation backend.
- Distributed/multiplayer state replication.
- Arbitrary scripting VM embedded in core library.
- Seamless infinite world streaming in v1 (loaded chunk API only).

## Key Constraints

- API is C-first and explicit about ownership.
- Material definitions must be data-driven first, callback-extensible second.
- Core runtime controls memory layout; per-cell user data must not force per-cell
  heap allocation in hot paths.
- Threading integration must be pluggable through a runner interface.

## Options Considered

### Option A: Full-grid AoS with serial stepping only

Pros:
- Very simple implementation.
- Easy to reason about update order.

Cons:
- Poor cache behavior for selective fields.
- Hard to scale with sparse activity and multicore CPUs.

### Option B: Chunked SoA with fixed-size per-cell payload

Pros:
- Strong cache locality.
- Simple and fast per-cell data access.
- Easy SIMD-friendly layout.

Cons:
- Payload size must be globally capped.
- Wasteful memory when most materials need no payload.

### Option C: Chunked SoA with hybrid payload (inline + overflow handles)

Pros:
- Fast inline path for common small payloads.
- Supports larger material-specific instance data.
- Preserves non-fungible cells without per-cell malloc.

Cons:
- More implementation complexity (overflow pools, handle validation).

## Recommended Direction

Use **Option C** with staged delivery:

1. Build chunked SoA core and inline payload path first.
2. Add overflow payload handles once baseline stepping is stable.
3. Add checkerboard parallel stepping through pluggable runners.

This keeps v1 tractable while preserving the extensibility needed for
material-rich games.

## Core Decisions

- Storage unit: fixed-size chunk (default `64 x 64` cells).
- Loaded state: sparse chunk table; only loaded chunks are simulated.
- Cell identity: stable coordinate-based identity plus per-cell payload state.
- Material registry: runtime registration with behavior and payload metadata.
- Update modes: full scan serial, active chunk serial, checkerboard parallel.
- Threading: runner abstraction (`parallel_for` contract), optional SDL adapter.

## Success Criteria

- Material registration + per-cell payload usage is stable and testable.
- Same initial seed/state produces identical output in deterministic mode.
- Chunk serial outperforms full scan on sparse-activity scenes.
- Parallel checkerboard mode shows speedup on multicore scenes with parity-safe
  behavior.
- No per-frame heap churn in steady-state benchmark scenes.
