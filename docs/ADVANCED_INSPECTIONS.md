# Spatial, animation, runtime UI and timing inspections

These workflows extend the existing typed `unreal_workflow_inspect` calls. They
inspect bounded native state. Placement remains a preview followed by the
existing reviewed scene apply; inspection itself does not move an actor.

## Terrain support

Surface requests support these optional fields:

| Field | Default | Meaning |
| --- | --- | --- |
| `support_samples` | `5` | `1`, `5` or `9` downward traces: center, footprint corners, then edge centers. |
| `trace_complex` | `false` | Use existing triangle collision for the downward traces. |
| `max_support_variation_cm` | `10` | Maximum absolute distance of sampled hits from the center hit's support plane, between 0 and 1,000 cm. |

All first blocking hits must belong to `surface_paths` and respect the requested
slope limit. Corners and edges are sampled at 90% of the oriented footprint.
Missing corner support, an unapproved blocker, excessive slope or excessive
height variation makes `placement_valid` false. The result includes each probe,
the largest observed plane deviation, and a reason. A five- or nine-point result
still does not prove that the space between probes supports the whole object.

The final overlap check uses conservative simple collision bounds even when the
ground traces use complex collision. World Partition and worlds with streaming
levels are refused: absence of currently loaded collision cannot establish safe
placement. Physics settling, deformable surfaces and moving platforms remain
separate runtime acceptance tasks.

## Navigation geometry and actual pawn movement

Navigation requests may add `probe_geometry: true` and `probe_spacing_cm` between
1 and 200 (default 50). The existing Recast route remains separate from this
optional geometry result:

- `complete_path` reports native navigation success.
- `geometry_probe_complete` reports whether the bounded probe run completed.
- `geometry_clear` reports ground samples and capsule sweeps without detected
  gaps, excessive sampled height changes or blocking collision.

The capsule uses half the requested width as its radius and half the configured
nav-agent height as its half-height. Probes use the `Pawn` trace channel and
simple collision. There are at most 256 samples; longer paths require a larger
spacing or smaller separately reviewed route. Streaming worlds and unsupported
capsule proportions are refused. Capsule sweeps can conservatively reject a step
that real CharacterMovement would climb, and a clear sweep does not prove door
interaction, steering or dynamic obstacle behavior.

The sandbox adds `AJevPawnTraversalRecipe`, a project-owned native functional
test. It spawns its own transient corridor, a 20 cm step and a native Character,
then drives that Character through actual PIE movement ticks. It records travel
and step rise. A blocking-wall fault proves that the recipe detects failure.
Cleanup removes only owned subjects. Adapt this source recipe to a game's own
movement contract before treating it as evidence about that game's pawn.

## Animation and weights

Rig requests may add `inspect_skin_weights: true`. The report checks up to
65,536 imported LOD0 vertices across at most 256 sections, reporting unweighted
vertices, invalid section bone-map references and uint16 weight sums that differ
from full normalization. `complete_lod_checked` is false when data is missing or
the bound is reached; valid checked vertices do not establish complete coverage.

When an animation sequence is supplied, `root_motion_samples` selects 1–64
equal-duration intervals (default 8). Native root-track extraction reports each
interval and the full-sequence transform. Extraction is available only while
asset compilation is idle and reports an unavailable skeleton or unsupported
duration without a successful extraction. It does
not run animation graphs, fire notifies, move a character or prove retargeting.
The native fixture uses real authored root keys and deliberate skin-weight
defects; playback appearance and deformation still require runtime review.

## Runtime widgets

`kind: "widgets"` normally reads the stored Widget Blueprint tree. Adding
`runtime_instance_path` selects an already existing, on-screen instance of that
exact Blueprint in the active PIE world. The Blueprint must directly derive from
native `UserWidget`. No instance is created or ticked by inspection, and no text,
visibility or other binding is invoked.

The report includes current game viewport dimensions, accumulated layout scale,
Slate application scale, cached allocated/desired sizes and current keyboard
focus for up to 256 widgets. Desired size exceeding allocation and a child's
bounds extending outside its parent are review hints. They do not establish
pixel-visible overflow, transformed clipping, DPI correctness or accessibility.
Accumulated layout scale includes inherited scaling and is not an isolated DPI
measurement. Nested UserWidget internals, named slots and screen-reader output
are outside this traversal.

The rendered fixture inspects an actual PIE widget and saves wide/compact
allocation captures. These are two widget allocation sizes, not tests on two
physical devices or independent artist acceptance. It confirms that a removed
widget is refused and closes its own PIE session.

## Timing measurements

Performance receipts retain `editor_tick_interval`, the measured wall interval
between game-thread ticker visits. They also contain `engine_published_timings`:
game, render, RHI and GPU0 cycle-counter observations and distributions. Native
RenderTimer counters and `RHIGetGPUFrameCycles` provide these values. Zero or
unavailable counters are omitted from distributions and represented as null in
observations; a missing GPU counter is not zero GPU cost.

Counters are observed at most once per game frame. The observation's game-frame
ID is **not** the frame that produced every counter. Counters can lag, repeat or
refer to different production frames, so `frame_aligned` is explicitly false.
These statistics are not per-viewport, per-asset or packaged-game attribution.
The existing comparison tool continues comparing its declared ticker metric;
do not silently substitute an asynchronous GPU statistic for it.

For useful comparisons, keep the same viewport, workload, warm-up, camera and
editor settings. The named protocol records operator intent; it does not freeze
or verify those conditions. True frame-correlated CPU/render/GPU analysis remains
an Unreal Insights or GPU-profiler acceptance task.
