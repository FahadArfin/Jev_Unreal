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

## 0.10 clean-host workflow acceptance

This is a reproducible acceptance procedure, not a claim that an untested machine
or every Unreal project works. Use a licensed supported Unreal installation and
a disposable project created specifically for acceptance. Jev_Unreal is an
independent MIT project, not an official Epic or TypeSafe product.

1. Install the exact release artifact into the disposable project using a reviewed
   setup plan. Keep the installer manifest and report. Confirm the expected
   project, plugin version, toolchain and port with setup inspection.
2. Build the plugin with the licensed engine. Save full build output privately;
   report the exact engine/plugin versions, exit result and relevant failures.
   Compilation alone does not prove an editor connection.
3. Initialize local bridge credentials through the secure helper. Configure the
   exact `.uproject` and one loopback endpoint/profile. Run doctor in the launcher's
   environment and retain its sanitized report. Confirm the actual connected
   project before issuing any edits.
4. Start with a small tool catalog such as `core,inspection,scene`. Discover the
   advertised tools and schemas, then run read-only compact inspection. Exercise
   a continuation page and a fresh delta. Confirm stale/mismatched cursors refuse.
5. In the disposable project, preview one measured spatial or layout recipe,
   inspect the review and explicitly apply it once. Verify fresh actor requirements
   and inspect a rendered capture. Try a stale preview and confirm it is refused.
6. With reviewed private runtime configuration, reconnect MCP between workflow
   preview and review. Confirm the durable checkpoint remains discoverable.
   Exercise observational reconciliation; never automatically replay an uncertain
   apply or treat a successful reconnection as transaction proof.
7. If file checkpoints are enabled, approve two disposable project files. Record
   a manifest, change one file intentionally, and confirm comparison identifies it
   while leaving the change intact. Unsaved editor buffers remain outside disk
   hashes. Verify Git/Perforce status separately; absent Perforce is an explicit
   untested/unavailable result, not a passing compatibility claim.
8. Run native `Jev.Editor` automation and the scoped bridge smoke workflow. Test
   visual and gameplay acceptance separately using the project's approved test
   fixture and existing PIE session. Include cancellation/cleanup evidence where
   supported. Pixel differences do not prove visual quality.
9. Provider checks are optional and separately authorized. Record provider
   authentication, typed response and accounting evidence without publishing keys,
   raw responses or private scene data. A failed provider check does not invalidate
   independently passing local editor tools.
10. Test uninstall/reinstall and modified-file preservation using the installer.
    Retain the exact acceptance evidence and artifact identity in the release
    report. Mark every unavailable platform, engine version or external service
    explicitly untested.

A public issue report should include version numbers, operating system, sanitized
doctor/setup summaries, the failing named workflow and a minimal public fixture.
Do not attach bridge tokens, provider keys, private runtime databases, proprietary
assets, raw provider payloads, engine binaries/source or entire project log trees.
Summarize the relevant failure and keep detailed sensitive logs local.

The scripted Python suite uses mocks and temporary files for many failure paths.
A real clean-host run, native build, rendered editor acceptance, Perforce server
test and paid-provider evaluation are distinct evidence categories. Publish only
the categories actually exercised for that release.
