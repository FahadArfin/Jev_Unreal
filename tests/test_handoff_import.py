"""Retained-source imports bind configuration, source bytes and native measurements."""

import asyncio
import hashlib
import json
import threading
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from jev_unreal.errors import JevError
from jev_unreal.handoff_import import HandoffImporter
from jev_unreal.workflows import ExpectedState

STATE = {"session_id": "session", "world_path": "/Game/Map.Map", "revision": "before"}


@pytest.fixture
def configured(tmp_path):
    files = []
    for name, role in [("prop.blend", "editable_source"), ("prop.fbx", "mesh")]:
        content = name.encode()
        (tmp_path / name).write_bytes(content)
        files.append(
            {
                "path": name,
                "role": role,
                "bytes": len(content),
                "sha256": hashlib.sha256(content).hexdigest(),
            }
        )
    asset = {
        "asset_id": "prop",
        "unreal_asset_path": "/Game/Props/Prop.Prop",
        "mesh_file": "prop.fbx",
        "bounds_size_cm": [100.0, 100.0, 100.0],
        "bounds_center_cm": [0.0, 0.0, 0.0],
        "material_slots": ["Surface"],
    }
    manifest = {
        "version": 1,
        "coordinate_contract": "unreal_local_centimeters_z_up",
        "producer": "source fixture",
        "source_license": "MIT",
        "provenance": "Original source fixture",
        "files": files,
        "assets": [asset],
    }
    (tmp_path / "handoff.json").write_text(json.dumps(manifest))
    project = str(tmp_path / "Test.uproject")
    config = {
        "version": 1,
        "project_file": project,
        "bundles": [{"id": "prop", "directory": str(tmp_path), "asset_id": "prop"}],
    }
    config_path = tmp_path / "config.json"
    config_path.write_text(json.dumps(config))
    row = {
        "alias": "prop",
        "asset_path": asset["unreal_asset_path"],
        "source_sha256": files[1]["sha256"],
    }
    observation = {
        **STATE,
        "revision": "after",
        "project_file": project,
        "asset_path": asset["unreal_asset_path"],
        "bounds_size_cm": asset["bounds_size_cm"].copy(),
        "bounds_center_cm": asset["bounds_center_cm"].copy(),
        "material_slot_names": ["Surface"],
    }

    async def call(action, params=None):
        if action == "handoff_manifest":
            return {"enabled": True, "aliases": [row]}
        if action == "handoff_preview":
            return {
                **row,
                "operation": params["operation"],
                "plan_id": "reviewed",
                **STATE,
                "project_file": project,
                "expires_in_seconds": 120,
            }
        if action == "handoff_apply":
            return {
                **row,
                "status": "imported",
                "saved": False,
                "plan_id": "reviewed",
                "operation": "import",
                **STATE,
                "project_file": project,
            }
        if action == "workflow_inspect":
            return observation
        raise AssertionError(action)

    bridge = SimpleNamespace(
        settings=SimpleNamespace(expected_project=project), call=AsyncMock(side_effect=call)
    )
    importer = HandoffImporter(bridge, str(config_path))
    return SimpleNamespace(
        root=tmp_path,
        bridge=bridge,
        importer=importer,
        row=row,
        observation=observation,
        config=config,
        manifest=manifest,
    )


async def test_preview_apply_verifies_fresh_contract_and_consumes_plan(configured):
    c = configured
    preview = await c.importer.preview("prop", "import", ExpectedState(**STATE))
    assert preview["retained_source_verified"]
    assert preview["files_verified"] == 2
    result = await c.importer.apply(preview["plan_id"])
    assert result["status"] == "verified"
    c.bridge.call.assert_awaited_with(
        "workflow_inspect",
        {
            "kind": "asset_diagnosis",
            "target_path": "/Game/Props/Prop.Prop",
            "dependency_depth": 1,
        },
    )
    with pytest.raises(JevError, match="already consumed"):
        await c.importer.apply(preview["plan_id"])


@pytest.mark.parametrize("changed", ["source", "manifest", "configuration"])
async def test_modified_review_inputs_never_dispatch_import(configured, changed):
    c = configured
    await c.importer.preview("prop", "reimport", ExpectedState(**STATE))
    c.bridge.call.reset_mock()
    if changed == "source":
        (c.root / "prop.blend").write_bytes(b"x" * len(b"prop.blend"))
    elif changed == "manifest":
        c.manifest["assets"][0]["bounds_center_cm"][2] = 20
        (c.root / "handoff.json").write_text(json.dumps(c.manifest))
    else:
        c.config["bundles"][0]["asset_id"] = "missing"
        (c.root / "config.json").write_text(json.dumps(c.config))
    with pytest.raises(JevError):
        await c.importer.apply("reviewed")
    c.bridge.call.assert_not_awaited()
    assert "reviewed" not in c.importer.plans


@pytest.mark.parametrize("changed", ["hash", "path", "project"])
async def test_mismatched_native_approval_or_project_cannot_preview(configured, changed):
    c = configured
    if changed == "hash":
        c.row["source_sha256"] = "0" * 64
    elif changed == "path":
        c.row["asset_path"] = "/Game/Other.Other"
    else:
        c.bridge.settings.expected_project = str(c.root / "Other.uproject")
    with pytest.raises(JevError):
        await c.importer.preview("prop", "import", ExpectedState(**STATE))
    assert all(call.args[0] != "handoff_preview" for call in c.bridge.call.await_args_list)


@pytest.mark.parametrize("failure", ["dimensions", "slots", "session", "transport"])
async def test_successful_native_import_is_not_false_acceptance(configured, failure):
    c = configured
    await c.importer.preview("prop", "import", ExpectedState(**STATE))
    if failure == "dimensions":
        c.observation["bounds_size_cm"][0] = 1.0
    elif failure == "slots":
        c.observation["material_slot_names"] = ["Wrong"]
    elif failure == "session":
        c.observation["session_id"] = "replaced"
    else:
        original = c.bridge.call.side_effect

        async def fail_observe(action, params=None):
            if action == "workflow_inspect":
                raise JevError("bridge_timeout", "Unknown observation.")
            return await original(action, params)

        c.bridge.call.side_effect = fail_observe
    result = await c.importer.apply("reviewed")
    assert result["status"] == "needs_review"
    assert "reviewed" not in c.importer.plans


async def test_expiry_and_uncertain_import_never_retry(configured):
    c = configured
    now = [0.0]
    c.importer.clock = lambda: now[0]
    await c.importer.preview("prop", "import", ExpectedState(**STATE))
    now[0] = 120.0
    c.bridge.call.reset_mock()
    with pytest.raises(JevError, match="expired"):
        await c.importer.apply("reviewed")
    c.bridge.call.assert_not_awaited()
    await c.importer.preview("prop", "import", ExpectedState(**STATE))
    c.bridge.call.reset_mock()
    c.bridge.call.side_effect = JevError("bridge_timeout", "Import may have happened.")
    with pytest.raises(JevError, match="may have happened"):
        await c.importer.apply("reviewed")
    c.bridge.call.assert_awaited_once_with("handoff_apply", {"plan_id": "reviewed"})


async def test_bundle_hashing_runs_off_event_loop_and_cancellation_never_imports(configured):
    c = configured
    entered, release = threading.Event(), threading.Event()
    original = c.importer._bundle
    thread_ids = []

    def bounded_hash(config, alias):
        thread_ids.append(threading.get_ident())
        entered.set()
        assert release.wait(2)
        return original(config, alias)

    c.importer._bundle = bounded_hash
    task = asyncio.create_task(c.importer.preview("prop", "import", ExpectedState(**STATE)))
    try:
        assert await asyncio.to_thread(entered.wait, 2)
        assert thread_ids != [threading.get_ident()]
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
    finally:
        release.set()
    assert all(call.args[0] != "handoff_preview" for call in c.bridge.call.await_args_list)


async def test_duplicate_manifest_keys_refused_before_native_calls(configured):
    c = configured
    encoded = json.dumps(c.manifest)
    (c.root / "handoff.json").write_text(encoded[:-1] + ', "version": 1}')
    with pytest.raises(JevError):
        await c.importer.preview("prop", "import", ExpectedState(**STATE))
    c.bridge.call.assert_not_awaited()


@pytest.mark.parametrize(
    "stage,field",
    [
        ("handoff_preview", "session_id"),
        ("handoff_preview", "revision"),
        ("handoff_preview", "project_file"),
        ("handoff_apply", "plan_id"),
        ("handoff_apply", "operation"),
        ("handoff_apply", "world_path"),
    ],
)
async def test_returned_receipt_identity_cannot_substitute_another_plan(configured, stage, field):
    c = configured
    original = c.bridge.call.side_effect

    async def substitute(action, params=None):
        result = await original(action, params)
        if action == stage:
            result[field] = "wrong"
        return result

    c.bridge.call.side_effect = substitute
    if stage == "handoff_preview":
        with pytest.raises(JevError, match="Native preview"):
            await c.importer.preview("prop", "import", ExpectedState(**STATE))
        assert not c.importer.plans
    else:
        await c.importer.preview("prop", "import", ExpectedState(**STATE))
        result = await c.importer.apply("reviewed")
        assert result["status"] == "needs_review"
        assert all(call.args[0] != "workflow_inspect" for call in c.bridge.call.await_args_list)
