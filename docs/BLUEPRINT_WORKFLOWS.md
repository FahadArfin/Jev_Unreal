# Blueprint inspection and reviewed compilation

`unreal_blueprint_inspect` reads already-loaded exact native `UBlueprint`,
`UWidgetBlueprint` and `UAnimBlueprint` assets. It returns stored graph/node/pin
identities, variables and existing diagnostics. Animation Blueprints additionally
identify their target skeleton; Widget Blueprints report their animation count.
It never loads or compiles the asset, runs graph-extension discovery callbacks,
returns variable defaults, or edits graphs. Custom asset subclasses are refused.
Read `graph_scan_complete`, `extension_graphs_omitted` and `truncated`: stored
graphs are not a guarantee of complete compiler or extension coverage.

## Fresh compiler diagnostics

Fresh compilation is a separate opt-in project capability. A project owner adds
exact aliases to `Config/DefaultGame.ini`, then restarts that project's editor:

```ini
[JevEditor.BlueprintCompilation]
bEnabled=true
+Targets=door|/Game/Blueprints/BP_Door.BP_Door
+Targets=hud|/Game/UI/WBP_HUD.WBP_HUD
+Targets=locomotion|/Game/Animation/ABP_Player.ABP_Player
```

The policy defaults to disabled. It accepts at most 64 unique aliases and exact
`/Game` object paths. Duplicate aliases/paths, malformed entries and oversized
policies disable execution. No client may supply a different path, compiler flags,
code, an arbitrary function name, a script, or a load request.

1. Open the approved asset in Unreal. Read `unreal_blueprint_compile_targets` and
   `unreal_blueprint_inspect`, then inspect the editor context.
2. Call `unreal_blueprint_compile_preview(target_id, expected_state)`, using the
   exact inspected session, world and revision. Review the returned asset,
   project, callback effects and plan ID. Preview does not compile.
3. Call `unreal_blueprint_compile(plan_id)` once. The native plan lasts 120 seconds
   and checks the project, editor state, exact live asset instance and policy
   again. Scene changes, notified object edits and Blueprint status/dirty changes
   invalidate it. The adapter requires normal Unreal edit notifications;
   unreported native memory writes are outside this stale-plan guarantee.
4. Read `status`, `error_count`, `warning_count`, `diagnostics` and
   `diagnostics_truncated`. A `failed` compiler receipt is an executed operation
   with diagnostic evidence; it is not a transport failure or permission to retry.
5. If the client connection times out, use `unreal_blueprint_compile_receipt` with
   the same plan ID before doing anything else. Retained receipts survive MCP
   reconnects for up to 15 minutes while this editor remains open. A missing
   receipt does not prove that an earlier compilation did not execute.

Fresh diagnostics are bounded to 128 messages of 1,024 characters. Native compiler
error/warning totals remain available when text truncates. Diagnostics may contain
private project information; review reports before sharing them.

## What compilation can change

Compilation executes trusted project and editor compiler callbacks, including
extensions, default-object validation and reinstancing. It can affect dependent
assets and existing instances. The adapter passes Unreal's `SkipSave` option and
reports `save_requested: false`; `saved: null` explicitly means callback-driven
saving is not comprehensively observed. It does not claim sandboxing, rollback,
transactional undo, cancellation, a hard time limit or complete side-effect
tracking. A slow callback can block the editor until Unreal returns.

PIE and simulation are refused. Compilation, validation and functional-test
execution are mutually exclusive through the bridge. Failed, stale and expired
commit attempts consume their plan. There is no automatic retry.

Successful compilation establishes compiler acceptance at that moment. It does
not establish widget layout quality, animation correctness, gameplay behavior,
packaged behavior or compatibility with every project compiler extension. Use
project-owned functional tests and human review for those acceptance decisions.

## Reviewed primitive pin edits (0.8)

`unreal_blueprint_pin_preview` adds a separate opt-in literal-edit plan. Enable
`bEnablePinEdits=true` alongside the existing exact compile target policy. Commit
with `unreal_blueprint_compile`; its receipt distinguishes the pin edit from the
fresh compiler verdict. Native Undo records the edit; compiler failure retains
it for explicit correction or Undo. See the [supported nodes and pin constraints](DOMAIN_WORKFLOWS.md#1-reviewed-blueprint-literal-edits).

## Reviewed math nodes and connections

`unreal_blueprint_graph_preview(target_id, graph_edit, expected_state)` adds a
separate opt-in graph plan. Enable `bEnableGraphEdits=true` in the same compilation
policy. Use graph/node/pin IDs returned by `unreal_blueprint_inspect`. The bridge
accepts exact native `UBlueprint` assets with editable K2 graphs; Widget and
Animation Blueprints retain inspection and compile support, but graph edits are
not enabled for their specialized graphs.

The closed edit vocabulary is:

| Operation | Fields in `graph_edit` | Acceptance rules |
| --- | --- | --- |
| `add_math_node` | `graph_id`, `function`, `x`, `y` | One native math node; integral coordinates in -100000..100000. |
| `remove_math_node` | `graph_id`, `node_id` | One allowed native node with no linked pins. |
| `connect` | `graph_id`, `output_node_id`, `output_pin_id`, `input_node_id`, `input_pin_id` | Same graph, distinct allowed nodes, exact matching primitive types, unoccupied input, schema-approved direct connection and no cycle. |
| `disconnect` | Same endpoint fields as `connect` | Exactly the existing reviewed connection. |

Every row also supplies `operation`. The function enum is `Add_IntInt`,
`Multiply_IntInt`, `Add_DoubleDouble`, `Multiply_DoubleDouble`, or `Not_PreBool`.
There is no arbitrary function discovery, execution node creation, coercion node
insertion, automatic link replacement, variable declaration or graph creation.
Addition returns the planned `added_node_id` for readback.

Preview captures a bounded stored graph snapshot, including node instance IDs,
positions, native function identities, primitive pin types/defaults and links.
The snapshot and edit eligibility are checked again at commit. Notified editor
changes and unnotified changes to captured graph fields invalidate the plan.
The bounds are 128 graphs, 2048 nodes in the selected graph, 8192 pins and 16384
stored link endpoints. This is not comprehensive observation of arbitrary
plugin-owned native memory.

Commit with `unreal_blueprint_compile(plan_id)`. The existing 120-second,
one-attempt rules apply, and compilation produces fresh bounded diagnostics.
The receipt records `graph_edit_applied` independently of the compiler verdict.
A native Undo transaction records the graph edit. Compiler failure retains the
edit for explicit Undo or a reviewed correction; it never silently retries or
claims compiler callback rollback. A successful compile does not prove the
new arithmetic is correct for the game's rules.
