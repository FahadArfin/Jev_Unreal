"""Installer tests use only disposable synthetic projects and plugin sources."""

import copy
import json
import os
import socket
from pathlib import Path

import pytest

from jev_unreal import setup
from jev_unreal.errors import JevError


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


@pytest.fixture
def project_source(tmp_path):
    project = tmp_path / "Game/Game.uproject"
    write_json(project, {"FileVersion": 3, "EngineAssociation": "5.8", "Description": "Keep me"})
    source = tmp_path / "Release/JevEditor"
    write_json(
        source / "JevEditor.uplugin",
        {
            "FileVersion": 3,
            "VersionName": "0.4.0",
            "EnabledByDefault": False,
            "Modules": [{"Name": "JevEditor", "Type": "Editor"}],
        },
    )
    code = source / "Source/JevEditor/Private/Example.cpp"
    code.parent.mkdir(parents=True)
    code.write_text("// Original release\n", encoding="utf-8")
    return project, source, project.parent / "Plugins/JevEditor"


def test_install_plan_is_read_only_and_enables_exact_project_entry(project_source):
    project, source, destination = project_source
    before = project.read_bytes()
    plan = setup.plan_install(project, source)
    assert plan["can_apply"] is True
    assert plan["project_entry_before"] is None
    assert plan["project_entry_after"] == {"Name": "JevEditor", "Enabled": True}
    assert plan["project_after"]["Description"] == "Keep me"
    assert {a["path"] for a in plan["actions"]} == {
        "JevEditor.uplugin",
        "Source/JevEditor/Private/Example.cpp",
    }
    assert project.read_bytes() == before
    assert not destination.exists()
    assert setup.plan_install(project, source) == plan


def test_install_backups_integrity_noop_and_stale_replay(project_source):
    project, source, destination = project_source
    before = project.read_bytes()
    plan = setup.plan_install(project, source)
    result = setup.apply_plan(plan)
    assert result["status"] == "installed"
    assert result["build_performed"] is False
    assert read_json(project)["Plugins"] == [{"Name": "JevEditor", "Enabled": True}]
    manifest = read_json(destination / setup.MANIFEST)
    assert manifest["owner"] == setup.OWNER
    assert manifest["files"] == plan["source_files"]
    backup = Path(result["backup_directory"])
    receipt = read_json(backup / "receipt.json")
    assert receipt["status"] == "complete"
    project_backup = next(v for v in receipt["files"] if v["path"] == str(project))
    assert (backup / project_backup["backup"]).read_bytes() == before
    assert not (destination.parent / ".JevEditor.install.lock").exists()
    with pytest.raises(JevError, match="changed") as exc:
        setup.apply_plan(plan)
    assert exc.value.code == "stale_setup_plan"
    unchanged = setup.apply_plan(setup.plan_install(project, source))
    assert unchanged["status"] == "unchanged"
    assert unchanged["backup_directory"] is None


@pytest.mark.parametrize(
    "plugins",
    [
        None,
        [],
        [{"Name": "Other", "Enabled": True}],
        [
            {"Name": "JevEditor", "Enabled": False, "SupportedTargetPlatforms": ["Win64"]},
            {"Name": "Other", "Enabled": False},
        ],
        [{"Name": "JevEditor", "Enabled": True}],
        [{"Name": "JevEditor"}],
    ],
)
def test_uninstall_restores_prior_project_entry_and_preserves_other_fields(project_source, plugins):
    project, source, destination = project_source
    original = read_json(project)
    if plugins is not None:
        original["Plugins"] = plugins
    write_json(project, original)
    setup.apply_plan(setup.plan_install(project, source))
    # Edits outside our plugin entry made after installation are not overwritten.
    current = read_json(project)
    current["LaterUserField"] = {"Answer": 42}
    write_json(project, current)
    result = setup.apply_plan(setup.plan_uninstall(project))
    assert result["status"] == "uninstalled"
    assert read_json(project) == {**original, "LaterUserField": {"Answer": 42}}
    assert not (destination / "JevEditor.uplugin").exists()
    assert not (destination / setup.MANIFEST).exists()
    assert not (destination / "Source/JevEditor/Private/Example.cpp").exists()


def test_upgrade_and_repair_with_owned_obsolete_file_removal(project_source):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    old = source / "Source/JevEditor/Private/Example.cpp"
    old.unlink()
    new = source / "Source/JevEditor/Private/Replacement.cpp"
    new.write_text("// New release\n", encoding="utf-8")
    metadata = read_json(source / "JevEditor.uplugin")
    metadata["VersionName"] = "0.5.0"
    write_json(source / "JevEditor.uplugin", metadata)
    plan = setup.plan_install(project, source)
    assert {v["action"] for v in plan["actions"]} == {"write", "remove"}
    setup.apply_plan(plan)
    assert not (destination / old.relative_to(source)).exists()
    installed_new = destination / new.relative_to(source)
    assert installed_new.read_bytes() == new.read_bytes()
    installed_new.unlink()
    repair = setup.plan_install(project, source)
    assert [a["path"] for a in repair["actions"]] == [new.relative_to(source).as_posix()]
    setup.apply_plan(repair)
    assert installed_new.read_bytes() == new.read_bytes()


def test_unknown_collision_is_never_adopted_even_identical(project_source):
    project, source, destination = project_source
    destination.mkdir(parents=True)
    (destination / "JevEditor.uplugin").write_bytes((source / "JevEditor.uplugin").read_bytes())
    plan = setup.plan_install(project, source)
    assert not plan["can_apply"]
    assert plan["conflicts"] == [{"path": "JevEditor.uplugin", "reason": "unowned_existing_file"}]
    with pytest.raises(JevError, match="conflict"):
        setup.apply_plan(plan)
    assert not (destination / setup.MANIFEST).exists()


def test_upgrade_blocks_local_modifications_uninstall_preserves_them(project_source):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    changed = destination / "Source/JevEditor/Private/Example.cpp"
    changed.write_text("// User changes\n", encoding="utf-8")
    unknown = destination / "Source/LocalNotes.txt"
    unknown.write_text("Keep this too", encoding="utf-8")
    binary = destination / "Binaries/Win64/local.dll"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"user generated")
    upgrade = setup.plan_install(project, source)
    assert not upgrade["can_apply"]
    assert upgrade["conflicts"][0]["reason"] == "locally_modified"
    uninstall = setup.plan_uninstall(project)
    assert uninstall["can_apply"]
    result = setup.apply_plan(uninstall)
    assert result["status"] == "partially_uninstalled"
    assert changed.read_text() == "// User changes\n"
    assert unknown.read_text() == "Keep this too"
    assert binary.read_bytes() == b"user generated"
    assert "Plugins" not in read_json(project)
    assert not (destination / setup.MANIFEST).exists()
    # Any subsequent installation still refuses to overwrite the preserved local source.
    assert not setup.plan_install(project, source)["can_apply"]


@pytest.mark.parametrize("change", ["project", "source", "destination", "manifest", "payload"])
def test_apply_rejects_stale_or_tampered_plan(project_source, change):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    plan = setup.plan_install(project, source)
    if change == "project":
        project.write_bytes(project.read_bytes() + b" ")
    elif change == "source":
        path = source / "Source/JevEditor/Private/Example.cpp"
        path.write_bytes(b"// Changed source")
    elif change == "destination":
        path = destination / "Source/JevEditor/Private/Example.cpp"
        path.write_bytes(b"// Changed destination")
    elif change == "manifest":
        path = destination / setup.MANIFEST
        path.write_bytes(path.read_bytes() + b" ")
    else:
        plan["project_after"] = {"Malicious": "Unreviewed write"}
    before = project.read_bytes()
    with pytest.raises(JevError) as exc:
        setup.apply_plan(plan)
    assert exc.value.code == "stale_setup_plan"
    assert project.read_bytes() == before


@pytest.mark.parametrize(
    "entry",
    [
        {"Name": "JevEditor", "Enabled": False},
        {
            "Name": "JevEditor",
            "Enabled": True,
            "NewField": "User edit",
        },
        None,
    ],
)
def test_external_plugin_entry_change_blocks_upgrade_and_uninstall(project_source, entry):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    current = read_json(project)
    current["Plugins"] = [] if entry is None else [entry]
    write_json(project, current)
    for plan in (setup.plan_install(project, source), setup.plan_uninstall(project)):
        assert not plan["can_apply"]
        assert any(v["reason"] == "project_plugin_entry_modified" for v in plan["conflicts"])
        with pytest.raises(JevError):
            setup.apply_plan(plan)
    assert (destination / "JevEditor.uplugin").exists()


def test_lock_is_not_deleted_or_overwritten(project_source):
    project, source, destination = project_source
    plan = setup.plan_install(project, source)
    lock = destination.parent / ".JevEditor.install.lock"
    lock.parent.mkdir(parents=True)
    lock.write_text("another-operation", encoding="utf-8")
    with pytest.raises(JevError, match="lock"):
        setup.apply_plan(plan)
    assert lock.read_text() == "another-operation"
    assert not destination.exists()


def test_failure_after_plugin_writes_rolls_back_original_bytes(project_source, monkeypatch):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    before = {
        p: p.read_bytes()
        for p in [
            project,
            destination / setup.MANIFEST,
            destination / "JevEditor.uplugin",
            destination / "Source/JevEditor/Private/Example.cpp",
        ]
    }
    code = source / "Source/JevEditor/Private/Example.cpp"
    code.write_text("// Upgraded source\n", encoding="utf-8")
    original_write = setup._atomic_write
    failed = False

    def fail_manifest(path, data):
        nonlocal failed
        if path == destination / setup.MANIFEST and not failed:
            failed = True
            raise OSError("Synthetic disk failure")
        return original_write(path, data)

    monkeypatch.setattr(setup, "_atomic_write", fail_manifest)
    with pytest.raises(OSError, match="Synthetic"):
        setup.apply_plan(setup.plan_install(project, source))
    assert failed
    assert all(p.read_bytes() == data for p, data in before.items())
    assert not (destination.parent / ".JevEditor.install.lock").exists()
    assert len(list((destination.parent / ".JevEditorBackups").iterdir())) == 2


def test_external_edit_during_failed_apply_is_not_overwritten_by_rollback(
    project_source, monkeypatch
):
    project, source, destination = project_source
    plan = setup.plan_install(project, source)
    original_write = setup._atomic_write
    victim = destination / "JevEditor.uplugin"

    def fail_after_edit(path, data):
        if path == project:
            victim.write_bytes(b"external user edit during apply")
            raise OSError("Synthetic interrupted apply")
        return original_write(path, data)

    monkeypatch.setattr(setup, "_atomic_write", fail_after_edit)
    with pytest.raises(JevError) as exc:
        setup.apply_plan(plan)
    assert exc.value.code == "setup_rollback_failed"
    assert victim.read_bytes() == b"external user edit during apply"
    assert not (destination / "Source/JevEditor/Private/Example.cpp").exists()
    assert "Plugins" not in read_json(project)


@pytest.mark.parametrize(
    "path",
    [
        "../outside.cpp",
        "/absolute.cpp",
        "Source/../outside.cpp",
        "Source\\outside.cpp",
        "Source/CON.cpp",
        "Source/file.cpp.",
        "Source/file.cpp ",
        "Source/a:stream.cpp",
        "Config/Local.ini",
    ],
)
def test_manifest_never_authorizes_traversal_or_other_project_files(project_source, path):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    manifest_path = destination / setup.MANIFEST
    manifest = read_json(manifest_path)
    record = next(iter(manifest["files"].values()))
    manifest["files"][path] = record
    write_json(manifest_path, manifest)
    with pytest.raises(JevError):
        setup.plan_uninstall(project)
    assert project.exists()


@pytest.mark.parametrize("mutation", ["owner", "project", "hash", "size", "entry", "extra"])
def test_malformed_manifest_is_not_adopted(project_source, mutation):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    path = destination / setup.MANIFEST
    manifest = read_json(path)
    if mutation == "owner":
        manifest["owner"] = "unknown installer"
    elif mutation == "project":
        manifest["project_file"] = "another-project.uproject"
    elif mutation == "hash":
        manifest["files"]["JevEditor.uplugin"]["sha256"] = "x" * 64
    elif mutation == "size":
        manifest["files"]["JevEditor.uplugin"]["bytes"] = -1
    elif mutation == "entry":
        manifest["project_entry"]["installed"]["Name"] = "OtherPlugin"
    else:
        manifest["unexpected"] = True
    write_json(path, manifest)
    with pytest.raises(JevError):
        setup.plan_install(project, source)


@pytest.mark.parametrize(
    "plugins",
    [
        None,
        {},
        ["JevEditor"],
        [{"Name": "JevEditor", "Enabled": "yes"}],
        [{"Name": "jevEDITOR"}],
        [{"Name": "JevEditor"}, {"Name": "JevEditor"}],
    ],
)
def test_malformed_or_ambiguous_project_entries_are_rejected(project_source, plugins):
    project, source, _ = project_source
    current = read_json(project)
    current["Plugins"] = plugins
    write_json(project, current)
    with pytest.raises(JevError):
        setup.plan_install(project, source)


def test_external_plugin_discovery_conflict_is_reported(project_source):
    project, source, _ = project_source
    current = read_json(project)
    current["AdditionalPluginDirectories"] = [str(source.parent)]
    write_json(project, current)
    plan = setup.plan_install(project, source)
    assert not plan["can_apply"]
    assert plan["duplicate_plugin_paths"] == [str(source / "JevEditor.uplugin")]
    assert plan["conflicts"][0]["reason"] == "duplicate_plugin"


@pytest.mark.parametrize("where", ["source", "destination", "ancestor", "manifest", "backup"])
def test_symbolic_links_are_rejected_without_touching_external_target(
    project_source, tmp_path, where
):
    project, source, destination = project_source
    outside = tmp_path / "Outside"
    outside.mkdir()
    marker = outside / "keep.txt"
    marker.write_text("must remain", encoding="utf-8")
    try:
        if where == "source":
            (source / "Source/External").symlink_to(outside, target_is_directory=True)
        elif where == "destination":
            destination.parent.mkdir(parents=True)
            destination.symlink_to(outside, target_is_directory=True)
        elif where == "ancestor":
            (project.parent / "Plugins").symlink_to(outside, target_is_directory=True)
        elif where == "manifest":
            destination.mkdir(parents=True)
            (destination / setup.MANIFEST).symlink_to(marker)
        else:
            destination.parent.mkdir(parents=True)
            (destination.parent / ".JevEditorBackups").symlink_to(outside, target_is_directory=True)
    except OSError:
        pytest.skip("Host does not permit creating symbolic links")
    with pytest.raises(JevError, match="links|reparse"):
        setup.apply_plan(setup.plan_install(project, source))
    assert marker.read_text() == "must remain"


def test_windows_reparse_attribute_is_rejected_even_if_not_symlink(project_source, monkeypatch):
    project, source, _ = project_source
    original = Path.lstat

    class Reparse:
        st_mode = 0o040755
        st_file_attributes = 0x400

    def patched(path):
        return Reparse() if path == source else original(path)

    monkeypatch.setattr(Path, "lstat", patched)
    with pytest.raises(JevError, match="reparse"):
        setup.plan_install(project, source)


def test_source_excludes_generated_files_and_private_notes(project_source):
    project, source, _ = project_source
    for name in ["Binaries/Win64/Jev.dll", "Intermediate/cache.txt", "Source/private.env"]:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("never install", encoding="utf-8")
    plan = setup.plan_install(project, source)
    assert len(plan["source_files"]) == 2


def test_read_plan_bounded_and_json_object_required(tmp_path, monkeypatch):
    path = tmp_path / "plan.json"
    write_json(path, {"plan": "synthetic"})
    assert setup.read_plan(path) == {"plan": "synthetic"}
    write_json(path, ["not an object"])
    with pytest.raises(JevError):
        setup.read_plan(path)
    monkeypatch.setattr(setup, "MAX_JSON_BYTES", 8)
    path.write_bytes(b" " * 33)
    with pytest.raises(JevError, match="size"):
        setup.read_plan(path)


@pytest.mark.parametrize(
    "plan",
    [
        None,
        {},
        [],
        {"schema": 1, "operation": "execute"},
        {
            "schema": 1,
            "operation": "install",
            "project_file": "game.uproject",
            "source_plugin": None,
        },
    ],
)
def test_invalid_apply_payload(plan):
    with pytest.raises(JevError) as exc:
        setup.apply_plan(plan)
    assert exc.value.code == "invalid_request"


def test_diagnostics_never_read_keys_or_recovery_contents(project_source, tmp_path, monkeypatch):
    project, source, _ = project_source
    local = tmp_path / "Credentials"
    local.mkdir()
    private = [local / name for name in ("bridge.token", "openrouter.dpapi", "typesafe.dpapi")]
    recovery = project.parent / "Saved/Autosaves/PackageRestoreData.json"
    recovery.parent.mkdir(parents=True)
    private.append(recovery)
    for path in private:
        path.write_bytes(b"SYNTHETIC-SECRET-CONTENT")
    original_open = Path.open

    def guarded_open(path, *args, **kwargs):
        if path in private:
            pytest.fail("Diagnostic tried to read a secret or recovery payload")
        return original_open(path, *args, **kwargs)

    monkeypatch.setattr(Path, "open", guarded_open)
    result = setup.inspect_setup(project, source_plugin=source, credential_directory=local)
    assert result["runtime_verified"] is False
    assert result["recovery_metadata"]["present"] is True
    assert all(v["present"] and v["contents_read"] is False for v in result["credentials"].values())
    assert "SYNTHETIC-SECRET-CONTENT" not in json.dumps(result)
    assert any("recovery" in step for step in result["next_steps"])


def test_diagnostics_report_busy_port_without_identifying_owner(project_source):
    project, _, _ = project_source
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        port = server.getsockname()[1]
        result = setup.inspect_setup(project, port=port)
    assert result["port"]["status"] == "in_use_or_unavailable"
    assert result["port"]["bridge_authenticated"] is False
    assert any("doctor" in step for step in result["next_steps"])


@pytest.mark.parametrize("port", [True, 0, 1023, 65536, "9845"])
def test_invalid_ports_never_probe(project_source, port):
    with pytest.raises(JevError, match="port"):
        setup.inspect_setup(project_source[0], port=port)


def test_engine_metadata_and_managed_diagnostics(project_source, tmp_path):
    project, source, destination = project_source
    setup.apply_plan(setup.plan_install(project, source))
    engine = tmp_path / "UE"
    write_json(
        engine / "Engine/Build/Build.version",
        {
            "MajorVersion": 5,
            "MinorVersion": 8,
            "PatchVersion": 2,
            "Changelist": 123,
        },
    )
    build = engine / (
        "Engine/Build/BatchFiles/Build.bat"
        if os.name == "nt"
        else "Engine/Build/BatchFiles/Linux/Build.sh"
    )
    build.parent.mkdir(parents=True)
    build.write_text("Do not execute me", encoding="utf-8")
    (destination / "Source/JevEditor/Private/Example.cpp").write_text("Modified", encoding="utf-8")
    result = setup.inspect_setup(project, engine_root=engine)
    assert result["project_enabled"] is True
    assert result["installed"]["managed"] is True
    assert result["installed"]["modified_files"] == ["Source/JevEditor/Private/Example.cpp"]
    assert result["engine"]["version"]["PatchVersion"] == 2
    assert result["engine"]["build_tool_present"] is True
    assert result["toolchain"]["verified_by_build"] is False


def test_source_size_and_count_bounds(project_source, monkeypatch):
    project, source, _ = project_source
    monkeypatch.setattr(setup, "MAX_FILES", 1)
    with pytest.raises(JevError, match="count|entries"):
        setup.plan_install(project, source)
    monkeypatch.setattr(setup, "MAX_FILES", 512)
    monkeypatch.setattr(setup, "MAX_TOTAL_BYTES", 1)
    with pytest.raises(JevError, match="size"):
        setup.plan_install(project, source)


def test_tampered_plan_has_no_effect_on_selected_project(project_source):
    project, source, _ = project_source
    plan = setup.plan_install(project, source)
    modified = copy.deepcopy(plan)
    modified["destination"] = str(project.parent / "Unexpected")
    with pytest.raises(JevError) as exc:
        setup.apply_plan(modified)
    assert exc.value.code == "stale_setup_plan"
    assert not (project.parent / "Unexpected").exists()


@pytest.mark.parametrize(
    "contents",
    [
        '{"FileVersion":3,"FileVersion":3}',
        '{"FileVersion":3,"value":NaN}',
        "[" * 1000 + "]" * 1000,
    ],
)
def test_project_json_ambiguity_is_rejected_before_any_write(project_source, contents):
    project, source, destination = project_source
    project.write_text(contents, encoding="utf-8")
    with pytest.raises(JevError):
        setup.plan_install(project, source)
    assert not destination.exists()


def test_network_share_paths_are_rejected_before_any_filesystem_probe(monkeypatch):
    from pathlib import PureWindowsPath

    from jev_unreal.setup import _absolute, _safe_path

    def no_stat(*args, **kwargs):
        pytest.fail("Network path must be refused before stat")

    monkeypatch.setattr(Path, "lstat", no_stat)
    for path in ("//server/share/Game.uproject", "\\\\server\\share\\Game.uproject"):
        with pytest.raises(JevError, match="local"):
            _absolute(path)
        with pytest.raises(JevError, match="local"):
            _safe_path(PureWindowsPath(path))
