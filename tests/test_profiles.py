"""Connection profiles isolate editor identities and never borrow legacy credentials."""

import copy
import json
import os
import shutil
import subprocess
from pathlib import Path

import httpx
import pytest

from jev_unreal.bridge import UnrealBridge
from jev_unreal.config import Settings
from jev_unreal.errors import JevError
from jev_unreal.profiles import read_profiles, read_token_file, selected_profile, summaries


@pytest.fixture(autouse=True)
def clean_environment(monkeypatch):
    for name in (
        "JEV_PROFILES_FILE",
        "JEV_PROFILE",
        "JEV_BRIDGE_TOKEN",
        "JEV_BRIDGE_TOKEN_FILE",
        "JEV_EXPECTED_PROJECT",
        "JEV_BRIDGE_URL",
        "JEV_BRIDGE_PORT",
        "JEV_MAX_REQUESTS",
    ):
        monkeypatch.delenv(name, raising=False)


@pytest.fixture
def profile_file(tmp_path):
    profiles = []
    for index, name in enumerate(("game", "sandbox")):
        token_file = tmp_path / f"{name}.token"
        token_file.write_text(name + "-synthetic-only-token-" + "x" * 32, encoding="utf-8")
        project_file = tmp_path / name / f"{name}.uproject"
        project_file.parent.mkdir()
        project_file.write_text('{"FileVersion":3}', encoding="utf-8")
        profiles.append(
            {
                "id": name,
                "project_file": str(project_file),
                "bridge_url": f"http://127.0.0.1:{9845 + index}",
                "bridge_token_file": str(token_file),
            }
        )
    path = tmp_path / "profiles.json"
    path.write_text(json.dumps({"version": 1, "profiles": profiles}), encoding="utf-8")
    return path, profiles


def write_profiles(path, profiles):
    path.write_text(json.dumps({"version": 1, "profiles": profiles}), encoding="utf-8")


def test_summaries_do_not_read_token_files(profile_file, monkeypatch):
    path, definitions = profile_file
    token_paths = {Path(p["bridge_token_file"]) for p in definitions}
    original_open = Path.open

    def guard(candidate, *args, **kwargs):
        if candidate in token_paths:
            pytest.fail("Profile listing opened a token file")
        return original_open(candidate, *args, **kwargs)

    monkeypatch.setattr(Path, "open", guard)
    result = summaries(path)
    assert len(result) == 2
    assert result[1]["id"] == "sandbox"
    assert result[1]["token_file_configured"] is True
    assert "synthetic-only-token" not in str(result)
    assert "bridge_token_file" not in str(result)
    assert "synthetic-only-token" not in repr(read_profiles(path))


def test_selected_profile_ignores_every_legacy_connection_field(profile_file, monkeypatch):
    path, definitions = profile_file
    monkeypatch.setenv("JEV_PROFILES_FILE", str(path))
    monkeypatch.setenv("JEV_PROFILE", "sandbox")
    monkeypatch.setenv("JEV_EXPECTED_PROJECT", "wrong.uproject")
    monkeypatch.setenv("JEV_BRIDGE_URL", "https://remote.invalid")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN", "legacy-token-must-not-be-used")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", str(path.parent / "missing.token"))
    monkeypatch.setenv("JEV_BRIDGE_PORT", "invalid")
    settings = Settings.from_env()
    assert settings.profile_id == "sandbox"
    assert settings.expected_project == definitions[1]["project_file"]
    assert settings.bridge_url == definitions[1]["bridge_url"]
    assert settings.bridge_token == Path(definitions[1]["bridge_token_file"]).read_text()
    assert "synthetic-only-token" not in repr(settings)


def test_only_selected_token_file_is_resolved(profile_file, monkeypatch):
    path, definitions = profile_file
    Path(definitions[0]["bridge_token_file"]).unlink()
    monkeypatch.setenv("JEV_PROFILES_FILE", str(path))
    monkeypatch.setenv("JEV_PROFILE", "sandbox")
    assert Settings.from_env().profile_id == "sandbox"
    monkeypatch.setenv("JEV_PROFILE", "game")
    with pytest.raises(JevError):
        Settings.from_env()


@pytest.mark.parametrize("missing", ["JEV_PROFILE", "JEV_PROFILES_FILE"])
def test_incomplete_profile_selection_never_falls_back(profile_file, monkeypatch, missing):
    path, _ = profile_file
    monkeypatch.setenv("JEV_PROFILES_FILE", str(path))
    monkeypatch.setenv("JEV_PROFILE", "game")
    monkeypatch.delenv(missing)
    monkeypatch.setenv("JEV_BRIDGE_TOKEN", "legacy-token-" + "x" * 32)
    with pytest.raises(JevError, match="both"):
        Settings.from_env()


def test_unknown_profile_is_structured_without_echoing_identifier(profile_file):
    with pytest.raises(JevError) as exc:
        selected_profile(profile_file[0], "SYNTHETIC-PRIVATE-IDENTIFIER")
    assert "SYNTHETIC-PRIVATE-IDENTIFIER" not in str(exc.value)


@pytest.mark.parametrize(
    "field,value",
    [
        ("bridge_url", "http://example.com:9845"),
        ("bridge_url", "http://localhost:9845"),
        ("bridge_url", "https://127.0.0.1:9845"),
        ("bridge_url", "http://127.0.0.1:9845/path"),
        ("bridge_url", "http://secret@127.0.0.1:9845"),
        ("bridge_url", "http://127.0.0.1:80"),
        ("bridge_url", "http://127.0.0.1:65536"),
        ("bridge_url", "http://127.0.0.1:09845"),
        ("bridge_url", "http://[::1]:9845"),
        ("bridge_url", 9845),
        ("id", "Bad Name"),
        ("id", "x" * 49),
        ("id", ""),
        ("id", None),
        ("project_file", "relative.uproject"),
        ("project_file", "C:relative.uproject"),
        ("project_file", "//server/share/game.uproject"),
        ("project_file", None),
        ("bridge_token_file", "relative.token"),
        ("bridge_token_file", "//server/share/token"),
        ("bridge_token_file", None),
    ],
)
def test_invalid_profile_fields(profile_file, field, value):
    path, definitions = profile_file
    definitions[0][field] = value
    write_profiles(path, definitions)
    with pytest.raises(JevError) as exc:
        read_profiles(path)
    assert exc.value.code == "configuration"


@pytest.mark.parametrize("duplicate", ["id", "project_file", "bridge_url"])
def test_duplicate_connection_identity_is_rejected(profile_file, duplicate):
    path, definitions = profile_file
    definitions[1][duplicate] = definitions[0][duplicate]
    write_profiles(path, definitions)
    with pytest.raises(JevError, match="unique"):
        read_profiles(path)


def test_normalized_endpoint_and_project_duplicates_are_rejected(profile_file):
    path, definitions = profile_file
    definitions[1]["bridge_url"] = definitions[0]["bridge_url"] + "/"
    write_profiles(path, definitions)
    with pytest.raises(JevError, match="unique"):
        read_profiles(path)


@pytest.mark.parametrize("field", ["api_key", "bridge_token", "unknown"])
def test_inline_keys_and_unrecognized_fields_are_not_permitted(profile_file, field):
    path, definitions = profile_file
    definitions[0][field] = "synthetic-private-value"
    write_profiles(path, definitions)
    with pytest.raises(JevError) as exc:
        read_profiles(path)
    assert "synthetic-private-value" not in str(exc.value)


@pytest.mark.parametrize(
    "value",
    [
        {},
        [],
        {"version": True, "profiles": []},
        {"version": 2, "profiles": []},
        {"version": 1, "profiles": []},
        {"version": 1, "profiles": [None]},
        {"version": 1, "profiles": {}, "extra": 1},
    ],
)
def test_root_schema_is_strict(profile_file, value):
    path, _ = profile_file
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(JevError):
        read_profiles(path)


def test_bounded_size_count_and_duplicate_json_keys(profile_file):
    path, definitions = profile_file
    write_profiles(path, [copy.deepcopy(definitions[0]) for _ in range(9)])
    with pytest.raises(JevError, match="eight"):
        read_profiles(path)
    path.write_bytes(b" " * 65537)
    with pytest.raises(JevError):
        read_profiles(path)
    path.write_text('{"version": 1, "version": 1, "profiles": []}', encoding="utf-8")
    with pytest.raises(JevError):
        read_profiles(path)


@pytest.mark.parametrize(
    "contents",
    [
        b"x" * 4097,
        b"short",
        b"\xffprivate-value",
        b"x" * 40 + b"\x00",
        b"x" * 40 + b" y",
        ("x" * 40 + "é").encode(),
    ],
)
def test_bad_token_files_never_echo_content_or_path(tmp_path, contents):
    path = tmp_path / "private-value.token"
    path.write_bytes(contents)
    with pytest.raises(JevError) as exc:
        read_token_file(path)
    assert "private-value" not in str(exc.value)
    assert str(path) not in str(exc.value)


def test_valid_bounded_token_accepts_bom_and_trailing_newline(tmp_path):
    path = tmp_path / "bridge.token"
    path.write_text("x" * 64 + "\n", encoding="utf-8-sig")
    assert read_token_file(path) == "x" * 64


def test_token_symlinks_and_directory_are_rejected(tmp_path):
    with pytest.raises(JevError):
        read_token_file(tmp_path)
    target = tmp_path / "target.token"
    target.write_text("x" * 64, encoding="utf-8")
    link = tmp_path / "link.token"
    try:
        link.symlink_to(target)
    except OSError:
        pytest.skip("Host does not permit creating symbolic links")
    with pytest.raises(JevError):
        read_token_file(link)


@pytest.mark.parametrize("port", ["80", "65536", "oops", "-9845", "９８４５"])
def test_invalid_legacy_native_port_is_not_used(port, monkeypatch):
    monkeypatch.setenv("JEV_BRIDGE_PORT", port)
    with pytest.raises(JevError, match="PORT"):
        Settings.from_env()


def test_legacy_port_url_precedence_and_default_are_backwards_compatible(monkeypatch):
    assert Settings.from_env().bridge_url == "http://127.0.0.1:9845"
    monkeypatch.setenv("JEV_BRIDGE_PORT", "9846")
    assert Settings.from_env().bridge_url == "http://127.0.0.1:9846"
    monkeypatch.setenv("JEV_BRIDGE_URL", "http://127.0.0.1:9847")
    assert Settings.from_env().bridge_url == "http://127.0.0.1:9847"


async def test_two_editors_keep_tokens_urls_and_project_checks_isolated(profile_file, monkeypatch):
    path, definitions = profile_file
    seen = []
    swapped = False

    def handler(request):
        index = request.url.port - 9845
        profile = definitions[index]
        expected_token = Path(profile["bridge_token_file"]).read_text()
        assert request.headers["Authorization"] == "Bearer " + expected_token
        action = json.loads(request.content)["action"]
        seen.append((index, action))
        if action == "status":
            identity = definitions[1 - index] if swapped else profile
            return httpx.Response(
                200,
                json={
                    "ok": True,
                    "result": {
                        "project_file": identity["project_file"],
                        "capabilities": [],
                    },
                },
            )
        return httpx.Response(200, json={"ok": True, "result": {"editor": profile["id"]}})

    monkeypatch.setenv("JEV_PROFILES_FILE", str(path))
    monkeypatch.setenv("JEV_PROFILE", "game")
    game_settings = Settings.from_env()
    monkeypatch.setenv("JEV_PROFILE", "sandbox")
    sandbox_settings = Settings.from_env()
    game = UnrealBridge(game_settings, httpx.MockTransport(handler))
    sandbox = UnrealBridge(sandbox_settings, httpx.MockTransport(handler))
    try:
        assert (await game.call("context"))["editor"] == "game"
        assert (await sandbox.call("context"))["editor"] == "sandbox"
        swapped = True
        for bridge in (game, sandbox):
            before = len(seen)
            with pytest.raises(JevError) as exc:
                await bridge.call("apply", {"plan_id": "never-send-to-wrong-project"})
            assert exc.value.code == "wrong_project"
            assert len(seen) == before + 1
            assert seen[-1][1] == "status"
    finally:
        await game.close()
        await sandbox.close()
    # Changing selection in the environment does not retarget an already constructed server.
    assert game.settings.profile_id == "game"
    assert sandbox.settings.profile_id == "sandbox"


@pytest.mark.skipif(os.name != "nt", reason="Windows PowerShell launcher integration")
def test_powershell_launcher_clears_legacy_selection_and_restores_environment(
    profile_file, tmp_path
):
    path, _ = profile_file
    script = Path(__file__).resolve().parents[1] / "scripts/Start-Mcp.ps1"
    shell = shutil.which("powershell")
    assert shell
    harness = tmp_path / "launcher-test.ps1"
    result_file = tmp_path / "observed.json"
    # Paths use environment variables; no path interpolation into executable shell text.
    harness.write_text(
        r"""
$ErrorActionPreference = 'Stop'
$env:OPENROUTER_API_KEY = 'synthetic-provider-key-no-decryption'
$env:JEV_BRIDGE_TOKEN = 'legacy-token-must-not-be-used'
$env:JEV_BRIDGE_TOKEN_FILE = 'legacy-token-file'
$env:JEV_EXPECTED_PROJECT = 'legacy-project'
$env:JEV_BRIDGE_URL = 'http://127.0.0.1:9999'
$env:JEV_BRIDGE_PORT = '9999'
$env:JEV_PROFILES_FILE = $null
$env:JEV_PROFILE = $null
function uv {
    $result = @{
        profile = $env:JEV_PROFILE
        profiles = $env:JEV_PROFILES_FILE
        project = $env:JEV_EXPECTED_PROJECT
        token = $env:JEV_BRIDGE_TOKEN
        token_file = $env:JEV_BRIDGE_TOKEN_FILE
        url = $env:JEV_BRIDGE_URL
        port = $env:JEV_BRIDGE_PORT
    }
    [IO.File]::WriteAllText($env:JEV_TEST_OUTPUT, ($result | ConvertTo-Json))
    $global:LASTEXITCODE = 0
}
& $env:JEV_TEST_SCRIPT -ProfilesFile $env:JEV_TEST_PROFILES -Profile sandbox
if ($env:JEV_BRIDGE_TOKEN -ne 'legacy-token-must-not-be-used') { throw 'Token not restored' }
if ($env:JEV_BRIDGE_TOKEN_FILE -ne 'legacy-token-file') { throw 'Token file not restored' }
if ($env:JEV_EXPECTED_PROJECT -ne 'legacy-project') { throw 'Project not restored' }
if ($env:JEV_BRIDGE_URL -ne 'http://127.0.0.1:9999') { throw 'URL not restored' }
if ($env:JEV_BRIDGE_PORT -ne '9999') { throw 'Port not restored' }
if ($env:JEV_PROFILE) { throw 'Profile not restored' }
if ($env:JEV_PROFILES_FILE) { throw 'Profile file not restored' }
""",
        encoding="utf-8",
    )
    environment = {
        **os.environ,
        "JEV_TEST_SCRIPT": str(script),
        "JEV_TEST_OUTPUT": str(result_file),
        "JEV_TEST_PROFILES": str(path),
    }
    result = subprocess.run(
        [
            shell,
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(harness),
        ],
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    observed = json.loads(result_file.read_text(encoding="utf-8-sig"))
    assert observed["profile"] == "sandbox"
    assert observed["profiles"] == str(path)
    assert all(not observed[key] for key in ("project", "token", "token_file", "url", "port"))
