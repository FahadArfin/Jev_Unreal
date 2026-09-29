"""Durable recipe tests use SQLite and synthetic editor responses, never an engine."""

import asyncio
import json
import threading
from copy import deepcopy
from types import SimpleNamespace

import pytest

from jev_unreal.errors import JevError
from jev_unreal.layouts import PreviewTracker
from jev_unreal.runtime_state import RuntimeStore
from jev_unreal.team_policy import RuntimeConfig
from jev_unreal.workflow_runs import WorkflowRuns

ACTOR = "/Temp/Map.Map:PersistentLevel.Cube"
RECIPE = {"kind": "ground", "actor_paths": [ACTOR], "z_cm": 0.0}


class Editor:
    def __init__(self, tmp_path):
        project = tmp_path / "project"
        project.mkdir()
        file = project / "Test.uproject"
        file.write_text("{}")
        self.config = RuntimeConfig(
            version=1, project_file=str(file), state_directory=str(tmp_path / "private")
        )
        self.store = RuntimeStore(self.config)
        self.settings = SimpleNamespace(expected_project=str(file))
        self.infrastructure = SimpleNamespace(
            require=lambda: self.store, policy=SimpleNamespace(hash="a" * 64)
        )
        self.state = {
            "project_file": str(file),
            "session_id": "session",
            "world_path": "/Temp/Map.Map",
            "revision": "before",
            "current_level": "/Temp/Map.Map:PersistentLevel",
        }
        self.actors = [self.actor(ACTOR, [50.0, 50.0, 100.0])]
        self.operations = []
        self.calls = []
        self.applied = False
        self.interruption = None
        self.hook = None

    @staticmethod
    def actor(path, location, label="Cube", scale=None, rotation=None):
        scale = scale or [1.0, 1.0, 1.0]
        size = [100 * x for x in scale]
        return {
            "path": path,
            "instance_id": "instance-" + path[-1],
            "label": label,
            "class": "/Script/Engine.StaticMeshActor",
            "folder": "",
            "location": location,
            "rotation": rotation or [0.0, 0.0, 0.0],
            "scale": scale,
            "static_mesh_path": "/Engine/BasicShapes/Cube.Cube",
            "collision_enabled": True,
            "materials": [],
            "materials_truncated": False,
            "editable": True,
            "edit_blockers": [],
            "bounds_available": True,
            "bounds_cm": {
                "size": size,
                "center": location,
                "min": [x - y / 2 for x, y in zip(location, size, strict=True)],
                "max": [x + y / 2 for x, y in zip(location, size, strict=True)],
            },
        }

    async def call(self, action, params=None):
        self.calls.append(action)
        params = params or {}
        if self.hook:
            self.hook(action)
        if action == "status":
            return deepcopy(self.state)
        if action == "actor_details":
            return {
                **self.state,
                "truncated": False,
                "actors": deepcopy([a for a in self.actors if a["path"] in params["actor_paths"]]),
            }
        if action == "preview":
            self.operations = deepcopy(params["operations"])
            return {
                "plan_id": "plan",
                "operations": self.operations,
                "expires_in_seconds": 120,
                **self.state,
            }
        if action == "apply":
            self.applied = True
            for index, op in enumerate(self.operations):
                value = self.actor(
                    op.get("actor_path", ACTOR + str(index)),
                    op["location"],
                    op.get("label", "Cube"),
                    op["scale"],
                    op["rotation"],
                )
                if op["op"] == "set_transform":
                    self.actors[index] = value
                else:
                    self.actors.append(value)
            self.state["revision"] = "after"
            if self.interruption:
                raise self.interruption
            return {
                "applied": True,
                **self.state,
                "actors": deepcopy(
                    self.actors if self.operations[0]["op"] == "set_transform" else self.actors[1:]
                ),
            }
        if action == "plan_status":
            return {
                "plan_id": "plan",
                "session_id": self.state["session_id"],
                "status": "applied" if self.applied else "pending",
            }
        raise AssertionError(action)


@pytest.fixture
def editor(tmp_path):
    return Editor(tmp_path)


def workflows(editor):
    return WorkflowRuns(editor, PreviewTracker(editor))


async def test_spatial_inspect_review_apply_and_independent_fresh_verification(editor):
    runs = workflows(editor)
    preview = await runs.preview(RECIPE)
    assert preview["status"] == "awaiting_review" and "apply" not in editor.calls
    result = await runs.apply(preview["run_id"], preview["review_sha256"])
    assert result["status"] == "verified"
    assert result["verification"]["status"] == "passed"
    assert editor.calls[-1] == "actor_details"
    assert result["saved"] is False
    with pytest.raises(JevError, match="never repeat"):
        await workflows(editor).apply(preview["run_id"], preview["review_sha256"])
    assert editor.calls.count("apply") == 1


async def test_reconnected_preview_can_be_applied_once_with_fresh_layout_checks(editor):
    preview = await workflows(editor).preview({"kind": "grid", "rows": 1, "columns": 1})
    resumed = workflows(editor)  # New tracker has no process-local preview expectations.
    result = await resumed.apply(preview["run_id"], preview["review_sha256"])
    assert result["status"] == "verified"
    assert result["verification"]["recipe_readback"]["status"] == "passed"
    assert resumed.list()["runs"][0]["run_id"] == preview["run_id"]


@pytest.mark.parametrize(
    "failure", [JevError("editor_unavailable", "timeout"), asyncio.CancelledError()]
)
async def test_lost_apply_response_is_durable_and_reconciliation_never_replays(editor, failure):
    runs = workflows(editor)
    preview = await runs.preview(RECIPE)
    editor.interruption = failure
    with pytest.raises(type(failure)):
        await runs.apply(preview["run_id"], preview["review_sha256"])
    resumed = workflows(editor)
    row = await resumed.status(preview["run_id"], reconcile=True)
    assert row["status"] == "outcome_uncertain"
    assert row["reconciliation"]["fresh_verification"]["status"] == "passed"
    assert row["replay_allowed"] is False
    with pytest.raises(JevError):
        await resumed.apply(preview["run_id"], preview["review_sha256"])
    assert editor.calls.count("apply") == 1


async def test_cancelled_preview_cannot_resume_or_apply_after_reconnect(editor):
    runs = workflows(editor)
    preview = await runs.preview(RECIPE)
    assert runs.cancel(preview["run_id"])["status"] == "cancelled"
    with pytest.raises(JevError):
        await workflows(editor).apply(preview["run_id"], preview["review_sha256"])
    assert "apply" not in editor.calls


@pytest.mark.parametrize("change", ["digest", "revision", "session", "expiry"])
async def test_wrong_review_and_stale_state_refuse_before_apply(editor, change):
    runs = workflows(editor)
    preview = await runs.preview(RECIPE)
    review = preview["review_sha256"]
    if change == "digest":
        review = "0" * 64
    elif change == "expiry":
        editor.store.clock = lambda: preview["expires_at"] + 1
    else:
        editor.state["session_id" if change == "session" else "revision"] = "changed"
    with pytest.raises(JevError):
        await runs.apply(preview["run_id"], review)
    assert "apply" not in editor.calls


async def test_post_apply_external_edit_fails_fresh_requirements(editor):
    runs = workflows(editor)
    preview = await runs.preview(RECIPE)

    def hook(action):
        if action == "actor_details" and editor.applied:
            editor.actors[0]["instance_id"] = "replacement"

    editor.hook = hook
    result = await runs.apply(preview["run_id"], preview["review_sha256"])
    assert result["status"] == "applied_unverified"
    assert result["verification"]["status"] == "unverifiable"


async def test_atomic_claim_prevents_two_reconnected_clients_dispatching(editor):
    first, second = workflows(editor), workflows(editor)
    preview = await first.preview(RECIPE)
    outcomes = await asyncio.gather(
        first.apply(preview["run_id"], preview["review_sha256"]),
        second.apply(preview["run_id"], preview["review_sha256"]),
        return_exceptions=True,
    )
    assert sum(isinstance(value, JevError) for value in outcomes) == 1
    assert editor.calls.count("apply") == 1


async def test_independent_sqlite_connections_cannot_both_claim_the_same_review(editor):
    preview = await workflows(editor).preview(RECIPE)
    ready = threading.Barrier(2)

    def claim():
        # Separate stores/owners/connections share only the on-disk project DB.
        store = RuntimeStore(editor.config)
        bridge = SimpleNamespace(infrastructure=SimpleNamespace(require=lambda: store))
        runs = WorkflowRuns(bridge, None)
        row = runs._get(preview["run_id"])
        ready.wait(timeout=5)  # Both readers have the old awaiting_review state.
        row.update(status="outcome_uncertain", apply_attempted=True)
        try:
            runs._save(row, expected_status="awaiting_review")
            return "claimed"
        except JevError as exc:
            return exc.code

    outcomes = await asyncio.gather(asyncio.to_thread(claim), asyncio.to_thread(claim))
    assert sorted(outcomes) == ["claimed", "workflow_consumed"]
    assert (await workflows(editor).status(preview["run_id"]))["apply_attempted"] is True
    assert "apply" not in editor.calls


async def test_reconcile_rejects_other_native_plan_and_new_session(editor):
    runs = workflows(editor)
    preview = await runs.preview(RECIPE)
    editor.state["session_id"] = "other"
    result = await runs.status(preview["run_id"], reconcile=True)
    assert result["reconciliation"] == "different_editor_session_or_world"
    assert "plan_status" not in editor.calls


async def test_workflow_and_checkpoint_tools_return_structured_errors_over_real_mcp():
    from mcp.shared.memory import create_connected_server_and_client_session

    from jev_unreal.config import Settings
    from jev_unreal.server import create_server

    server = create_server(Settings())  # Default configuration has no runtime/editor access.
    calls = {
        "unreal_workflow_run_preview": {"recipe": {"kind": "grid", "rows": 1, "columns": 1}},
        "unreal_workflow_run_apply": {"run_id": "a" * 32, "review_sha256": "b" * 64},
        "unreal_workflow_run": {},
        "unreal_workflow_run_cancel": {"run_id": "a" * 32},
        "unreal_project_vcs_status": {"files": ["Config.ini"]},
        "unreal_project_checkpoint": {"files": ["Config.ini"]},
        "unreal_project_checkpoint_compare": {"checkpoint_id": "a" * 32},
    }
    async with create_connected_server_and_client_session(server) as client:
        catalog = {tool.name: tool for tool in (await client.list_tools()).tools}
        for name, arguments in calls.items():
            assert catalog[name].outputSchema["type"] == "object", name
            response = await client.call_tool(name, arguments)
            # A handled domain failure is still a structured tool result, so clients
            # can inspect its code without scraping fallback JSON text.
            assert not response.isError, name
            assert response.structuredContent["ok"] is False, name
            assert isinstance(response.structuredContent["error"]["code"], str), name
            assert json.loads(response.content[0].text) == response.structuredContent, name
