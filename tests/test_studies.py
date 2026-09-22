"""Synthetic fixtures for study bookkeeping; these are not workflow measurements."""

import copy
import hashlib
import json
from collections import Counter
from pathlib import Path

import pytest
from pydantic import ValidationError

from jev_unreal import studies
from jev_unreal.benchmarks import fingerprint
from jev_unreal.errors import JevError


@pytest.fixture
def study(tmp_path):
    files = {}

    def artifact(name, text):
        path = tmp_path / name
        data = text.encode()
        path.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        files[digest] = str(path)
        return digest

    protocol = {
        "id": "synthetic-contract-fixture",
        "provenance": "authored_synthetic_pilot",
        "seed": 43,
        "participants": ["agent-a", "agent-b"],
        "repetitions": 1,
        "tasks": [
            {
                "id": "translate-actor",
                "initial_state_sha256": artifact("state", "fixture state"),
                "acceptance_criteria_sha256": artifact("criteria", "fixture pass criterion"),
                "goal_sha256": artifact("goal", "fixture move goal"),
                "timeout_seconds": 60,
                "criteria": [{"id": "transform", "evaluation": "automated"}],
            }
        ],
        "methods": [
            {
                "method": method,
                "implementation": "fixture-v1",
                "configuration_sha256": artifact(method, f"fixture config {method}"),
            }
            for method in studies.METHODS
        ],
        "environment_sha256": artifact("environment", "fixture environment"),
        "allowed_tools_sha256": artifact("tools", "fixture allowed tools"),
        "intervention_policy_sha256": artifact("policy", "fixture policy"),
    }
    trace = artifact("trace", "synthetic trace: not actual workflow evidence")
    manifest = studies.create_manifest(protocol)
    observations = []
    for trial in manifest["trials"]:
        method = next(item for item in protocol["methods"] if item["method"] == trial["method"])
        observations.append(
            {
                "trial_id": trial["trial_id"],
                "attempt_id": f"attempt-{trial['sequence']}",
                "method": trial["method"],
                "initial_state_sha256": protocol["tasks"][0]["initial_state_sha256"],
                "acceptance_criteria_sha256": protocol["tasks"][0]["acceptance_criteria_sha256"],
                "configuration_sha256": method["configuration_sha256"],
                **{
                    field: protocol[field]
                    for field in (
                        "environment_sha256",
                        "allowed_tools_sha256",
                        "intervention_policy_sha256",
                    )
                },
                "status": "passed",
                "elapsed_seconds": 20,
                "correction_seconds": 2,
                "failure_count": 0,
                "tool_call_count": 4,
                "total_cost_usd": None,
                "cost_source": None,
                "cost_evidence_sha256": None,
                "trace_sha256": trace,
                "evidence_kind": "automated_observation",
                "criteria": [
                    {
                        "criterion_id": "transform",
                        "status": "passed",
                        "evidence_kind": "automated_observation",
                        "evidence_sha256": trace,
                    }
                ],
            }
        )
    return protocol, manifest, observations, files


def test_manifest_is_deterministic_counterbalanced_and_has_unique_trials(study):
    protocol = copy.deepcopy(study[0])
    protocol["participants"] = [f"participant-{i}" for i in range(6)]
    manifest = studies.create_manifest(protocol)
    assert manifest == studies.create_manifest(protocol)
    assert len({trial["trial_id"] for trial in manifest["trials"]}) == 18
    for position in range(1, 4):
        counts = Counter(
            trial["method"]
            for trial in manifest["trials"]
            if trial["position_in_block"] == position
        )
        assert counts == {"jev": 2, "keyword": 2, "direct_agent": 2}
    orders = [
        tuple(trial["method"] for trial in manifest["trials"][i : i + 3]) for i in range(0, 18, 3)
    ]
    assert len(set(orders)) == 6
    protocol["seed"] += 1
    assert studies.create_manifest(protocol) != manifest


def test_automated_pilot_does_not_claim_human_review_or_independence(study):
    _, manifest, observations, files = study
    report = studies.score_study(manifest, observations, evidence_files=files)
    assert report["provenance_declared"] == "authored_synthetic_pilot"
    assert report["independence_verified"] is False
    assert report["human_review_verified"] is False
    assert report["trial_schedule_verified"]
    for method in report["methods"].values():
        assert method["planned"] == method["passed"] == 2
        assert method["completion_rate"] == 1
        assert method["reported_total_cost_usd"] is None
        assert method["cost_missing"] == 2
        assert not method["total_cost_complete"]
        assert method["evidence_kinds"] == {"automated_observation": 2}
    serialized = json.dumps(report)
    assert all(path not in serialized for path in files.values())
    assert "synthetic trace:" not in serialized


def test_missing_and_failed_observations_remain_in_denominators(study):
    _, manifest, observations, files = study
    observed = [row for row in observations if row["method"] != "jev"]
    row = next(row for row in observed if row["method"] == "keyword")
    row["status"], row["failure_count"] = "failed", 2
    row["criteria"][0]["status"] = "failed"
    report = studies.score_study(manifest, observed, evidence_files=files)
    assert report["methods"]["jev"]["missing"] == 2
    assert report["methods"]["jev"]["completion_rate"] == 0
    assert report["methods"]["keyword"]["completion_rate"] == 0.5
    assert report["methods"]["keyword"]["observed_failure_events"] == 2
    assert report["methods"]["keyword"]["observed_elapsed_seconds_total"] == 40
    paired = report["paired_comparisons"]
    assert paired[0]["observed_pairs"] == 2
    assert paired[0]["left_only_passed"] == 1
    assert paired[1]["missing_pairs"] == 2
    assert paired[1]["metrics"]["elapsed_seconds"]["mean_delta"] is None


def test_paired_delta_keeps_failed_time_and_missing_cost_unknown(study):
    _, manifest, observations, files = study
    for row in observations:
        if row["method"] == "jev":
            row["elapsed_seconds"] = 40
            row["status"], row["failure_count"] = "failed", 1
            row["criteria"][0]["status"] = "failed"
        if row["method"] == "direct_agent":
            row["total_cost_usd"] = 1
            row["cost_source"] = "provider_metered"
            row["cost_evidence_sha256"] = row["trace_sha256"]
    report = studies.score_study(manifest, observations, evidence_files=files)
    pair = next(
        pair
        for pair in report["paired_comparisons"]
        if pair["left"] == "direct_agent" and pair["right"] == "jev"
    )
    assert pair["metrics"]["elapsed_seconds"]["mean_delta"] == 20
    assert pair["metrics"]["elapsed_seconds"]["participant_cluster_bootstrap_95"] == [20, 20]
    assert pair["metrics"]["total_cost_usd"]["paired_observations"] == 0
    assert pair["metrics"]["total_cost_usd"]["mean_delta"] is None
    assert report["methods"]["direct_agent"]["total_cost_complete"]
    assert report["methods"]["direct_agent"]["reported_total_cost_usd"] == 2


def test_single_observed_participant_has_no_bootstrap_interval(study):
    _, manifest, observations, files = study
    ids = {
        trial["trial_id"] for trial in manifest["trials"] if trial["participant_id"] == "agent-a"
    }
    report = studies.score_study(
        manifest, [row for row in observations if row["trial_id"] in ids], evidence_files=files
    )
    metric = report["paired_comparisons"][0]["metrics"]["elapsed_seconds"]
    assert metric["participants"] == 1
    assert metric["participant_cluster_bootstrap_95"] is None


@pytest.mark.parametrize(
    "field",
    [
        "initial_state_sha256",
        "acceptance_criteria_sha256",
        "configuration_sha256",
        "environment_sha256",
        "allowed_tools_sha256",
        "intervention_policy_sha256",
    ],
)
def test_changed_state_or_controls_cannot_be_imported(study, field):
    _, manifest, observations, files = study
    observations[0][field] = "f" * 64
    with pytest.raises(JevError, match="differs from registration"):
        studies.score_study(manifest, observations, evidence_files=files)


@pytest.mark.parametrize("change", ["method", "trial", "duplicate_trial", "duplicate_attempt"])
def test_unplanned_or_duplicate_attempts_cannot_replace_failures(study, change):
    _, manifest, observations, files = study
    if change == "method":
        observations[0]["method"] = next(
            m for m in studies.METHODS if m != observations[0]["method"]
        )
    elif change == "trial":
        observations[0]["trial_id"] = "unregistered"
    elif change == "duplicate_trial":
        observations.append(copy.deepcopy(observations[0]))
    else:
        observations[0]["attempt_id"] = observations[1]["attempt_id"]
    with pytest.raises(JevError):
        studies.score_study(manifest, observations, evidence_files=files)


@pytest.mark.parametrize("change", ["order", "task", "hash"])
def test_edited_trial_manifest_is_rejected(study, change):
    _, manifest, observations, files = study
    if change == "order":
        manifest["trials"].reverse()
    elif change == "task":
        manifest["trials"][0]["task_id"] = "different-task"
    else:
        manifest["protocol_sha256"] = "f" * 64
    with pytest.raises(JevError, match="frozen trial"):
        studies.score_study(manifest, observations, evidence_files=files)


@pytest.mark.parametrize("change", ["missing", "extra", "changed"])
def test_evidence_files_are_required_and_checked(study, change, tmp_path):
    _, manifest, observations, files = study
    digest = next(iter(files))
    if change == "missing":
        files.pop(digest)
    elif change == "extra":
        path = tmp_path / "unrelated"
        path.write_text("unrelated")
        files["f" * 64] = str(path)
    else:
        Path(files[digest]).write_text("tampered evidence")
    with pytest.raises(JevError):
        studies.score_study(manifest, observations, evidence_files=files)


def test_automation_cannot_pass_a_human_acceptance_criterion(study):
    protocol, _, observations, files = study
    protocol["tasks"][0]["criteria"][0]["evaluation"] = "human"
    manifest = studies.create_manifest(protocol)
    # Map the same synthetic records onto the newly preregistered human-criteria trials.
    for trial, row in zip(manifest["trials"], observations, strict=True):
        row["trial_id"] = trial["trial_id"]
    with pytest.raises(JevError, match="human acceptance"):
        studies.score_study(manifest, observations, evidence_files=files)
    for row in observations:
        row["criteria"][0]["evidence_kind"] = "human_attested"
        row["criteria"][0]["reviewer_id"] = "synthetic-reviewer"
    report = studies.score_study(manifest, observations, evidence_files=files)
    assert not report["human_review_verified"]


@pytest.mark.parametrize("change", ["deadline", "criterion", "criterion_missing"])
def test_pass_requires_every_registered_criterion_within_deadline(study, change):
    _, manifest, observations, files = study
    if change == "deadline":
        observations[0]["elapsed_seconds"] = 61
    elif change == "criterion":
        observations[0]["criteria"][0]["status"] = "not_measured"
    else:
        observations[0]["criteria"][0]["criterion_id"] = "different-criterion"
    with pytest.raises((JevError, ValidationError)):
        studies.score_study(manifest, observations, evidence_files=files)


@pytest.mark.parametrize(
    "patch",
    [
        {"total_cost_usd": 2},
        {"reviewer_id": "invented"},
        {"evidence_kind": "human_attested"},
        {"correction_seconds": 21},
        {"elapsed_seconds": float("nan")},
        {"tool_call_count": True},
        {"status": "failed"},
        {"arbitrary_field": "unexpected"},
    ],
)
def test_observation_contract_rejects_inconsistent_or_coerced_values(study, patch):
    row = {**study[2][0], **patch}
    with pytest.raises(ValidationError):
        studies.StudyObservation.model_validate(row)


def test_cli_roundtrip_exclusive_output_and_payload_free_errors(study, tmp_path, capsys):
    protocol, manifest, observations, files = study
    protocol_file, manifest_file = tmp_path / "protocol.json", tmp_path / "manifest.json"
    protocol_file.write_text(json.dumps(protocol))
    assert studies.main(["plan", str(protocol_file), "--output", str(manifest_file)]) == 0
    assert json.loads(manifest_file.read_text()) == manifest
    observation_file, evidence_file, report_file = (
        tmp_path / "observations.json",
        tmp_path / "evidence.json",
        tmp_path / "report.json",
    )
    observation_file.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "manifest_sha256": fingerprint(manifest),
                "observations": observations,
            }
        )
    )
    evidence_file.write_text(json.dumps(files))
    command = [
        "score",
        str(manifest_file),
        str(observation_file),
        "--evidence-map",
        str(evidence_file),
        "--output",
        str(report_file),
    ]
    assert studies.main(command) == 0
    before = report_file.read_bytes()
    assert studies.main(command) == 2
    assert report_file.read_bytes() == before
    output = capsys.readouterr()
    assert str(tmp_path) not in output.out + output.err
    assert "fixture state" not in output.out + output.err


def test_protocol_rejects_unbounded_schedule_or_repeated_method(study):
    protocol = study[0]
    protocol["methods"][0]["method"] = "jev"
    with pytest.raises(ValidationError):
        studies.create_manifest(protocol)
    protocol["methods"][0]["method"] = "direct_agent"
    protocol["participants"] = [f"participant-{i}" for i in range(32)]
    protocol["repetitions"] = 20
    protocol["tasks"] = [{**protocol["tasks"][0], "id": f"task-{i}"} for i in range(3)]
    with pytest.raises(ValidationError, match="4096"):
        studies.create_manifest(protocol)


def test_schema_generation_is_available_without_private_inputs(capsys):
    assert studies.main(["schema", "observations"]) == 0
    schema = json.loads(capsys.readouterr().out)
    assert "observations" in schema["properties"]
    assert schema["additionalProperties"] is False


def test_public_format_example_contains_no_measured_trials():
    root = Path(__file__).resolve().parents[1]
    example = root / "examples/benchmarks/study-format"
    manifest = studies.create_manifest(json.loads((example / "protocol.json").read_text()))
    observations = json.loads((example / "empty-observations.json").read_text())
    assert observations["manifest_sha256"] == fingerprint(manifest)
    assert observations["observations"] == []
    files = {
        digest: root / path
        for digest, path in json.loads((example / "evidence-map.json").read_text()).items()
    }
    report = studies.score_study(manifest, [], evidence_files=files)
    assert all(
        method["observed"] == 0 and method["missing"] == 2 for method in report["methods"].values()
    )


def test_evidence_directory_is_rejected_before_open(study, tmp_path):
    _, manifest, observations, files = study
    files[next(iter(files))] = str(tmp_path)
    with pytest.raises(JevError, match="regular local"):
        studies.score_study(manifest, observations, evidence_files=files)
