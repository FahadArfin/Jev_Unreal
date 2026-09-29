# Repeatable visual and gameplay acceptance

The acceptance tools combine existing native viewport capture, fresh selected-actor
verification and named project-owned functional tests. They help retain and compare
evidence after an edit. They never change the camera, start or stop PIE, retry a
test, load arbitrary files, execute scripts, or save a map.

These tools require `JEV_EXPECTED_PROJECT` and the existing authenticated loopback
bridge. Native test allowlists and team policies continue to govern test execution.
An acceptance result never grants permission to edit or execute project code.

## Capture → edit → capture → compare

1. Inspect `unreal_status`, the intended actors and viewport. Choose observable
   requirements for `unreal_verify` checks and explicit visual review criteria.
2. Call `unreal_acceptance_capture` with a short `protocol_id`, the current
   `expected_state`, optional `checks`, and `max_dimension` (64–1024, default 512).
   The result includes a native MCP image and `receipt_id`.
3. Make separately reviewed edits through existing preview/apply tools. An
   acceptance capture does not authorize or perform an edit.
4. Read current status and capture again with the same protocol, checks and image
   size. Use the new revision in `expected_state`; changes between captures are
   expected, while changes during either capture invalidate that capture.
5. Call `unreal_acceptance_compare` with the baseline and candidate receipt IDs.
   Set `include_images: true` to retrieve both images in baseline/candidate order.
   The default returns compact metadata and measurements without resending images.
6. Review the actual images against your criteria. Optionally repeat the comparison
   with a `visual_review` containing `reviewer`, `verdict` (`passed`, `failed`, or
   `inconclusive`), `criteria`, and `observations`.

Example capture request, after obtaining these values from the connected editor:

```json
{
  "protocol_id": "platform-clearance-v1",
  "expected_state": {
    "session_id": "COPY_FROM_UNREAL_STATUS",
    "world_path": "COPY_FROM_UNREAL_STATUS",
    "revision": "COPY_FROM_UNREAL_STATUS"
  },
  "max_dimension": 512,
  "checks": []
}
```

An empty check list explicitly means no geometric requirements were tested. Add
the existing bounded `unreal_verify` check forms to test labels, bounds, transforms,
materials, attachments, or clearance alongside the image. The same check list is
required for a comparable candidate. All fresh checks are bound to the capture's
project, session, world and revision; missing or unverifiable evidence cannot pass.

## What a comparison means

The server records viewport location, rotation, FOV, exposure, view mode, realtime
and motion-blur settings before and after each capture. Both captures must use the
same project/session/world, camera settings, protocol, requested maximum dimension,
actual dimensions and check definitions. A mismatch yields `inconclusive` with
specific reasons. Capture IDs must be distinct. Revision changes *between* captures
are allowed for the intended edit.

For native-compatible noninterlaced 8-bit RGB/RGBA PNGs, the result measures the
maximum absolute channel difference per pixel and reports the percentage whose
difference exceeds `pixel_tolerance` (0–255, default 0). Other valid PNG formats have
no pixel metric. Decoding validates compressed-stream boundaries and never allocates
beyond the already bounded image dimensions. No external imaging dependency is used.

**Pixel equality is not visual acceptance.** A misplaced object can look identical
in both images. Temporal antialiasing, exposure adaptation, shader compilation,
animation, lighting and simulation can also change pixels without a regression.
Matching camera settings and a protocol name do not freeze those systems or prove
equivalent workloads. Prepare and settle the scene yourself before capturing.

Without a visual review, the overall comparison stays `inconclusive` (or `failed`
when candidate checks fail). With matching conditions, passing or absent checks and
a declared passing review, the status is `passed_with_declared_visual_review`.
This is explicitly a **caller attestation**, not an automated quality judgment;
`automated_visual_quality_proven` remains false. Failed or unverifiable checks and
incompatible capture conditions cannot be overridden by that attestation.

## One approved gameplay test with before/after evidence

1. Configure the existing project `[JevEditor.FunctionalTesting]` allowlist. Discover
   test IDs with `unreal_functional_tests`; this workflow does not expand that list.
2. Start one standalone PIE session yourself. Keep a rendered, unlocked perspective
   level-editor viewport available. These captures show that editor viewport,
   **not the PIE game's rendered frame**.
3. Capture a fresh baseline in the current editor state.
4. Explicitly call `unreal_acceptance_playtest_start` with `baseline_id`, the approved
   `test_id` and matching `expected_state`. This runs trusted project callbacks and
   is marked as a tool with side effects. It requires enabled, available tests and
   an existing eligible PIE session. It returns a local `run_id` and native `job_id`.
5. Poll `unreal_acceptance_playtest_job(run_id)`. Each call reads once and returns;
   it never sleeps, retries or restarts a test. Use existing
   `unreal_functional_cancel(job_id)` for an explicit cancellation if necessary.
6. On terminal completion, the workflow attempts one fresh after-capture with the
   baseline's protocol and checks. Use its `after_capture_id` with the baseline in
   `unreal_acceptance_compare(include_images=true)` for visual review.

Passing gameplay evidence requires native `passed`/`Succeeded`, an actually started
test, cleanup attempted, zero observed errors and untruncated evidence. Missing,
failed, timed-out, cancelled, interrupted, unknown or incomplete results never pass.
If gameplay passes and comparable after-checks are satisfactory, the workflow says
`gameplay_passed_visual_review_required`. If the capture is unavailable, gameplay
evidence is retained but the overall result remains inconclusive. Cleanup attempted
does not prove that the scene was reset or that every callback side effect was undone.

Terminal results are cached historical receipts; repeated polls do not rerun the
capture or revalidate the current scene. Start a new explicitly reviewed run for
new evidence. A successful named test proves only what that project test checks.

## Retention and validation scope

Images remain in server process memory: up to 12 capture receipts, 8 MiB of retained
PNG data, and 32 playtest receipts, with a 30-minute expiry. Old receipts can be
evicted sooner. Reconnecting the server discards them. There is no arbitrary file
read/write or automatic evidence upload. The requesting MCP client may send image
content to its configured vision provider, just as with `unreal_capture`.

Unit tests use synthetic PNGs and mocked native responses to cover decompression
bounds, image metrics, identity/camera drift, expiration, selected checks, and test
failure/cleanup paths. These tests establish Python workflow behavior; they are not
live gameplay, visual acceptance or provider-efficiency evidence. See
[VALIDATION.md](VALIDATION.md) for separately recorded integration evidence.
