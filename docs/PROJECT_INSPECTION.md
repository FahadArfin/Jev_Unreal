# Blueprint, dependency and project-rule inspection

These local tools extend inspection beyond loaded scene actors. They preserve the
configured project identity, authenticated loopback connection and bounded request
schemas. They do not send assets to Jev or any cloud service.

## Blueprint inspection

`unreal_blueprint_inspect(asset_path, graph_limit=16, node_limit=128, pin_limit=512)`
reads one exact, already-loaded `/Game/.../Asset.Asset` or `/Engine/.../Asset.Asset`
native `UBlueprint`, `UWidgetBlueprint` or `UAnimBlueprint`. Open the Blueprint in
Unreal first if the response is `asset_not_loaded`. Other subclasses and redirectors
remain unsupported. Widget trees and animation runtime state are not graph inspection.
Use the separate [reviewed compile workflow](BLUEPRINT_WORKFLOWS.md) for fresh diagnostics.

The result contains existing compile status, parent/generated class, local variable
names/types/GUIDs, graph paths/GUIDs, node classes/GUIDs/positions, pin types/GUIDs,
links and stored node compiler messages. Defaults and node titles are omitted;
titles can invoke node-specific editor behavior. No graphs, pins or variables are
changed. The tool does not load or compile the Blueprint, mark its package dirty,
or run a construction script. A stored `up_to_date` status is not a new compile or
gameplay acceptance result; stored node messages are not a complete compiler log.

Limits apply across the response: at most 32 graphs, 256 nodes, 1,024 pins,
128 variables and eight links per pin. Text fields are capped, and `truncated`
reports omitted text/records. `graph_count`, `node_count`, `pin_count` and
`link_count` describe the observed source sizes. Stored graph arrays and subgraphs
are traversed with a visited set, at most 256 distinct graphs and 1,024 references;
plugin extension graph callbacks are never invoked. `graph_scan_complete` and
`extension_graphs_omitted` make partial coverage explicit; `graph_count` is a lower
bound when scanning is incomplete. The bridge's one-MiB response cap
still applies; reduce limits if it returns `response_too_large`. GUIDs are the
stored Unreal identities, not identities invented from display names.

## Dependency and import provenance

`unreal_asset_dependencies(asset_path, direction="dependencies", category="package",
offset=0, limit=100)` reads direct package-level Asset Registry edges. Direction can
also be `referencers`; categories are `package`, `manage` and `searchable_name`.
Results are sorted, with at most 200 edges per page and `next_offset` when more
remain. Offset is bounded to 100,000. There is no recursive graph traversal or
asset loading. Unreal's own direct-edge query materializes its edge list before
pagination; pagination bounds the response, not the engine's internal query cost.

Each edge carries its identifier, category and native property bits. Package edges
also expose `hard`, `game` and `build`; manage edges expose `direct` and `cook_rule`.
For UE 5.8 those bits are 1, 2, 4, 8 and 16 respectively. Package queries describe
the package containing the selected object, not exclusively that object's fields.
`registry_loading=true` means discovery is incomplete. Pages can change if the
registry updates between requests. Registry references are not proof of every
dynamic runtime reference or a definitive missing-reference diagnostic.

`unreal_asset_import_info(asset_path)` reads the registry's recorded `SourceFile`
metadata, returning up to 16 source basenames, recorded timestamps and recorded
MD5 values. Full source directories are removed; files are never opened or hashed.
Metadata is limited to 64 KiB before parsing. Missing/unparseable metadata has an
explicit flag and is not reported as a missing source file. The tool does not yet
certify DCC units, pivots, textures or successful round-trip export/import.

Epic describes the registry's discovery/loading distinction in its
[Asset Registry documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-registry-in-unreal-engine).

## Selected native Data Validation rules

Execution is **disabled by default**. A project maintainer can configure specific
loaded C++ `UEditorValidatorBase` classes in the project's `Config/DefaultGame.ini`:

```ini
[JevEditor.Validation]
bEnabled=true
MaxJobSeconds=30
+Rules=mesh-budget|/Script/MyGameEditor.MeshBudgetValidator
+Rules=asset-naming|/Script/MyGameEditor.AssetNamingValidator
```

These are illustrative class names: configure your own existing rules. IDs must be
unique and at most 64 characters; at most 64 rules can be configured. The class
must already be loaded, native, concrete and derived from `UEditorValidatorBase`.
An invalid configuration disables execution. The server cannot write this policy,
enable rules, select arbitrary classes, evaluate code or invoke commandlets.

`unreal_validation_rules()` lists configured aliases, class paths and availability
without invoking any validator. `unreal_validation_start(rule_ids, asset_paths)`
queues 1–8 unique approved rules for 1–20 unique exact asset paths. All paths must
exist in the registry before queueing. The MCP bridge automatically attaches the
authenticated preflight's exact project/session/world/revision. Native
`validation_start` requires `expected_project` plus `expected_state` (only
`session_id`, `world_path`, `revision`) and rejects any mismatch before queueing;
an editor replacement between the status read and mutation cannot run rules for
another session or project. `unreal_validation_job(job_id)` returns
progress and evidence; `unreal_validation_cancel(job_id)` retains completed results
and cancels remaining work. One job may run at a time.
The revision is checked again before the first queued callback; editing the scene
while the job waits cancels it with `revision_changed_before_start`.

Each editor tick processes one asset/rule pair through the actual
`UEditorValidatorBase::ValidateLoadedAsset` API in a fresh validator instance.
Results distinguish `valid`, `invalid` and `not_validated`; no applicable rule is
never silently treated as success. Job states are `queued`, `running`, `completed`,
`cancelled`, `timed_out` and `failed`; only a completed job with every result valid
has overall verdict `valid`. Invalid results dominate a completed verdict; any
skipped/not-validated result otherwise prevents a valid verdict.

Rules and asset loading run **trusted project code**. They can load dependent
assets, trigger load-time Blueprint compilation, modify state or emit their own
logs. The bridge cannot sandbox that code or promise it is side-effect-free.
It records whether the selected asset's package is dirty before/after the rule,
requests no automatic fix/save, and does not invoke every globally registered
validator, `UObject::IsDataValid`, recursive dependency validation or
`PostAssetValidation`. Rules requiring shared instances or global post-validation
aggregation are therefore not supported by this adapter.
Discovery and receipts expose `instance_lifetime` and
`post_asset_validation_called` so clients can check this contract explicitly.
Each result identifies `validator_class`, `validator_enabled`, and
`not_validated_reason` (`validator_disabled` or `not_applicable_or_no_verdict`).
`package_dirty_changed` describes only the observed selected-package dirty bit;
an already-dirty package can still change without a new transition.
Receipts use `save_requested=false`, `saved=null` and
`callback_side_effects_tracked=false`; they do not assert that project code never
saved or changed anything. Callback reentry cannot start or cancel another job.

Cancellation and the 1–120-second configured time budget are cooperative **between
calls**. A running validator or asset load blocks the editor game thread and cannot
be interrupted; elapsed time may exceed the configured budget. A timeout preserves
completed evidence and never passes the job. Project/session/world changes,
entering PIE/Simulate or revoking configuration cancel pending work. A final fresh
identity check happens after the last validator before completion.

Up to eight job records are retained in editor memory for 15 minutes after finish;
restarts discard them. Each asset/rule result includes counts, timing and at most
eight messages of 512 characters, with at most 256 messages per job. Truncation is
explicit. Messages can contain project data supplied by the project's validator;
they remain local unless the user or client shares them.

The integration uses UE 5.8's installed `EditorValidatorBase.h`,
`EditorValidatorSubsystem.h` and `Misc/DataValidation.h`. Epic's
[Data Validation documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/data-validation-in-unreal-engine)
describes the existing project-rule systems this adapter builds on.

## Validation coverage

The compatibility fixtures exercise the following contracts on the licensed local
engine. They are reproducible examples, not acceptance from outside production teams:

| Rule or pattern | Adapter coverage | Boundary |
| --- | --- | --- |
| Engine localization validator | Nonlocalized registered project asset receives its native verdict. | A project's localized asset tree still needs its own positive and negative fixtures. |
| Engine material validator | Disabled material-platform configuration remains `not_validated`. | No shader-platform compatibility or shader compilation claim follows from this case. |
| Project naming rule with `AssetPasses` / `AssetFails` / `AssetWarning` | Native helper diagnostics and verdicts are retained; mutable instance state is isolated per asset. | This tests the API conventions, not a universal naming standard. |
| Validator that dirties the selected package | Before/after dirty transition is reported and the adapter requests no save. | Changes to other objects or files are not comprehensively tracked. |
| Shared-instance or `PostAssetValidation` cleanup | Explicitly unsupported; global hooks are not called. | Rewrite the rule to finish within each callback or use Unreal's full validation subsystem outside this bridge. |

`Jev.Editor.ValidatorCompatibility` covers these cases and the queued revision
guard. Cancellation, timeout and policy revocation remain covered separately.

Native source fixtures create unsaved in-memory assets and a Blueprint with known
stored node diagnostics. `Jev.Editor.BlueprintInspection` checks identities,
read-only behavior, bounds and unsupported inputs; `AssetProjectInspection` checks
registry queries and source-path redaction. `ValidationJobs` and
`ValidationJobSafety` invoke a real native fixture validator and check positive,
negative, skipped, cancelled, revoked, timed-out and truncated results. Fixture
validators are disabled during normal editor use. These tests do not establish
compatibility with every project's validator, compile a production Blueprint or
certify a finished game. See [validation evidence](VALIDATION.md) for the actual
build/editor checks completed for this release.
