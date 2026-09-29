"""Credential provenance diagnostics never decrypt, print, hash or authenticate keys."""

import json
from argparse import Namespace
from pathlib import Path
from unittest.mock import AsyncMock

import pytest

from jev_unreal import connections, setup
from jev_unreal.cli import run_command
from jev_unreal.config import Settings
from jev_unreal.errors import JevError


@pytest.fixture(autouse=True)
def registry_metadata(monkeypatch):
    monkeypatch.setattr(
        connections, "_windows_environment_presence", lambda name: {"user": False, "machine": None}
    )


def test_saved_key_timestamp_without_reading_contents(tmp_path, monkeypatch):
    saved = tmp_path / "openrouter.dpapi"
    saved.write_text("private ciphertext")
    monkeypatch.setattr(Path, "read_bytes", lambda self: pytest.fail("No contents read"))
    monkeypatch.setattr(Path, "read_text", lambda self, **kw: pytest.fail("No contents read"))
    report = connections.credential_diagnostics(
        Settings(), credential_directory=tmp_path, environment={}
    )
    assert report["saved_file"]["present"] is True
    assert report["saved_file"]["modified_at_utc"]
    assert report["selected_source"] == "not_loaded"
    assert report["issues"][0]["code"] == "saved_key_not_loaded"
    assert report["authentication"] == "unknown"
    assert "private ciphertext" not in json.dumps(report)
    assert "sha256" not in json.dumps(report)


@pytest.mark.parametrize(
    "marker,source,evidence",
    [
        (None, "process_environment", "local_configuration"),
        ("saved_dpapi", "saved_dpapi", "launcher_reported"),
        ("process_environment", "process_environment", "launcher_reported"),
        ("untrusted-private-marker", "process_environment", "local_configuration"),
    ],
)
def test_source_precedence_and_marker_enum_never_leak_secrets(tmp_path, marker, source, evidence):
    (tmp_path / "openrouter.dpapi").write_text("private saved value")
    env = {"OPENROUTER_API_KEY": "private process value", "JEV_CREDENTIAL_SOURCE": marker}
    report = connections.credential_diagnostics(
        Settings(api_key="private loaded value"),
        credential_directory=tmp_path,
        environment=env,
    )
    assert report["selected_source"] == source
    assert report["source_evidence"] == evidence
    assert report["provider_tested"] is False
    assert "private" not in json.dumps(report)
    assert any(
        issue["code"] == "environment_overrides_saved_key" for issue in report["issues"]
    ) is (marker != "saved_dpapi")


@pytest.mark.parametrize("redirected", [False, True], ids=["same-file", "app-overlay"])
def test_saved_key_physical_path_diagnoses_redirect_without_reading_contents(
    tmp_path, monkeypatch, redirected
):
    saved = tmp_path / "openrouter.dpapi"
    saved.write_text("synthetic ciphertext never read")
    physical = tmp_path / "package-cache" / saved.name if redirected else saved

    def resolve_metadata(path, *, strict=False):
        assert path == saved
        assert strict is True
        return physical

    monkeypatch.setattr(Path, "resolve", resolve_metadata)
    monkeypatch.setattr(Path, "open", lambda *args, **kwargs: pytest.fail("No contents read"))
    report = connections.credential_diagnostics(
        Settings(api_key="synthetic loaded key"),
        credential_directory=tmp_path,
        environment={"JEV_CREDENTIAL_SOURCE": "saved_dpapi"},
    )
    assert report["saved_file"]["logical_path"] == str(saved)
    assert report["saved_file"]["resolved_path"] == str(physical)
    assert report["saved_file"]["redirected"] is redirected
    assert report["saved_file"]["contents_read"] is False
    assert report["saved_file"]["present"] is True
    assert report["selected_source"] == "saved_dpapi"
    assert report["authentication"] == "unknown"
    assert report["provider_tested"] is False
    assert [issue["code"] for issue in report["issues"]] == (
        ["saved_key_path_redirected"] if redirected else []
    )
    if redirected:
        assert report["issues"][0]["severity"] == "warning"
        assert "Compare both paths and timestamps" in report["issues"][0]["next_action"]
    assert "synthetic" not in json.dumps(report)


def test_saved_key_resolution_failure_retains_known_metadata(tmp_path, monkeypatch):
    saved = tmp_path / "openrouter.dpapi"
    saved.write_text("synthetic ciphertext never read")

    def unavailable(path, *, strict=False):
        raise OSError("synthetic private diagnostic")

    monkeypatch.setattr(Path, "resolve", unavailable)
    monkeypatch.setattr(Path, "open", lambda *args, **kwargs: pytest.fail("No contents read"))
    report = connections.credential_diagnostics(
        Settings(), credential_directory=tmp_path, environment={}
    )
    assert report["saved_file"]["present"] is True
    assert report["saved_file"]["modified_at_utc"]
    assert report["saved_file"]["logical_path"] == str(saved)
    assert report["saved_file"]["resolved_path"] is None
    assert report["saved_file"]["redirected"] is None
    assert report["saved_file"]["resolution_error"] == "unavailable"
    assert report["saved_file"]["contents_read"] is False
    assert report["issues"][0]["code"] == "saved_key_not_loaded"
    assert "synthetic" not in json.dumps(report)


def test_typesafe_and_unavailable_metadata_are_distinct_from_authentication(tmp_path):
    report = connections.credential_diagnostics(
        Settings(provider="typesafe", api_key="secret"),
        credential_directory=tmp_path,
        environment={"OPENROUTER_API_KEY": "wrong-provider-secret"},
    )
    assert report["environment_variable"] == "TYPESAFE_API_KEY"
    assert report["environment_presence"]["process"] is False
    assert report["selected_source"] == "settings_supplied"
    assert report["saved_file"]["present"] is False
    assert report["authentication"] == "unknown"


async def test_doctor_wrong_project_has_actionable_sanitized_guidance_without_provider(monkeypatch):
    bridge = AsyncMock()
    bridge.call.side_effect = JevError("wrong_project", "Editor identity differs.")
    monkeypatch.setattr("jev_unreal.cli.UnrealBridge", lambda settings: bridge)
    monkeypatch.setattr("jev_unreal.cli.DecisionClient", lambda *_: pytest.fail("No provider"))
    result = await run_command(Namespace(command="doctor"), Settings(expected_project="intended"))
    issue = next(item for item in result["issues"] if item["code"] == "wrong_project")
    assert "other game's editor" in issue["next_action"]
    bridge.call.assert_awaited_once_with("status")
    assert result["provider"]["diagnostics"]["provider_tested"] is False


def test_setup_presence_reports_timestamp_without_opening_secret(tmp_path, monkeypatch):
    saved = tmp_path / "openrouter.dpapi"
    saved.write_text("private")
    monkeypatch.setattr(Path, "open", lambda *args, **kwargs: pytest.fail("No contents read"))
    report = setup._presence(saved)
    assert report["modified_at_utc"]
    assert report["contents_read"] is False
    assert "private" not in json.dumps(report)
