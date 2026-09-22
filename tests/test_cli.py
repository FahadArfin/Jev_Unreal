"""Public CLI diagnostics must work without provider requests or secret disclosure."""

from argparse import Namespace
from unittest.mock import AsyncMock

import pytest

from jev_unreal.cli import run_command
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
        bridge.call.return_value = {"project_file": "test.uproject"}
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
