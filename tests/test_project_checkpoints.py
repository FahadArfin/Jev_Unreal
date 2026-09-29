"""Approved local-file tests and mocked VCS output; no remote Perforce or editor calls."""

import hashlib
import json
import sys
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP
from pydantic import ValidationError

from jev_unreal.errors import JevError
from jev_unreal.project_checkpoints import (
    CheckpointConfig,
    ProjectCheckpoints,
    fixed_status_command,
    register_checkpoint_tools,
)
from jev_unreal.runtime_state import RuntimeStore
from jev_unreal.team_policy import RuntimeConfig


@pytest.fixture
def setup(tmp_path):
    project = tmp_path / "project"
    project.mkdir()
    uproject = project / "Public.uproject"
    uproject.write_text("{}")
    target = project / "Config.ini"
    target.write_text("setting=old")
    executable = tmp_path / "pinned-tool"
    executable.write_bytes(b"synthetic executable identity")
    config = {
        "version": 1,
        "project_file": str(uproject),
        "approved_files": ["Config.ini", "deleted.txt", "-option.ini"],
        "git_executable": str(executable),
        "git_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
    }
    config_file = tmp_path / "checkpoints.json"
    config_file.write_text(json.dumps(config))
    store = RuntimeStore(
        RuntimeConfig(
            version=1, project_file=str(uproject), state_directory=str(tmp_path / "private")
        )
    )
    bridge = SimpleNamespace(
        settings=SimpleNamespace(expected_project=str(uproject)),
        infrastructure=SimpleNamespace(
            require=lambda: store, policy=SimpleNamespace(hash="a" * 64)
        ),
        call=AsyncMock(
            return_value={
                "project_file": str(uproject),
                "session_id": "s",
                "world_path": "w",
                "revision": "r",
                "dirty_package_count": 1,
            }
        ),
    )
    runner = AsyncMock(return_value=b" M Config.ini\0?? -option.ini\0")

    async def response(argv, root, env):
        return b"" if "--show-prefix" in argv else runner.return_value

    runner.side_effect = response
    return SimpleNamespace(
        project=project,
        target=target,
        executable=executable,
        config=config,
        config_file=config_file,
        store=store,
        bridge=bridge,
        runner=runner,
    )


def checkpoints(setup):
    return ProjectCheckpoints(setup.bridge, str(setup.config_file), runner=setup.runner)


async def test_manifest_is_durable_disk_only_and_comparison_detects_changes(setup):
    manager = checkpoints(setup)
    result = await manager.create(["Config.ini", "deleted.txt"])
    assert result["dirty_package_count"] == 1
    assert "unsaved" in result["unsaved_warning"]
    assert result["restoration_available"] is False
    assert result["files"][1] == {"path": "deleted.txt", "exists": False}
    assert "setting=old" not in json.dumps(result)
    assert checkpoints(setup).compare(result["receipt_id"])["matches_disk"] is True
    setup.target.write_text("setting=new")
    compared = checkpoints(setup).compare(result["receipt_id"])
    assert compared["changed_files"] == ["Config.ini"]
    assert setup.target.read_text() == "setting=new"


async def test_git_status_uses_fixed_argv_literal_files_and_scrubbed_environment(
    setup, monkeypatch
):
    monkeypatch.setenv("GIT_DIR", "attacker")
    monkeypatch.setenv("GIT_CONFIG_COUNT", "1")
    result = await checkpoints(setup).vcs_status(["Config.ini", "-option.ini"])
    assert result["git"]["status"] == "observed"
    assert result["git"]["clean_selected_files"] is False
    argv, root, env = setup.runner.call_args.args
    assert argv[-3:] == ["--", "Config.ini", "-option.ini"]
    assert "--literal-pathspecs" in argv and "core.fsmonitor=false" in argv
    assert root == setup.project
    assert "GIT_DIR" not in env and "GIT_CONFIG_COUNT" not in env
    assert env["GIT_OPTIONAL_LOCKS"] == "0"
    assert result["p4"]["status"] == "not_configured"
    setup.bridge.call.assert_not_called()


async def test_changed_executable_never_runs_and_errors_do_not_echo_stderr(setup):
    manager = checkpoints(setup)
    setup.executable.write_bytes(b"changed")
    assert (await manager.vcs_status(["Config.ini"]))["git"]["reason"] == "vcs_identity"
    setup.runner.assert_not_called()


async def test_directory_scope_cannot_turn_an_approved_file_into_a_recursive_vcs_scan(setup):
    setup.target.unlink()
    setup.target.mkdir()
    with pytest.raises(JevError, match="directories"):
        await checkpoints(setup).vcs_status(["Config.ini"])
    setup.runner.assert_not_called()


async def test_perforce_uses_exact_absolute_files_and_candid_unavailable_status(setup):
    setup.config["p4_executable"] = setup.config.pop("git_executable")
    setup.config["p4_sha256"] = setup.config.pop("git_sha256")
    setup.config["p4_port"] = "ssl:perforce.example.invalid:1666"
    setup.config_file.write_text(json.dumps(setup.config))
    setup.runner.return_value = b"... depotFile //depot/private\n... action edit\n"
    result = await checkpoints(setup).vcs_status(["Config.ini"])
    assert result["p4"]["opened_file_count"] == 1
    assert "private" not in json.dumps(result)
    assert setup.runner.call_args.args[0][-1] == str(setup.target)
    argv, _, env = setup.runner.call_args.args
    assert argv[argv.index("-p") + 1] == setup.config["p4_port"]
    assert "P4LOGINSSO=" in argv and "P4CONFIG=" in argv
    assert "P4PORT" not in env
    setup.runner.side_effect = JevError("vcs_timeout", "redacted")
    assert (await checkpoints(setup).vcs_status(["Config.ini"]))["p4"]["status"] == "unavailable"


async def test_git_rename_omits_unapproved_paths_without_claiming_cleanliness(setup):
    setup.runner.return_value = b"R  private-destination.ini\0Config.ini\0"
    result = (await checkpoints(setup).vcs_status(["Config.ini"]))["git"]
    assert result["entries"] == [{"path": "Config.ini", "status": "R "}]
    assert result["clean_selected_files"] is False
    assert result["out_of_scope_paths_omitted"] is True
    assert "private-destination" not in json.dumps(result)


async def test_git_status_maps_nested_project_paths_without_exposing_sibling_files(setup):
    setup.runner.side_effect = [
        b"games/Example/\n",
        b" M games/Example/Config.ini\0 M games/Private/Config.ini\0",
    ]
    result = (await checkpoints(setup).vcs_status(["Config.ini"]))["git"]
    assert result["entries"] == [{"path": "Config.ini", "status": " M"}]
    assert result["out_of_scope_paths_omitted"] and not result["clean_selected_files"]
    assert "Private" not in json.dumps(result)


@pytest.mark.parametrize(
    "name",
    [
        "../secret",
        "/absolute",
        "C:/file",
        "Saved/log.txt",
        ".git/config",
        "x@change",
        "x#rev",
        "x*",
        "a\\b",
        "a\nb",
        "folder/.../private.ini",
    ],
)
def test_config_refuses_path_and_revision_injection(name):
    with pytest.raises(ValidationError):
        CheckpointConfig(project_file="project", approved_files=[name])


@pytest.mark.parametrize("port", [None, "rsh:command", "jsh:command", "host:0", "host:65536"])
def test_perforce_requires_reviewed_tcp_endpoint_without_command_transports(port):
    with pytest.raises(ValidationError):
        CheckpointConfig(project_file="project", approved_files=["Config.ini"],
                         p4_executable="p4", p4_sha256="a" * 64, p4_port=port)


@pytest.mark.parametrize("output", [b" M Config.ini", b"\0private\0", b"xx Config.ini\0"])
async def test_malformed_git_output_cannot_claim_cleanliness(setup, output):
    setup.runner.return_value = output
    result = (await checkpoints(setup).vcs_status(["Config.ini"]))["git"]
    assert result["status"] == "unavailable" and result["reason"] == "vcs_response"


async def test_unapproved_and_linked_files_are_refused_before_reading(setup, tmp_path):
    manager = checkpoints(setup)
    with pytest.raises(JevError, match="exact files"):
        await manager.create(["secret.txt"])
    setup.target.unlink()
    secret = tmp_path / "secret"
    secret.write_text("private")
    try:
        setup.target.symlink_to(secret)
    except OSError:
        pytest.skip("Host does not permit test symlinks")
    with pytest.raises(JevError):
        await manager.create(["Config.ini"])


async def test_checkpoint_policy_change_and_wrong_editor_never_create_fresh_evidence(setup):
    receipt = await checkpoints(setup).create(["Config.ini"])
    setup.config["approved_files"] = ["Config.ini"]
    setup.config_file.write_text(json.dumps(setup.config))
    with pytest.raises(JevError, match="configuration"):
        checkpoints(setup).compare(receipt["receipt_id"])
    setup.bridge.call.return_value["project_file"] = "/other/Other.uproject"
    with pytest.raises(JevError, match="configured editor"):
        await checkpoints(setup).create(["Config.ini"])


async def test_fixed_process_runner_caps_output_and_redacts_failed_stderr(tmp_path):
    # Only a synthetic child Python process; never Git, Perforce, a shell or an editor.
    env = {}
    with pytest.raises(JevError) as failure:
        await fixed_status_command([sys.executable, "-c", "print('x' * 70000)"], tmp_path, env)
    assert failure.value.code == "vcs_output_limit"
    with pytest.raises(JevError) as failure:
        await fixed_status_command(
            [sys.executable, "-c", "import sys; sys.exit('secret')"], tmp_path, env
        )
    assert failure.value.code == "vcs_unavailable" and "secret" not in str(failure.value)


async def test_checkpoint_mcp_schema_is_bounded_and_real_methods_are_registered(setup):
    server = FastMCP("test")
    register_checkpoint_tools(server, setup.bridge, str(setup.config_file))
    tools = {tool.name: tool for tool in await server.list_tools()}
    assert tools["unreal_project_checkpoint"].inputSchema["properties"]["files"]["maxItems"] == 32
    assert tools["unreal_project_checkpoint_compare"].annotations.readOnlyHint is True
