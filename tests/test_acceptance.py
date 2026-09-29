"""Synthetic report-contract fixtures; no human, clean-host or independent-study evidence."""

import copy
import hashlib
import json

import pytest

from jev_unreal.acceptance import TASKS, inspect_acceptance
from jev_unreal.benchmarks import fingerprint
from jev_unreal.errors import JevError
from jev_unreal.studies import create_manifest


@pytest.fixture
def report(tmp_path):
    evidence = tmp_path / "fixture.txt"
    evidence.write_bytes(b"synthetic fixture, not human acceptance")
    data = {
        "schema_version": 1,
        "report_id": "synthetic-report",
        "contexts": [
            {
                "id": "fixture",
                "project_kind": "synthetic_fixture",
                "attestor_id": "fixture-author",
                "attestation_evidence_id": "evidence",
                "attestations": {},
                "plugin_version": "test",
                "engine_version": "test",
                "operating_system": "synthetic",
            }
        ],
        "evidence": [
            {
                "id": "evidence",
                "relative_path": "fixture.txt",
                "sha256": hashlib.sha256(evidence.read_bytes()).hexdigest(),
            }
        ],
        "observations": [],
    }
    path = tmp_path / "acceptance.json"
    return path, data


def score(report):
    path, data = report
    path.write_text(json.dumps(data))
    return inspect_acceptance(path)


def observation(priority, task, context="fixture", outcome="passed", suffix=""):
    return {
        "attempt_id": f"{context}-{task}{suffix}",
        "priority": priority,
        "task_id": task,
        "context_id": context,
        "outcome": outcome,
        "elapsed_seconds": 1.0,
        "evidence_ids": ["evidence"],
    }


def test_empty_evidence_bundle_reports_all_six_priorities_incomplete(report):
    result = score(report)
    assert set(result["priorities"]) == set(TASKS)
    assert all(row["status"] == "incomplete" for row in result["priorities"].values())
    assert result["evidence_hashes_checked"] == 1
    assert result["all_priorities_attested_pass"] is False
    assert result["production_readiness_established"] is False


def test_automated_observations_do_not_become_human_acceptance(report):
    report[1]["observations"] = [observation("review", task) for task in TASKS["review"]]
    result = score(report)
    assert result["priorities"]["review"]["status"] == "attestation_incomplete"
    assert (
        "physical_keyboard"
        in result["priorities"]["review"]["attestation_gaps"]["fixture-keyboard_copy"]
    )
    assert result["human_acceptance_verified"] is False


def test_complete_human_claims_remain_explicit_unverified_attestations(report):
    context = report[1]["contexts"][0]
    context.update(
        display_scaling="150%",
        panel_width="400 px",
        input_device="physical keyboard",
        language="reviewed translation",
        assistive_software_version="fixture",
    )
    context["attestations"] = {
        name: True
        for name in (
            "human_observed",
            "participant_not_implementer",
            "physical_keyboard",
            "assistive_software",
            "reviewed_translation",
            "all_attempts_included",
        )
    }
    report[1]["observations"] = [observation("review", task) for task in TASKS["review"]]
    result = score(report)
    assert result["priorities"]["review"]["status"] == "attested_pass"
    assert result["human_acceptance_verified"] is False
    assert result["clean_host_verified"] is False


@pytest.mark.parametrize("outcome", ["failed", "blocked"])
def test_failed_and_blocked_attempts_remain_visible_after_a_passing_retry(report, outcome):
    report[1]["observations"] = [
        observation("review", "inspect_units", outcome=outcome),
        observation("review", "inspect_units", suffix="-retry"),
    ]
    result = score(report)["priorities"]["review"]
    assert result["status"] == outcome
    assert result["counts"][outcome] == result["counts"]["passed"] == 1
    assert len(result["attempts"]) == 2


@pytest.mark.parametrize(
    "value", ["../outside", "/absolute", "C:/private", "nested\\file", "./fixture.txt"]
)
def test_evidence_paths_cannot_escape_manifest_directory(report, value):
    report[1]["evidence"][0]["relative_path"] = value
    with pytest.raises(JevError):
        score(report)


def test_hash_changes_and_duplicate_attempts_fail_closed(report):
    report[1]["evidence"][0]["sha256"] = "0" * 64
    with pytest.raises(JevError):
        score(report)
    report[1]["evidence"][0]["sha256"] = hashlib.sha256(
        (report[0].parent / "fixture.txt").read_bytes()
    ).hexdigest()
    item = observation("review", "inspect_units")
    report[1]["observations"] = [item, item]
    with pytest.raises(JevError):
        score(report)


def test_installation_needs_both_project_kinds_and_every_declared_matrix_cell(report):
    base = report[1]["contexts"][0]
    base.update(python_version="fixture", compiler_version="fixture", windows_sdk_version="fixture")
    base["attestations"] = {
        "clean_windows_host": True,
        "licensed_engine": True,
        "all_attempts_included": True,
    }
    for kind in ("blueprint_only", "cpp"):
        context = copy.deepcopy(base) | {"id": kind, "project_kind": kind}
        report[1]["contexts"].append(context)
    report[1]["installation_matrix"] = ["blueprint_only", "cpp"]
    report[1]["observations"] = [
        observation("installation", task, "blueprint_only") for task in TASKS["installation"]
    ]
    result = score(report)["priorities"]["installation"]
    assert result["status"] == "incomplete"
    assert len(result["missing"]) == 6
    report[1]["observations"] += [
        observation("installation", task, "cpp") for task in TASKS["installation"]
    ]
    result = score(report)
    assert result["priorities"]["installation"]["status"] == "attested_pass"
    assert result["clean_host_verified"] is False


def test_actual_study_scorer_keeps_missing_trials_and_pilot_scope(report):
    path, data = report
    digest = data["evidence"][0]["sha256"]
    manifest = create_manifest(
        {
            "id": "fixture-study",
            "provenance": "authored_synthetic_pilot",
            "seed": 1,
            "participants": ["fixture"],
            "repetitions": 1,
            "tasks": [
                {
                    "id": "fixture-task",
                    "initial_state_sha256": digest,
                    "acceptance_criteria_sha256": digest,
                    "goal_sha256": digest,
                    "timeout_seconds": 60,
                    "criteria": [{"id": "fixture", "evaluation": "automated"}],
                }
            ],
            "methods": [
                {"method": method, "implementation": "fixture", "configuration_sha256": digest}
                for method in ("direct_agent", "keyword", "jev")
            ],
            "environment_sha256": digest,
            "allowed_tools_sha256": digest,
            "intervention_policy_sha256": digest,
        }
    )
    bundle = {"manifest_sha256": fingerprint(manifest), "observations": []}
    for name, content in (("study-manifest", manifest), ("study-observations", bundle)):
        file = path.parent / (name + ".json")
        file.write_text(json.dumps(content))
        data["evidence"].append(
            {
                "id": name,
                "relative_path": file.name,
                "sha256": hashlib.sha256(file.read_bytes()).hexdigest(),
            }
        )
    data["study"] = {
        "manifest_evidence_id": "study-manifest",
        "observations_evidence_ids": ["study-observations"],
        "evidence_map": {digest: "evidence"},
    }
    data["observations"] = [observation("study", "registered_workflow_study")]
    result = score(report)
    assert result["study_summary"]["methods"]["jev"]["missing"] == 1
    assert result["priorities"]["study"]["status"] == "incomplete"
    assert result["independence_verified"] is False


def test_unknown_task_and_false_pass_without_elapsed_time_are_rejected(report):
    item = observation("review", "invented-task")
    report[1]["observations"] = [item]
    with pytest.raises(JevError):
        score(report)
    item["task_id"] = "inspect_units"
    item["elapsed_seconds"] = None
    with pytest.raises(JevError):
        score(report)
