"""Real command-line acceptance in disposable projects, without Unreal or credentials."""

import json
import os
import subprocess
import sys

import pytest


@pytest.fixture
def cli_project(tmp_path):
    project = tmp_path / "My Game/My Game.uproject"
    project.parent.mkdir()
    original = {
        "FileVersion": 3,
        "EngineAssociation": "5.8",
        "Description": "Preserve this project field",
        "Plugins": [{"Name": "ExistingPlugin", "Enabled": True}],
        "CustomNested": {"dimensions": [10, 20, 30]},
    }
    project.write_text(json.dumps(original), encoding="utf-8")
    source = tmp_path / "Trusted Release/JevEditor"
    module = source / "Source/JevEditor/JevEditor.Build.cs"
    module.parent.mkdir(parents=True)
    module.write_text("// Synthetic source fixture; never compiled.\n", encoding="utf-8")
    (source / "JevEditor.uplugin").write_text(
        json.dumps(
            {
                "FileVersion": 3,
                "VersionName": "0.4.0",
                "Modules": [{"Name": "JevEditor", "Type": "Editor"}],
            }
        ),
        encoding="utf-8",
    )
    environment = {
        **os.environ,
        "JEV_BRIDGE_TOKEN": "",
        "JEV_BRIDGE_TOKEN_FILE": str(tmp_path / "MUST-NOT-READ-TOKEN"),
        "JEV_PROFILES_FILE": str(tmp_path / "MUST-NOT-READ-PROFILES"),
        "JEV_PROFILE": "missing-profile",
        "JEV_PROVIDER": "invalid-must-not-load-settings",
        "JEV_BRIDGE_PORT": "not-an-integer",
        "OPENROUTER_API_KEY": "",
        "TYPESAFE_API_KEY": "",
        "LOCALAPPDATA": str(tmp_path / "PrivateLocalData"),
    }

    def run(*arguments, success=True):
        process = subprocess.run(
            [sys.executable, "-m", "jev_unreal", *map(str, arguments)],
            cwd=tmp_path,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if success:
            assert process.returncode == 0, process.stderr
            assert not process.stderr
            return json.loads(process.stdout)
        assert process.returncode != 0
        assert not process.stdout
        return json.loads(process.stderr)

    return project, source, original, run


def test_cli_full_lifecycle_preserves_project_and_never_loads_connection_config(
    cli_project, tmp_path
):
    project, source, original, run = cli_project
    before = project.read_bytes()
    inspected = run("setup", "inspect", "--project", project, "--source-plugin", source)
    assert inspected["project_enabled"] is False
    assert inspected["runtime_verified"] is False
    assert inspected["source"]["version"] == "0.4.0"
    plan_file = tmp_path / "private" / "reviews" / "install-plan.json"
    planned = run(
        "setup", "plan", "--project", project, "--source-plugin", source, "--output", plan_file
    )
    assert planned == json.loads(plan_file.read_text(encoding="utf-8"))
    assert planned["can_apply"] is True
    assert project.read_bytes() == before
    result = run("setup", "apply", plan_file)
    assert result["status"] == "installed"
    installed = json.loads(project.read_text(encoding="utf-8"))
    assert installed == {
        **original,
        "Plugins": [
            *original["Plugins"],
            {
                "Name": "JevEditor",
                "Enabled": True,
            },
        ],
    }
    assert inspected["source"]["files"] == 2
    destination = project.parent / "Plugins/JevEditor"
    assert (destination / "Source/JevEditor/JevEditor.Build.cs").exists()
    after = run("setup", "inspect", "--project", project)
    assert after["project_enabled"] is True
    assert after["installed"]["managed"] is True
    assert after["installed"]["modified_files"] == []
    removal_file = tmp_path / "uninstall-plan.json"
    removal = run("setup", "uninstall-plan", "--project", project, "--output", removal_file)
    assert removal["project_entry_after"] is None
    result = run("setup", "apply", removal_file)
    assert result["status"] == "uninstalled"
    assert json.loads(project.read_text(encoding="utf-8")) == original
    assert not (destination / "JevEditor.uplugin").exists()
    assert not (destination / ".jev-install.json").exists()


def test_cli_output_is_exclusive_and_preserves_existing_file(cli_project, tmp_path):
    project, source, _, run = cli_project
    output = tmp_path / "already-reviewed.json"
    output.write_text("Keep this unrelated file", encoding="utf-8")
    result = run(
        "setup",
        "plan",
        "--project",
        project,
        "--source-plugin",
        source,
        "--output",
        output,
        success=False,
    )
    assert result["error"]["code"] == "input_error"
    assert output.read_text() == "Keep this unrelated file"
    assert not (project.parent / "Plugins").exists()


def test_cli_stale_apply_is_structured_and_does_not_change_files(cli_project, tmp_path):
    project, source, _, run = cli_project
    plan = tmp_path / "plan.json"
    run("setup", "plan", "--project", project, "--source-plugin", source, "--output", plan)
    project.write_bytes(project.read_bytes() + b" ")
    before = project.read_bytes()
    result = run("setup", "apply", plan, success=False)
    assert result["error"]["code"] == "stale_setup_plan"
    assert project.read_bytes() == before
    assert not (project.parent / "Plugins").exists()


def test_profiles_list_cli_does_not_resolve_any_token_or_inherited_selection(cli_project, tmp_path):
    project, _, _, run = cli_project
    config = tmp_path / "profiles-to-list.json"
    config.write_text(
        json.dumps(
            {
                "version": 1,
                "profiles": [
                    {
                        "id": "listed",
                        "project_file": str(project),
                        "bridge_url": "http://127.0.0.1:9846",
                        "bridge_token_file": str(tmp_path / "MISSING-SELECTED-TOKEN"),
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    result = run("profiles", "list", config)
    assert result == {
        "profiles": [
            {
                "id": "listed",
                "project_file": str(project),
                "bridge_url": "http://127.0.0.1:9846",
                "token_file_configured": True,
            }
        ]
    }
