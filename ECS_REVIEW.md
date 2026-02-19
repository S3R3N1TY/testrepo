# ECS Migration Status

The ECS migration plan is now implemented for this codebase scope.

## Implemented

- Engine-owned ECS world lifecycle and capacity reservation.
- Generation-based `Entity` handles with index reuse and sparse-set component pools.
- View/query API with smallest-pool iteration and const-correct traversal.
- Phase-based `SystemScheduler` with declared read/write component metadata.
- Parallel system execution per phase using conflict-aware independent batches.
- Engine-owned render extraction from ECS data to `FrameGraphInput`.
- Broader component model:
  - `Transform` (position / rotation / scale)
  - `LinearVelocity`
  - `AngularVelocity`
  - `MeshRef`
  - `RenderVisibility`
  - `RenderLayer`
  - `Lifetime`
- Gameplay sample updated to configure ECS data + systems only.
- Engine frame metrics logging (alive count, draw count, sim ms, extraction ms).
- Dedicated CPU-only benchmark executable (`ecs_benchmark`) and CI-style perf gate test (`ecs_benchmark_10k`).

## Notes

- Scheduler parallelism is conflict-aware at batch level and deterministic by phase.
- The render path remains Vulkan render-graph based; ECS feeds extraction data into that pipeline.
