"""Local fixtures only: no engine, provider or user project is launched or modified."""

import asyncio
import json
import os
import subprocess
import sys
from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP

from jev_unreal.bridge import UnrealBridge
from jev_unreal.config import Settings
from jev_unreal.errors import JevError
from jev_unreal.infrastructure import ProjectInfrastructure, register_infrastructure_tools
from jev_unreal.runtime_state import RuntimeStore
from jev_unreal.team_policy import MUTATING_ACTIONS, TeamPolicy, read_runtime_config


@pytest.fixture
def runtime_config(tmp_path):
    project = tmp_path / "Project" / "Fixture.uproject"
    project.parent.mkdir()
    project.write_text("{}")
    filename = tmp_path / "runtime.json"
    data = {
        "version": 1,
        "project_file": str(project),
        "state_directory": str(tmp_path / "private-state"),
        "allowed_actions": sorted(MUTATING_ACTIONS),
        "asset_roots": ["/Game/Approved", "/Engine/BasicShapes"],
        "allowed_operations": ["set_transform", "spawn_primitive", "light"],
        "blueprint_targets": ["door"],
        "functional_tests": ["door"],
        "validation_rules": ["mesh"],
    }
    filename.write_text(json.dumps(data))
    return filename, data


def make_runtime(runtime_config):
    filename, data = runtime_config
    return ProjectInfrastructure(
        Settings(expected_project=data["project_file"], runtime_config_file=str(filename))
    )


STATE = {"session_id": "session-one", "world_path": "/Game/Approved/Map.Map", "revision": "r1"}


@pytest.mark.parametrize(
    "change",
    [
        {"shell": "powershell"},
        {"allowed_actions": ["execute"]},
        {"asset_roots": ["/Game/Approved/../Secret"]},
        {"allowed_operations": ["arbitrary --command"]},
    ],
)
def test_runtime_rejects_unknown_controls_and_unsafe_policy(runtime_config, change):
    filename, data = runtime_config
    filename.write_text(json.dumps(data | change))
    with pytest.raises(JevError, match="configuration"):
        read_runtime_config(str(filename), data["project_file"])


def test_duplicate_keys_identity_and_project_local_storage_are_rejected(runtime_config):
    filename, data = runtime_config
    with pytest.raises(JevError):
        read_runtime_config(str(filename), data["project_file"] + "other")
    filename.write_text(json.dumps(data | {"state_directory": str(filename.parent / "Project")}))
    with pytest.raises(JevError):
        read_runtime_config(str(filename), data["project_file"])
    filename.write_text('{"version":1,"version":1}')
    with pytest.raises(JevError):
        read_runtime_config(str(filename), data["project_file"])


def test_runtime_storage_links_are_rejected(runtime_config):
    filename, data = runtime_config
    target = filename.parent / "real-state"
    target.mkdir()
    link = filename.parent / "linked-state"
    try:
        link.symlink_to(target, target_is_directory=True)
    except OSError:
        pytest.skip("Host does not permit symlink fixtures.")
    filename.write_text(json.dumps(data | {"state_directory": str(link)}))
    with pytest.raises(JevError):
        read_runtime_config(str(filename), data["project_file"])


@pytest.mark.parametrize(
    "action,params,status",
    [
        ("preview", {"operations": [{"op": "replace_mesh"}]}, STATE),
        (
            "preview",
            {"operations": [{"op": "set_transform", "actor_path": "/Game/Secret/A.A"}]},
            STATE,
        ),
        ("frame", {"actor_paths": ["/Game/ApprovedSibling/A.A"]}, STATE),
        ("frame", {}, STATE | {"world_path": "/Game/Secret/Map.Map"}),
        ("blueprint_graph_preview", {"target_id": "unapproved"}, STATE),
        ("validation_start", {"rule_ids": ["unapproved"]}, STATE),
        ("functional_start", {"test_id": "unapproved"}, STATE),
    ],
)
async def test_policy_denial_happens_before_mutation_dispatch(
    runtime_config, action, params, status
):
    runtime = make_runtime(runtime_config)
    dispatch = AsyncMock()
    with pytest.raises(JevError) as error:
        await runtime.execute(action, params, status, dispatch)
    assert error.value.code == "policy_denied"
    dispatch.assert_not_called()
    assert runtime.store.list_receipts() == []


async def test_successful_preview_reconnect_apply_consumed_once(runtime_config):
    first = make_runtime(runtime_config)
    await first.execute(
        "preview",
        {"operations": [{"op": "set_transform"}]},
        STATE,
        AsyncMock(return_value={"plan_id": "plan-one"}),
    )
    second = make_runtime(runtime_config)
    dispatch = AsyncMock(return_value={"applied": True, "raw_private_text": "not-stored"})
    result = await second.execute("apply", {"plan_id": "plan-one"}, STATE, dispatch)
    assert result["applied"] is True
    with pytest.raises(JevError) as error:
        await second.execute("apply", {"plan_id": "plan-one"}, STATE, dispatch)
    assert error.value.code == "policy_plan_required"
    assert dispatch.await_count == 1
    persisted = json.dumps(second.store.list_receipts())
    assert "not-stored" not in persisted
    assert "response_observed" in persisted


async def test_uncertain_apply_is_persisted_and_never_replayed(runtime_config):
    runtime = make_runtime(runtime_config)
    await runtime.execute("preview", {}, STATE, AsyncMock(return_value={"plan_id": "p"}))
    dispatch = AsyncMock(side_effect=JevError("editor_unavailable", "private transport details"))
    with pytest.raises(JevError):
        await runtime.execute("apply", {"plan_id": "p"}, STATE, dispatch)
    recovered = make_runtime(runtime_config)
    receipts = recovered.store.list_receipts()
    assert receipts[0]["status"] == "outcome_uncertain"
    assert receipts[0]["identity"] == STATE
    assert "private transport details" not in json.dumps(receipts)
    with pytest.raises(JevError):
        await recovered.execute("apply", {"plan_id": "p"}, STATE, dispatch)
    assert dispatch.await_count == 1


async def test_changed_session_or_policy_invalidates_old_authorization(runtime_config):
    runtime = make_runtime(runtime_config)
    await runtime.execute("preview", {}, STATE, AsyncMock(return_value={"plan_id": "p"}))
    dispatch = AsyncMock()
    with pytest.raises(JevError):
        await runtime.execute("apply", {"plan_id": "p"}, STATE | {"session_id": "new"}, dispatch)
    filename, data = runtime_config
    filename.write_text(json.dumps(data | {"allowed_operations": ["set_transform"]}))
    with pytest.raises(JevError):
        await make_runtime(runtime_config).execute("apply", {"plan_id": "p"}, STATE, dispatch)
    dispatch.assert_not_called()


async def test_unapproved_resolved_blueprint_path_never_authorizes_compile(runtime_config):
    runtime = make_runtime(runtime_config)
    with pytest.raises(JevError):
        await runtime.execute(
            "blueprint_compile_preview",
            {"target_id": "door"},
            STATE,
            AsyncMock(return_value={"plan_id": "p", "asset_path": "/Game/X.X"}),
        )
    with pytest.raises(JevError) as error:
        await runtime.execute("blueprint_compile", {"plan_id": "p"}, STATE, AsyncMock())
    assert error.value.code == "policy_plan_required"


@pytest.mark.parametrize(
    "reference",
    [
        {"operations": [{"op": "set_transform", "materials": [{"path": "/Game/Secret/M.M"}]}]},
        {
            "operations": [
                {
                    "op": "set_transform",
                    "mesh_review": {"result_mesh": {"path": "/Game/Secret/Mesh.Mesh"}},
                }
            ]
        },
    ],
)
async def test_normalized_implicit_references_block_apply_authorization(runtime_config, reference):
    runtime = make_runtime(runtime_config)
    with pytest.raises(JevError) as error:
        await runtime.execute(
            "preview", {}, STATE, AsyncMock(return_value={"plan_id": "p", **reference})
        )
    assert error.value.code == "policy_denied"
    with pytest.raises(JevError):
        await runtime.execute("apply", {"plan_id": "p"}, STATE, AsyncMock())


def test_graph_operation_and_unsaved_world_need_explicit_policy(runtime_config):
    runtime = make_runtime(runtime_config)
    params = {"target_id": "door", "graph_edit": {"operation": "add_math_node"}}
    with pytest.raises(JevError):
        runtime.policy.check("blueprint_graph_preview", params, STATE)
    expanded = runtime.config.model_copy(
        update={
            "allowed_operations": ["add_math_node"],
            "asset_roots": ["/Game/Approved", "/Temp"],
        }
    )
    policy = TeamPolicy(expanded)
    policy.check(
        "blueprint_graph_preview", params, STATE | {"world_path": "/Temp/Untitled_1.Untitled_1"}
    )
    policy.check_data({"asset_class": "/Script/Engine.Blueprint"}, result=True)
    with pytest.raises(JevError):
        policy.check_data({"asset_path": "/Script/Engine.Blueprint"}, result=True)


def test_inspection_parameter_kind_does_not_become_an_edit_operation(runtime_config):
    policy = make_runtime(runtime_config).policy
    policy.check_data({"before": {"parameters": [{"kind": "scalar", "name": "Tint"}]}}, result=True)
    with pytest.raises(JevError):
        policy.check_data({"requested": {"kind": "material_scalar"}}, result=True)


async def test_cancellation_records_uncertainty_and_releases_automatic_lease(runtime_config):
    runtime = make_runtime(runtime_config)
    entered = asyncio.Event()

    async def wait_forever(*args):
        entered.set()
        await asyncio.Event().wait()

    task = asyncio.create_task(runtime.execute("frame", {}, STATE, wait_forever))
    await entered.wait()
    with pytest.raises(JevError) as error:
        runtime.release_lease(runtime.store.lease_status()["lease_id"])
    assert error.value.code == "lease_busy"
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert runtime.store.lease_status() == {"held": False}
    assert runtime.store.list_receipts()[0]["status"] == "outcome_uncertain"


def test_atomic_cross_client_lease_conflict_expiry_and_owner_release(runtime_config):
    config = make_runtime(runtime_config).config
    now = [100.0]
    first, second = (RuntimeStore(config, clock=lambda: now[0]) for _ in range(2))
    lease, _ = first.acquire(2)
    with pytest.raises(JevError) as error:
        second.acquire()
    assert error.value.code == "project_leased"
    assert second.release(lease) is False
    now[0] += 3
    replacement, _ = second.acquire()
    assert replacement != lease
    assert first.release(lease) is False
    with pytest.raises(JevError):
        first.renew(lease)
    assert second.release(replacement)


def test_lease_excludes_a_separate_python_process(runtime_config):
    runtime = make_runtime(runtime_config)
    lease, _ = runtime.store.acquire()
    filename, data = runtime_config
    # Controlled test-only subprocess; this is not exposed through any product config or API.
    script = (
        "import sys\nfrom jev_unreal.team_policy import read_runtime_config\n"
        "from jev_unreal.runtime_state import RuntimeStore\n"
        "from jev_unreal.errors import JevError\n"
        "try:\n RuntimeStore(read_runtime_config(sys.argv[1],sys.argv[2])).acquire()\n"
        "except JevError as exc:\n print(exc.code)\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", script, str(filename), data["project_file"]],
        capture_output=True,
        text=True,
        timeout=10,
        check=True,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    assert result.stdout.strip() == "project_leased"
    runtime.store.release(lease)


def test_unfinished_receipt_survives_restart_with_no_success_inference(runtime_config):
    runtime = make_runtime(runtime_config)
    receipt = runtime.store.begin("apply", {"secret": "not-stored"}, STATE, runtime.policy.hash)
    reopened = make_runtime(runtime_config).store
    assert reopened.get(receipt)["status"] == "dispatched_uncertain"
    assert b"not-stored" not in reopened.path.read_bytes()
    assert reopened.forget(receipt)
    with pytest.raises(JevError):
        reopened.get(receipt)


def test_receipt_commit_survives_abrupt_writer_process_exit(runtime_config):
    filename, data = runtime_config
    script = (
        "import os,sys\nfrom jev_unreal.team_policy import read_runtime_config\n"
        "from jev_unreal.runtime_state import RuntimeStore\n"
        "store=RuntimeStore(read_runtime_config(sys.argv[1],sys.argv[2]))\n"
        "store.begin('apply', {'plan_id':'crash-fixture'}, "
        "{'session_id':'crash-session','world_path':'/Game/Approved/Map.Map',"
        "'revision':'r1'}, 'policy')\n"
        "os._exit(19)\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", script, str(filename), data["project_file"]],
        capture_output=True,
        timeout=10,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    assert result.returncode == 19
    records = make_runtime(runtime_config).store.list_receipts()
    assert records[0]["status"] == "dispatched_uncertain"
    assert records[0]["plan_id"] == "crash-fixture"


def test_retention_and_consumed_plan_expiry_are_bounded(runtime_config):
    config = make_runtime(runtime_config).config.model_copy(update={"max_receipts": 16})
    now = [100.0]
    store = RuntimeStore(config, clock=lambda: now[0])
    for _ in range(20):
        store.begin("frame", {}, STATE, "policy")
        now[0] += 1
    assert len(store.list_receipts(100)) == 16
    store.authorize_plan("s", "p", "apply", "policy")
    now[0] += 121
    with pytest.raises(JevError):
        store.consume_plan("s", "p", "apply", "policy")
    now[0] += 31 * 86400
    assert store.list_receipts() == []


async def test_bridge_enforces_configured_policy_at_actual_mutation_boundary(runtime_config):
    filename, data = runtime_config
    bridge = UnrealBridge(
        Settings(expected_project=data["project_file"], runtime_config_file=str(filename))
    )
    bridge._call = AsyncMock(return_value=STATE | {"project_file": data["project_file"]})
    try:
        with pytest.raises(JevError) as error:
            await bridge.call("frame", {"actor_paths": ["/Game/Secret/A.A"]})
        assert error.value.code == "policy_denied"
        bridge._call.assert_awaited_once_with("status", {})
    finally:
        await bridge.close()


async def test_registered_job_schema_has_no_executable_or_argument_input():
    bridge = UnrealBridge(Settings())
    server = FastMCP("infrastructure-test")
    register_infrastructure_tools(server, bridge)
    try:
        tools = {tool.name: tool for tool in await server.list_tools()}
        assert len(tools) == 10
        assert all(tool.outputSchema is not None for tool in tools.values())
        assert set(tools["unreal_named_job_start"].inputSchema["properties"]) == {"plan_id"}
        assert set(tools["unreal_named_job_preview"].inputSchema["properties"]) == {"name"}
        assert tools["unreal_durable_receipts"].annotations.readOnlyHint is True
        assert tools["unreal_named_job_start"].annotations.destructiveHint is True
    finally:
        await bridge.close()


def test_no_runtime_config_keeps_persistence_opt_in():
    runtime = ProjectInfrastructure(Settings())
    assert runtime.status()["configured"] is False
    with pytest.raises(JevError) as error:
        runtime.require()
    assert error.value.code == "runtime_configuration_required"


def test_policy_is_independent_of_model_confidence(runtime_config):
    config = make_runtime(runtime_config).config.model_copy(update={"allowed_actions": []})
    with pytest.raises(JevError) as error:
        TeamPolicy(config).check("frame", {"confidence": 1.0}, STATE)
    assert error.value.code == "policy_denied"
