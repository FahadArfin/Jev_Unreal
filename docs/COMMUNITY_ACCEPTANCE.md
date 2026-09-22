# Run a community acceptance session

The automated fixtures establish specific local behavior. Use this protocol to
collect the missing clean-host, real-project and human evidence. Record every
attempt, including blocked tasks. Do not put credentials, private game assets,
machine account paths or unredacted logs in public issues.

## Review panel: a 20-minute observed session

Use a disposable map with three native static mesh actors. Record the plugin
version, Unreal version, display scaling, panel width, input device, language and
assistive software/version. Ask a consenting participant who did not implement
the panel to perform these tasks; the observer must not silently fix problems:

1. Inspect the selected actors and identify their current position and units.
2. Preview a 25 cm translation and describe which fields will change.
3. Copy a before/after value using only the physical keyboard. Reading text and
   pressing Enter in that text must not apply the plan.
4. Apply deliberately once. Inspect the actors again and verify the measured change.
5. Undo in Unreal and explain why the historical applied receipt does not prove
   the current scene matches it.
6. Create another preview, edit the selection independently, then attempt Apply.
   Explain the rejection and create a new preview without replaying the old one.
7. Read a retained failure and its recovery guidance using assistive software.
   Verify that the announced name, role, value and enabled state agree with the
   visible control. Check that passive polling does not repeatedly announce text.
8. Repeat at a narrow dock width and with a reviewed translation or expanded-label
   test. Confirm the action label, selected review text and focus remain reachable.

For each task record pass/fail/blocked, elapsed time, assistance, observed behavior
and a redacted evidence reference. A synthetic key event is not a physical-keyboard
result; a native accessibility notification request is not proof it was spoken.
Expanded English labels are not a translated-language acceptance result.

## Installation: clean-host and upgrade matrix

Start from a clean Windows VM or separate machine with a licensed Unreal install.
Use disposable Blueprint-only and C++ projects. Record the OS, Python, compiler,
Windows SDK and exact engine/plugin versions, including missing dependencies.

1. Run `uv sync --locked --all-extras`, then `jev-unreal setup inspect --project
   PROJECT --source-plugin Plugins/JevEditor --engine-root ENGINE`. Diagnose each
   missing prerequisite before installing. Dependency metadata is not a build.
2. Generate a `setup plan`, inspect its changes, close the target editor, and
   `setup apply` the exact file. Build that project's editor target with its
   licensed engine, open it, and prove authenticated `doctor` and MCP discovery.
3. Install the previous tagged release in another disposable project, then upgrade
   to this release. Check project plugin entries, source hashes and retained backups.
4. Change an installed source file and repeat upgrade/removal. The installer must
   refuse to overwrite it. A reviewed manual reconciliation is separate evidence.
5. Interrupt installation in a disposable fixture. Use `setup recovery`, create a
   `setup recovery-plan`, review it, then `setup recover` that plan. Verify original
   files exactly, and verify later independent changes are preserved.
6. Exercise a Windows file-sharing lock and an active installer. Record safe refusal
   or rollback and the retained receipt; do not simply delete locks to force progress.

The repository's subprocess crash tests simulate process termination, not sudden
power loss, network-share failure or hostile filesystem races. Report those as
separate experiments if actually performed.

## Real-project rules and gameplay

Follow [validator contracts](PROJECT_INSPECTION.md) and
[gameplay recipes](GAMEPLAY_RECIPES.md). For each approved native rule record its
class, prerequisite configuration, valid/invalid/not-applicable assets, diagnostic
coverage, dirty-state changes, cancellation and cleanup requirements. Rules needing
global post-validation cleanup do not fit the selected-rule adapter.

Adapt each gameplay recipe to the project's real observable acceptance criteria.
Keep a passing fixture and one deliberate regression. Repeat execution and check
cleanup. A complete navigation path alone does not establish successful pawn travel.
For Blueprint compilation, use known good/broken project assets and inspect effects
on instances and dependent assets after the [reviewed compile](BLUEPRINT_WORKFLOWS.md).

## Productivity evidence

Use the preregistered [workflow study](BENCHMARKS.md) protocol. Freeze initial-state
and acceptance hashes before collecting observations. Compare direct-agent, keyword
and Jev methods with matched tools, model, project, hardware and intervention policy.
Record failures, abstentions, corrections and missing costs. Preserve evidence
locally and publish only a reviewed summary. Authored sandbox pilots must be labelled
as such; they are not an independent representative study.
