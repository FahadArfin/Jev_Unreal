"""Configuration must constrain destinations and keep credentials out of output."""

import pytest

from jev_unreal.config import Settings, loopback_url
from jev_unreal.errors import JevError


@pytest.fixture(autouse=True)
def clear_configuration_environment(monkeypatch):
    for name in (
        "JEV_PROVIDER",
        "OPENROUTER_API_KEY",
        "TYPESAFE_API_KEY",
        "JEV_MODEL",
        "JEV_BRIDGE_URL",
        "JEV_BRIDGE_TOKEN",
        "JEV_BRIDGE_TOKEN_FILE",
        "JEV_EXPECTED_PROJECT",
        "JEV_MAX_REQUESTS",
        "JEV_CATALOG_FILE",
        "JEV_PROFILES_FILE",
        "JEV_PROFILE",
        "JEV_BRIDGE_PORT",
    ):
        monkeypatch.delenv(name, raising=False)


@pytest.mark.parametrize(
    "value",
    [
        "https://127.0.0.1:9845",
        "http://localhost:9845",
        "http://192.168.1.3:9845",
        "http://example.com:9845",
        "http://127.0.0.1.example.com:9845",
        "http://127.0.0.1:9845@evil.example:9845",
        "http://user:private-password@127.0.0.1:9845",
        "http://127.0.0.1:9845/path",
        "http://127.0.0.1:9845?secret=value",
        "http://127.0.0.1:9845#fragment",
        "http://127.0.0.1",
        "http://127.0.0.1:notaport",
        "http://127.0.0.1:65536",
        "http://[::1:9845",
        "http://[not-an-address]:9845",
        "",
    ],
)
def test_bridge_origin_rejects_remote_ambiguous_and_malformed_urls(value):
    with pytest.raises(JevError) as caught:
        loopback_url(value)
    assert caught.value.code == "configuration"
    assert "private-password" not in str(caught.value)


@pytest.mark.parametrize("origin", ["http://127.0.0.1:9845", "http://[::1]:9845"])
def test_explicit_loopback_origins_are_supported(origin):
    assert loopback_url(origin + "/") == origin


def test_credentials_are_omitted_from_settings_representation():
    settings = Settings(api_key="synthetic-private-key", bridge_token="synthetic-bridge-secret")
    assert "synthetic-private-key" not in repr(settings)
    assert "synthetic-bridge-secret" not in repr(settings)


def test_missing_credentials_are_allowed_for_offline_configuration():
    settings = Settings.from_env()
    assert settings.api_key == ""
    assert settings.bridge_token == ""
    assert settings.expected_project == ""


@pytest.mark.parametrize(
    ("provider", "expected_key", "expected_model", "expected_endpoint"),
    [
        (
            "openrouter",
            "synthetic-openrouter-key",
            "typesafe/jev-1.13",
            "https://openrouter.ai/api/alpha/decisions",
        ),
        (
            "typesafe",
            "synthetic-typesafe-key",
            "jev-1.13.0",
            "https://api.typesafe.ai/v1/systemone",
        ),
    ],
)
def test_provider_selects_only_its_own_credentials(
    monkeypatch, provider, expected_key, expected_model, expected_endpoint
):
    monkeypatch.setenv("JEV_PROVIDER", provider)
    monkeypatch.setenv("OPENROUTER_API_KEY", "synthetic-openrouter-key")
    monkeypatch.setenv("TYPESAFE_API_KEY", "synthetic-typesafe-key")
    settings = Settings.from_env()
    assert settings.api_key == expected_key
    assert settings.model == expected_model
    assert settings.endpoint == expected_endpoint


def test_local_token_file_accepts_bom_and_trailing_newline(monkeypatch, tmp_path):
    token_file = tmp_path / "token.txt"
    token_file.write_text("a" * 64 + "\n", encoding="utf-8-sig")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", str(token_file))
    assert Settings.from_env().bridge_token == "a" * 64


def test_explicit_token_takes_precedence_over_token_file(monkeypatch, tmp_path):
    monkeypatch.setenv("JEV_BRIDGE_TOKEN", "a" * 64)
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", str(tmp_path / "missing.txt"))
    assert Settings.from_env().bridge_token == "a" * 64


@pytest.mark.parametrize("invalid_encoding", [False, True])
def test_token_file_errors_are_structured_and_do_not_echo_content(
    monkeypatch, tmp_path, invalid_encoding
):
    token_file = tmp_path / "private-token-path.txt"
    if invalid_encoding:
        token_file.write_bytes(b"\xffsynthetic-private-token")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", str(token_file))
    with pytest.raises(JevError) as caught:
        Settings.from_env()
    assert caught.value.code == "configuration"
    assert "synthetic-private-token" not in str(caught.value)
    assert str(token_file) not in str(caught.value)


@pytest.mark.parametrize("limit", ["not-a-number", "0", "10001"])
def test_invalid_session_request_limit_fails_before_server_start(monkeypatch, limit):
    monkeypatch.setenv("JEV_MAX_REQUESTS", limit)
    with pytest.raises(JevError) as caught:
        Settings.from_env()
    assert caught.value.code == "configuration"


@pytest.mark.parametrize(
    "arguments",
    [
        {"provider": "arbitrary-service"},
        {"max_requests": 0},
        {"max_requests": 10001},
        {"cache_seconds": -1},
        {"cache_seconds": 3601},
        {"timeout_seconds": 0},
        {"timeout_seconds": 121},
        {"timeout_seconds": float("nan")},
    ],
)
def test_invalid_configuration_bounds(arguments):
    with pytest.raises(JevError) as caught:
        Settings(**arguments)
    assert caught.value.code == "configuration"
