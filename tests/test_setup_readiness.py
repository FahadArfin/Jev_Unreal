"""Read-only synthetic prerequisite fixtures; never run a compiler, SDK or engine."""

import json
import os

import pytest

from jev_unreal import setup


@pytest.fixture
def selected_engine(tmp_path, monkeypatch):
    project = tmp_path / "Project" / "Fixture.uproject"
    project.parent.mkdir()
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "5.8"}))
    engine = tmp_path / "Engine"
    version = engine / "Engine/Build/Build.version"
    version.parent.mkdir(parents=True)
    version.write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 1}))
    monkeypatch.setattr(
        setup,
        "_toolchain",
        lambda: {
            "platform": "Windows",
            "msvc": [],
            "windows_sdk": [],
            "windows_sdk_components": [],
        },
    )
    return project, engine


def test_missing_engine_entrypoints_and_toolchain_have_actionable_diagnostics(selected_engine):
    project, engine = selected_engine
    report = setup.inspect_setup(project, engine_root=engine)
    checks = {row["id"]: row for row in report["readiness"]["checks"]}
    assert checks["engine_association"]["status"] == "present"
    assert checks["ubt_assembly"]["status"] == "missing_or_unavailable"
    assert checks["msvc_components"]["status"] == "missing_or_unverified"
    assert checks["windows_sdk_components"]["guidance"]
    assert report["readiness"]["metadata_prerequisites_satisfied"] is False
    assert report["readiness"]["fresh_host_verified"] is False
    assert report["readiness"]["build_verified"] is False


def test_selected_engine_mismatch_is_not_silently_accepted(selected_engine):
    project, engine = selected_engine
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "5.7"}))
    report = setup.inspect_setup(project, engine_root=engine)
    assert report["engine"]["prerequisites"]["association_match"] is False
    assert "engine_association" in report["readiness"]["known_blockers"]


def test_custom_engine_association_is_explicitly_unverified(selected_engine):
    project, engine = selected_engine
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "{custom-engine-guid}"}))
    report = setup.inspect_setup(project, engine_root=engine)
    assert report["engine"]["prerequisites"]["association_match"] is None
    assert report["readiness"]["metadata_prerequisites_satisfied"] is False


def test_bundled_dotnet_metadata_does_not_claim_runtime_loading(selected_engine):
    project, engine = selected_engine
    dotnet = engine / "Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe"
    dotnet.parent.mkdir(parents=True)
    dotnet.write_bytes(b"not executable; fixture only")
    config = engine / "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.runtimeconfig.json"
    config.parent.mkdir(parents=True)
    config.write_text(
        json.dumps(
            {
                "runtimeOptions": {
                    "framework": {"name": "Microsoft.NETCore.App", "version": "10.0.0"}
                }
            }
        )
    )
    report = setup.inspect_setup(project, engine_root=engine)["engine"]["prerequisites"]
    assert report["bundled_dotnet_hosts"] == [str(dotnet)]
    assert report["runtime_requirement"]["version"] == "10.0.0"
    assert report["runtime_load_verified"] is False


@pytest.mark.skipif(os.name != "nt", reason="Windows standard-location scanner fixture")
def test_compiler_file_alone_is_not_a_complete_toolchain(tmp_path, monkeypatch):
    monkeypatch.setenv("ProgramFiles", str(tmp_path))
    monkeypatch.setenv("ProgramFiles(x86)", str(tmp_path))
    compiler = (
        tmp_path
        / "Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.0/bin/Hostx64/x64/cl.exe"
    )
    compiler.parent.mkdir(parents=True)
    compiler.write_bytes(b"fixture only")
    report = setup._toolchain()
    assert report["msvc"][0]["complete_presence"] is False
    assert report["msvc"][0]["components"]["compiler"] is True
    assert report["msvc"][0]["components"]["linker"] is False
