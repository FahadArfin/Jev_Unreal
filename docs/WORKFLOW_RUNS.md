# Durable recipe runs

`unreal_workflow_run_preview` connects inspection, measured planning, review,
explicit application and fresh verification for spatial recipes (`align`,
`distribute`, `ground`, `snap_grid`) and blockout layouts (`grid`, `stairs`,
`room`). It uses existing native preview/apply operations, project policies,
leases and one-shot plans. It introduces no native execution surface.

Configure a reviewed `JEV_RUNTIME_CONFIG` first, including `preview` and `apply`
in its allowed actions and the required asset roots/operations. Its private
`state_directory` must remain outside the project tree. Recipe metadata lives
in a project-bound SQLite table, with at most 128 retained runs (or the lower
runtime receipt limit), at most 64 KiB per run and the configured retention
period. A full store refuses new runs rather than silently forgetting review
checkpoints. This storage is local/private; do not commit or share its database.

1. Call `unreal_workflow_run_preview` with a bounded recipe. Existing actors are
   inspected by exact path before spatial calculations. The result includes
   the native preview, generated requirements, `run_id` and `review_sha256`.
2. Review that concrete preview. Within the user's authorized scope, call
   `unreal_workflow_run_apply` with the exact run ID and review digest.
3. The run durably records its one allowed attempt **before** dispatch. The
   native identity, stale-plan, authentication and project-policy checks still
   apply. A separate fresh actor inspection verifies the generated requirements.
4. Review the resulting status and verification evidence. `verified` means
   those explicit actor requirements passed; it is not gameplay, visual-quality,
   save or production-readiness evidence.

```json
{
  "recipe": {
    "kind": "ground",
    "actor_paths": ["/Game/Example.Example:PersistentLevel.Crate"],
    "z_cm": 0.0
  }
}
```

Native previews expire after at most 120 seconds. A review digest is a binding
to the returned plan, not a source of permissions. Workflows never save a map,
restore files, undo transactions or automatically replay an operation.

## Reconnect and cancellation

Call `unreal_workflow_run` without an ID to list up to 20 retained runs after an
MCP reconnect. With `run_id`, it returns historical metadata. Set `reconcile:
true` to observe native plan history and, where exact instance-bound requirements
are available, fresh verification. Reconciliation never applies anything and
never resets the durable one-shot guard.

An undispatched, unexpired preview can still be applied after an MCP reconnect
when the editor session/world/revision and exact review digest match. Editor
restart or changed state requires a new preview. If an apply response was lost,
the run remains `outcome_uncertain` even if later observations support a likely
outcome. In particular, a layout's missing created-actor identities are not
reconstructed by guessing which actors look similar.

`unreal_workflow_run_cancel` abandons an undispatched local run. Its native
preview expires normally; cancellation is not a native transaction rollback and
does not revoke another client's independently authorized actions. An already
attempted apply cannot be cancelled or retried through this workflow.

Persisted records contain approved desired operations/checks, exact identities,
digests and statuses. They exclude raw inspection snapshots, provider payloads,
images and logs. An interrupted process can leave a `preparing` or
`preview_interrupted` record; create a new reviewed preview rather than resuming
that incomplete preparation automatically.

The Python regression suite uses synthetic editor responses and real temporary
SQLite storage to cover reconnects, atomic claims, stale state, cancellation,
lost responses, changed actor instances and fresh layout requirements. Those
checks are separate from live Unreal acceptance.
