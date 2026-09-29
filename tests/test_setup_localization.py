"""Exact localization resource ownership using disposable synthetic source bundles."""

import json

import pytest

from jev_unreal import setup
from jev_unreal.errors import JevError


@pytest.fixture
def localized_source(tmp_path):
    project = tmp_path / "Project/Fixture.uproject"
    project.parent.mkdir()
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "5.8"}))
    source = tmp_path / "SourceBundle/JevEditor"
    source.mkdir(parents=True)
    (source / "JevEditor.uplugin").write_text(
        json.dumps(
            {
                "FileVersion": 3,
                "VersionName": "fixture",
                "Modules": [{"Name": "JevEditor", "Type": "Editor"}],
            }
        )
    )
    cpp = source / "Source/JevEditor/Fixture.cpp"
    cpp.parent.mkdir(parents=True)
    cpp.write_text("// synthetic source only")
    for relative in setup._RESOURCES - {"Resources/Icon128.png"}:
        resource = source / relative
        resource.parent.mkdir(parents=True, exist_ok=True)
        resource.write_bytes(b"synthetic localization fixture")
    return project, source


def test_exact_source_and_compiled_locale_resources_are_hash_owned(localized_source):
    project, source = localized_source
    unexpected = source / "Content/PrivateGameData.uasset"
    unexpected.write_bytes(b"do not copy")
    config = source / "Config/DefaultEngine.ini"
    config.write_text("do not activate")
    plan = setup.plan_install(project, source)
    assert setup._RESOURCES - {"Resources/Icon128.png"} <= set(plan["source_files"])
    assert "Content/PrivateGameData.uasset" not in plan["source_files"]
    assert "Config/DefaultEngine.ini" not in plan["source_files"]
    setup.apply_plan(plan)
    installed = project.parent / "Plugins/JevEditor"
    assert (installed / "Localization/JevEditor/fr/JevEditor.po").is_file()
    assert (installed / "Content/Localization/JevEditor/es/JevEditor.locres").is_file()
    assert not (installed / "Config/DefaultEngine.ini").exists()
    assert setup.apply_plan(setup.plan_uninstall(project))["status"] == "uninstalled"
    assert not (installed / "Localization/JevEditor/fr/JevEditor.po").exists()


def test_modified_translation_is_preserved_and_resource_changes_stale_plan(localized_source):
    project, source = localized_source
    plan = setup.plan_install(project, source)
    resource = source / "Localization/JevEditor/fr/JevEditor.po"
    resource.write_bytes(b"new reviewed source")
    with pytest.raises(JevError) as error:
        setup.apply_plan(plan)
    assert error.value.code == "stale_setup_plan"
    setup.apply_plan(setup.plan_install(project, source))
    installed = project.parent / "Plugins/JevEditor/Localization/JevEditor/fr/JevEditor.po"
    installed.write_bytes(b"user translation changes")
    assert setup.plan_install(project, source)["can_apply"] is False
    result = setup.apply_plan(setup.plan_uninstall(project))
    assert result["status"] == "partially_uninstalled"
    assert installed.read_bytes() == b"user translation changes"


@pytest.mark.parametrize(
    "path",
    [
        "Localization/JevEditor/ja/JevEditor.po",
        "Content/Localization/JevEditor/fr/Other.locres",
        "Config/DefaultEngine.ini",
        "Content/Localization/JevEditor/../private.locres",
        "Content/Localization/JevEditor/fr/evil.dll",
    ],
)
def test_resource_whitelist_cannot_expand_to_other_configs_assets_or_binaries(path):
    with pytest.raises(JevError):
        setup._relative(path)
