import json
import re
from pathlib import Path

import pytest
from mcp.server.fastmcp import FastMCP

from jev_unreal.recipes import get_recipe, recipe_catalog, register_recipe_resources


@pytest.mark.parametrize("locale", ["en", "fr", "es"])
def test_localized_recipes_keep_same_tools_and_no_permission_grant(locale):
    catalog = recipe_catalog(locale)
    assert len(catalog["recipes"]) == 8
    assert catalog["executes_tools"] is False
    for recipe in catalog["recipes"]:
        source = get_recipe(recipe["recipe_id"])
        assert tuple(recipe["tool_sequence"]) == source.tool_sequence
        assert recipe["permission_note"]
        assert recipe["required_evidence"]
        assert recipe["prerequisite"]
        assert recipe["executes_tools"] is False
        if locale != "en":
            assert recipe["title"] != source.title
            assert recipe["translation_status"] == "draft_requires_human_review"


@pytest.mark.parametrize(
    ("recipe", "locale"),
    [("arbitrary_execution", "en"), ("move_actor", "../../secret"), ("move_actor", "de")],
)
def test_unknown_recipe_or_locale_cannot_select_external_content(recipe, locale):
    with pytest.raises(ValueError, match="exact published"):
        get_recipe(recipe, locale)


async def test_recipe_resources_and_prompt_are_callable_without_editor_or_provider():
    server = FastMCP("recipe fixture")
    register_recipe_resources(server)
    assert await server.list_tools() == []
    resources = await server.list_resources()
    assert [str(resource.uri) for resource in resources] == ["jev://recipes"]
    resource = await server.read_resource("jev://recipes/fr")
    catalog = json.loads(list(resource)[0].content)
    assert catalog["locale"] == "fr"
    single = await server.read_resource("jev://recipes/es/move_actor")
    assert json.loads(list(single)[0].content)["recipe_id"] == "move_actor"
    prompt = await server.get_prompt(
        "beginner_workflow", {"recipe_id": "move_actor", "locale": "es"}
    )
    assert "unreal_apply" in prompt.messages[0].content.text
    assert "draft_requires_human_review" in prompt.messages[0].content.text


def po_records(path):
    records = []
    for block in path.read_text(encoding="utf-8").split("\n\n"):
        fields = {}
        for line in block.splitlines():
            for field in ("msgctxt", "msgid", "msgstr"):
                if line.startswith(field + " "):
                    fields[field] = json.loads(line[len(field) + 1 :])
        if fields.get("msgctxt"):
            records.append(fields)
    return records


@pytest.mark.parametrize("culture", ["fr", "es"])
def test_native_translations_match_current_source_and_preserve_format_arguments(culture):
    root = Path(__file__).resolve().parents[1] / "Plugins/JevEditor"
    source = (root / "Source/JevEditor/Private/JevEditorReviewPanel.cpp").read_text(
        encoding="utf-8"
    )
    current = {
        key: json.loads('"' + value + '"')
        for key, value in re.findall(r'LOCTEXT\("([^"]+)",\s*"((?:[^"\\]|\\.)*)"\)', source)
    }
    records = po_records(root / "Localization/JevEditor" / culture / "JevEditor.po")
    assert len(records) == 60
    identities = set()
    for record in records:
        namespace, key = record["msgctxt"].split(",", 1)
        assert namespace == "JevEditorReviewPanel"
        assert key not in identities
        identities.add(key)
        assert record["msgid"] == current[key], f"stale translated source: {culture}/{key}"
        assert record["msgstr"]
        assert set(re.findall(r"\{[^}]+\}", record["msgid"])) == set(
            re.findall(r"\{[^}]+\}", record["msgstr"])
        )


def test_pipeline_is_source_only_and_does_not_skip_translation_source_checks():
    root = Path(__file__).resolve().parents[1] / "Plugins/JevEditor"
    config = (root / "Config/Localization/JevEditor.ini.template").read_text(encoding="utf-8")
    assert "GatherTextFromSource" in config
    assert "GatherTextFromAssets" not in config
    assert "bSkipSourceCheck=false" in config
    descriptor = json.loads((root / "JevEditor.uplugin").read_text(encoding="utf-8"))
    assert descriptor["LocalizationTargets"] == [{"Name": "JevEditor", "LoadingPolicy": "Editor"}]
