# Project-owned gameplay checks

`unreal_functional_tests`, `unreal_functional_start`, `unreal_functional_job` and
`unreal_functional_cancel` expose a bounded part of Unreal's native
`AFunctionalTest` lifecycle. They run a named project-owned test in **one already
running standalone Play In Editor session**. They never launch/stop PIE, load a
level, select arbitrary classes, pass arbitrary test arguments, retry a test or
save assets. Simulate, multiplayer PIE, packaged sessions and remote devices are
outside this version's scope.

## Project policy and use

A maintainer places an `AFunctionalTest` in the loaded editor map and configures
its exact editor actor path in `Config/DefaultGame.ini`:

```ini
[JevEditor.FunctionalTesting]
bEnabled=true
MaxJobSeconds=30
+Tests=door-opens|/Game/Maps/TestMap.TestMap:PersistentLevel.DoorFunctionalTest
```

This path is illustrative. Use the exact loaded editor actor path, not its label
or PIE-prefixed counterpart. Execution defaults to disabled. At most 64 unique
aliases are allowed; IDs use ASCII letters, digits, underscore, dash and period,
up to 64 characters. Invalid configuration disables execution. The bridge cannot
edit this configuration or create new approved test aliases.

1. `unreal_functional_tests()` lists configured aliases, loaded actor availability,
   enabled state and whether a single standalone PIE world is ready.
2. Start Play in Unreal yourself, with one standalone player.
3. Read `unreal_status()` and select an approved test.
4. Call `unreal_functional_start(test_id, expected_state)` with that exact
   `session_id`, editor `world_path` and `revision`.
5. Poll `unreal_functional_job(job_id)`. The result is `passed` only after the
   actual native test reports `Succeeded`, no observed functional-test errors
   contradict it, the original identities still match, and cleanup has returned.

Cancellation uses `unreal_functional_cancel(job_id)` and only stops this job's
test. It leaves the PIE session running. The adapter captures the exact editor
actor, its `EditorUtilities::GetSimWorldCounterpartActor` result and the PIE world
as weak object identities; replacements or another PIE world cannot satisfy the
old job. The native `RunFrame`/`RunTime` pair also binds the run, so a later restart
on the same actor cannot be cancelled or attributed to an old receipt. The
expected editor revision is checked again before starting on a later
tick. Once started, test code runs in PIE and the editor revision is no longer a
gameplay pass condition.

Only one functional job runs at a time, and the adapter refuses to start while
another native functional test is already running in that PIE world. Test scans
are capped at 1,000 actors and refuse execution when isolation cannot be checked.
The server also coordinates this with Data Validation jobs.
MCP starts are refused while another Unreal automation test is active; the native
regression fixture has a private C++ opt-in for its controlled automation context.

## Results and cleanup

States are `queued`, `running`, `passed`, `failed`, `error`, `timed_out`,
`cancelled` and `interrupted`. The receipt includes the native result, elapsed
time, whether execution began, cleanup status, exact editor/PIE actor paths and a
bounded failure message. A missing world, replaced actor, revoked configuration or
another test taking ownership yields interruption, never success. The adapter
does not reset existing test results before executing or honor automatic reruns.

For its surviving owned test, the adapter calls `FinishTest(Error, ...)` when
stopping an active run, followed by `CleanUp()`. When the test completes normally,
it captures the native result and calls `CleanUp()`. If PIE has ended or the
original test was destroyed, cleanup cannot be promised; `cleanup_attempted` is
false and the project test's own `EndPlay` handling is responsible for teardown.
Identity is checked again after finish callbacks and cleanup, because callbacks
can end PIE or destroy/restart the test. Such changes cannot produce a pass.
`cleanup_attempted=true` means the project's cleanup callback returned, not that
the bridge can prove every side effect was undone.

Tests and cleanup are trusted project code. They can spawn/destroy actors, run
Blueprint/C++ logic, access services, write project logs or implement their own
saving. The bridge requests no save and cannot sandbox those callbacks. Project
maintainers should approve tests with explicit setup and teardown appropriate for
the current project. This permission is independent of any Jev recommendation or
confidence score.
Receipts explicitly use `save_requested=false`, `saved=null` and
`callback_side_effects_tracked=false` rather than asserting that arbitrary project
callbacks never saved. Reentrant callback requests cannot mutate job ownership.

The configured 1–120-second time budget and cancellation are cooperative between
game-thread callbacks. A blocking test callback cannot be interrupted, and cleanup
can itself block. A deadline reached before the adapter observes success is a
timeout. Up to 64 receipts remain in editor memory for 15 minutes after completion;
they are lost on editor restart. Failure text is capped at 1,024 characters with
an explicit truncation flag; it is selected native failure evidence, not a full
gameplay log or assertion transcript.
The adapter observes error-level `LogFunctionalTest` messages during its isolated
run, retaining at most 32 messages of 512 characters. An explicit `Succeeded`
result cannot override observed assertion errors. In the native automation fixture,
framework error/expected-diagnostic deltas are also observed because Unreal routes
assertions directly to the active automation test. This is selected native error
evidence, not complete logs or proof that unreported project assertions passed.

## What is tested

`Jev.Editor.FunctionalJobs` is a latent native automation suite built on
`FFunctionalTestBase`. Its unsaved source-only fixtures start a dedicated test PIE
session with Unreal's `FStartPIECommand`, move a probe actor to expected coordinates,
observe positive, negative and failed-assertion-followed-by-success results,
exercise callbacks that destroy the test during finish/cleanup, check stale-state and
allowlist refusal, cancellation, permission revocation and timeout, then end that
fixture session with `FEndPlayMapCommand` and check interruption evidence.
It also replaces a run on the same actor on a later frame and verifies that
cancelling the old job leaves the replacement running.

The production tool never starts or stops PIE; those actions belong solely to the
isolated automation fixture. This verifies a real native gameplay lifecycle but
does not prove a project's door, navigation or combat behavior. Those require
project-authored tests and their own evidence. See [validation evidence](VALIDATION.md)
for completed runs and remaining limitations.

Epic documents the existing setup, completion and cleanup model in
[Functional Testing](https://dev.epicgames.com/documentation/en-us/unreal-engine/functional-testing-in-unreal-engine).
The implementation follows the installed UE 5.8 `FunctionalTest.h/.cpp` and uses no
arbitrary test command or code execution endpoint.
