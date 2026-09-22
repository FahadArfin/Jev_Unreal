"""Public CLI diagnostics must work without provider requests or secret disclosure."""

from argparse import Namespace
from unittest.mock import AsyncMock

import pytest

from jev_unreal.cli import (
    PROJECT_WORKFLOW_CAPABILITIES,
    PROJECT_WORKFLOW_FEATURES,
    WORKFLOW_CAPABILITIES,
    read_checks,
    run_command,
)
from jev_unreal.config import Settings
from jev_unreal.errors import JevError


@pytest.mark.parametrize(
    "connected,bound,ready",
    [
        (False, False, False),
        (False, True, False),
        (True, False, False),
        (True, True, True),
    ],
)
async def test_doctor_readiness_and_no_provider_calls(monkeypatch, connected, bound, ready):
    bridge = AsyncMock()
    if connected:
        bridge.call.return_value = {
            "project_file": "test.uproject",
            "capabilities": list(WORKFLOW_CAPABILITIES | PROJECT_WORKFLOW_CAPABILITIES),
        }
    else:
        bridge.call.side_effect = JevError("editor_unavailable", "Editor unavailable")
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)

    def no_provider(*args, **kwargs):
        pytest.fail("Doctor must not create a provider client")

    monkeypatch.setattr("jev_unreal.cli.DecisionClient", no_provider)
    result = await run_command(
        Namespace(command="doctor"),
        Settings(
            expected_project="test.uproject" if bound else "",
            api_key="synthetic-private-key",
        ),
    )
    assert result["ready"] is ready
    assert result["provider"]["tested"] is False
    assert "synthetic-private-key" not in str(result)
    bridge.close.assert_awaited_once()


async def test_cli_layouts_are_offline_and_catalog_empty_is_explicit():
    result = await run_command(Namespace(command="layouts"), Settings())
    assert set(result) == {"grid", "stairs", "room"}
    result = await run_command(Namespace(command="catalog", catalog_command="status"), Settings())
    assert result["tool_count"] == 0


async def test_doctor_reports_connected_but_outdated_native_plugin(monkeypatch):
    bridge = AsyncMock()
    bridge.call.return_value = {
        "project_file": "test.uproject",
        "capabilities": ["status", "preview"],
    }
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)
    result = await run_command(
        Namespace(command="doctor"), Settings(expected_project="test.uproject")
    )
    assert result["editor"]["ready"] is True
    assert result["ready"] is False
    assert set(result["workflow_compatibility"]["missing_capabilities"]) == WORKFLOW_CAPABILITIES
    assert "Rebuild" in result["next_steps"][0]


async def test_doctor_old_03_bridge_retains_core_compatibility_but_requires_05_upgrade(monkeypatch):
    bridge = AsyncMock()
    bridge.call.return_value = {
        "project_file": "test.uproject",
        "bridge_version": "0.3.0",
        "capabilities": list(WORKFLOW_CAPABILITIES),
    }
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)
    result = await run_command(
        Namespace(command="doctor"), Settings(expected_project="test.uproject")
    )
    assert result["ready"] is False
    assert result["editor"]["ready"] is True
    assert result["workflow_compatibility"] == {"ready": True, "missing_capabilities": []}
    project = result["project_workflow_compatibility"]
    assert project["ready"] is False
    assert set(project["missing_capabilities"]) == PROJECT_WORKFLOW_CAPABILITIES
    assert set(project["features"]) == set(PROJECT_WORKFLOW_FEATURES)
    assert all(not feature["ready"] for feature in project["features"].values())
    assert any("0.7" in step and "Native plan review" in step for step in result["next_steps"])
    bridge.call.assert_awaited_once_with("status")


@pytest.mark.parametrize("missing", sorted(PROJECT_WORKFLOW_CAPABILITIES))
async def test_doctor_identifies_each_missing_project_capability(monkeypatch, missing):
    bridge = AsyncMock()
    bridge.call.return_value = {
        "project_file": "test.uproject",
        "bridge_version": "0.5.0",
        "capabilities": sorted((WORKFLOW_CAPABILITIES | PROJECT_WORKFLOW_CAPABILITIES) - {missing}),
    }
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)
    result = await run_command(
        Namespace(command="doctor"), Settings(expected_project="test.uproject")
    )
    assert result["ready"] is False
    assert result["workflow_compatibility"]["ready"] is True
    project = result["project_workflow_compatibility"]
    assert project["missing_capabilities"] == [missing]
    failed_features = [feature for feature in project["features"].values() if not feature["ready"]]
    assert len(failed_features) == 1
    assert failed_features[0]["missing_capabilities"] == [missing]
    assert failed_features[0]["label"] in result["next_steps"][0]


async def test_doctor_current_05_bridge_is_ready_without_running_or_enabling_project_code(
    monkeypatch,
):
    bridge = AsyncMock()
    bridge.call.return_value = {
        "project_file": "test.uproject",
        "bridge_version": "0.5.0",
        "capabilities": sorted(WORKFLOW_CAPABILITIES | PROJECT_WORKFLOW_CAPABILITIES),
    }
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)

    def no_provider(*args, **kwargs):
        pytest.fail("Doctor compatibility checks must remain local")

    monkeypatch.setattr("jev_unreal.cli.DecisionClient", no_provider)
    result = await run_command(
        Namespace(command="doctor"), Settings(expected_project="test.uproject")
    )
    assert result["ready"] is True
    assert result["project_workflow_compatibility"]["ready"] is True
    assert result["project_workflow_compatibility"]["missing_capabilities"] == []
    assert result["project_workflow_compatibility"]["policy_verified"] is False
    assert result["next_steps"] == []
    bridge.call.assert_awaited_once_with("status")
    bridge.close.assert_awaited_once()


async def test_doctor_old_04_bridge_reports_only_new_mesh_features_missing(monkeypatch):
    bridge = AsyncMock()
    bridge.call.return_value = {
        "project_file": "test.uproject",
        "bridge_version": "0.4.0",
        "capabilities": sorted(
            (WORKFLOW_CAPABILITIES | PROJECT_WORKFLOW_CAPABILITIES)
            - {"replace_mesh", "duplicate_mesh"}
        ),
    }
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)
    result = await run_command(
        Namespace(command="doctor"), Settings(expected_project="test.uproject")
    )
    assert result["ready"] is False
    assert result["workflow_compatibility"]["ready"] is True
    report = result["project_workflow_compatibility"]
    assert report["missing_capabilities"] == ["duplicate_mesh", "replace_mesh"]
    assert not report["features"]["mesh_editing"]["ready"]
    assert all(
        value["ready"] for name, value in report["features"].items() if name != "mesh_editing"
    )
    assert "0.7" in result["next_steps"][0]


def test_cli_verification_file_is_bounded_and_has_no_arbitrary_options(tmp_path):
    path = tmp_path / "checks.json"
    path.write_text('{"checks":[],"command":"anything"}')
    with pytest.raises(JevError, match="Use checks"):
        read_checks(path)
    path.write_bytes(b" " * 65537)
    with pytest.raises(JevError, match="64 KiB"):
        read_checks(path)
    path.write_text('{"checks":[]}')
    assert read_checks(path) == {"checks": []}  # Typed helper rejects empty checks.


async def test_cli_inspect_and_failed_verification_are_read_only(monkeypatch, tmp_path):
    bridge = AsyncMock()
    bridge.call.return_value = {"actors": []}
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)
    result = await run_command(Namespace(command="inspect", actor_paths=["/Temp/A.A"]), Settings())
    assert result == {"actors": []}
    bridge.call.assert_awaited_once_with("actor_details", {"actor_paths": ["/Temp/A.A"]})
    path = tmp_path / "checks.json"
    path.write_text('{"checks":[{"kind":"label","actor_path":"/Temp/A.A","expected":"A"}]}')
    result = await run_command(Namespace(command="verify", file=str(path)), Settings())
    assert result["status"] == "unverifiable"
    assert bridge.call.await_count == 2
