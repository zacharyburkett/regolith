# Regolith

Regolith is a high-performance 2D pixel-material simulation library ("falling
sand" style) for the Ardent ecosystem.

Current implementation includes:

- World/material/chunk lifecycle
- Non-fungible per-cell inline instance data
- Serial full-scan and active-chunk stepping
- Checkerboard chunk stepping with runner-based parallel dispatch
- Deterministic cross-chunk conflict merge in checkerboard mode

Optional SDL3 runner adapter target:

- Enable with `-DREGOLITH_BUILD_SDL_RUNNER=ON`
- Public adapter header: `include/regolith/runner_sdl.h`

Planning docs:

- `docs/PROPOSAL.md`
- `docs/ARCHITECTURE.md`
- `docs/API_SKETCH.md`
- `docs/THREADING.md`
- `docs/PROJECT_PLAN.md`
