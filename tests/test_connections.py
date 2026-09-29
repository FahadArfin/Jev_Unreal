import json
from unittest.mock import AsyncMock

from jev_unreal import __version__
from jev_unreal.connections import check_profiles
from jev_unreal.errors import JevError


async def test_profiles_check_only_explicit_endpoints_preserves_failures(tmp_path):
    token = tmp_path / "bridge.token"
    token.write_text("x" * 40)
    profiles = [
        {
            "id": identifier,
            "project_file": str(tmp_path / (identifier + ".uproject")),
            "bridge_url": f"http://127.0.0.1:{9845 + i}",
            "bridge_token_file": str(token),
        }
        for i, identifier in enumerate(("first", "second"))
    ]
    path = tmp_path / "profiles.json"
    path.write_text(json.dumps({"version": 1, "profiles": profiles}))
    bridges, configs = [], []

    def factory(config):
        configs.append(config)
        bridge = AsyncMock()
        if config.profile_id == "first":
            bridge.call.return_value = {
                "session_id": "S",
                "bridge_version": __version__.split("a")[0],
            }
        else:
            bridge.call.side_effect = JevError("wrong_project", "Secret private diagnostic")
        bridges.append(bridge)
        return bridge

    report = await check_profiles(path, factory)
    assert not report["all_connected"]
    assert report["profiles"][0]["versions_match"]
    assert report["profiles"][1]["error_code"] == "wrong_project"
    assert "Secret" not in json.dumps(report) and "x" * 40 not in json.dumps(report)
    assert [c.bridge_url for c in configs] == [p["bridge_url"] for p in profiles]
    for bridge in bridges:
        bridge.call.assert_awaited_once_with("status")
        bridge.close.assert_awaited_once()
