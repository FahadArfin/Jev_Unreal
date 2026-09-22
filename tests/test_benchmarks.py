import copy
import json
from dataclasses import replace

import httpx
import pytest
from pydantic import ValidationError

from jev_unreal.benchmarks import (
    MAX_FILE_BYTES,
    AnswerKey,
    AnswerLabel,
    BenchmarkDataset,
    RoutingObservation,
    RoutingRun,
    WorkflowObservation,
    compare_runs,
    fingerprint,
    hash_evidence,
    import_direct_agent_run,
    import_workflow_outcomes,
    load_answer_key,
    load_dataset,
    main,
    read_json,
    run_jev,
    run_keyword,
    score_routing,
    validate_run,
)
from jev_unreal.config import Settings
from jev_unreal.decision import DecisionClient
from jev_unreal.errors import JevError


@pytest.fixture
def dataset_dict():
    return {
        "id": "test-fixture",
        "provenance": "public_synthetic",
        "catalog": [
            {"id": "inspect", "description": "Inspect current selected actors and project."},
            {"id": "capture", "description": "Capture a rendered picture of the scene."},
        ],
        "cases": [
            {
                "id": "training",
                "split": "train",
                "group_id": "train-family",
                "goal": "Which actors are selected currently?",
            },
            {
                "id": "development",
                "split": "dev",
                "group_id": "dev-family",
                "goal": "Render a picture of my scene.",
            },
            {
                "id": "h1",
                "split": "heldout",
                "group_id": "inspect-family",
                "goal": "Inspect selected actors.",
            },
            {
                "id": "h2",
                "split": "heldout",
                "group_id": "defer-family",
                "goal": "Unsupported zzzzzz action.",
            },
        ],
    }


@pytest.fixture
def dataset(dataset_dict):
    return BenchmarkDataset.model_validate(dataset_dict)


@pytest.fixture
def key(dataset):
    return AnswerKey(
        dataset_sha256=fingerprint(dataset),
        labels=[
            AnswerLabel(case_id="training", accepted_tools=["inspect"], expect_defer=False),
            AnswerLabel(case_id="development", accepted_tools=["capture"], expect_defer=False),
            AnswerLabel(case_id="h1", accepted_tools=["inspect"], expect_defer=False),
            AnswerLabel(case_id="h2", expect_defer=True),
        ],
    )


def direct_data(dataset):
    data = run_keyword(dataset).model_dump()
    data.update(
        method="direct_agent",
        implementation="external-agent-v1",
        human_reviewed=True,
        reviewer_id="reviewer-1",
        evidence_sha256="a" * 64,
    )
    for row in data["observations"]:
        row.update(cost_usd=None, cost_source=None)
    return data


def workflow_data(dataset, run):
    return {
        "dataset_sha256": fingerprint(dataset),
        "routing_run_sha256": fingerprint(run),
        "human_reviewed": True,
        "reviewer_id": "reviewer-1",
        "observations": [
            {
                "case_id": "h1",
                "status": "passed",
                "completion_seconds": 30.0,
                "correction_seconds": 10.0,
                "failure_count": 1,
                "tool_call_count": 4,
                "total_cost_usd": None,
                "evidence_sha256": "b" * 64,
            }
        ],
    }


def test_keyword_and_score_are_real_routing_only(dataset, key):
    run = run_keyword(dataset)
    assert [row.case_id for row in run.observations] == ["h1", "h2"]
    assert [(row.outcome, row.selected) for row in run.observations] == [
        ("recommend", "inspect"),
        ("defer", None),
    ]
    report = score_routing(dataset, key, run)
    assert report["routing"]["correct"] == 2
    assert report["routing"]["coverage"] == 0.5
    assert report["routing"]["correct_abstentions"] == 1
    assert report["routing"]["cost_usd"]["reported_sum"] == 0
    assert report["workflow"]["scope"] == "not_measured"
    assert report["workflow"]["completion_rate_over_all_cases"] is None
    assert all(row.latency_ms >= 0 for row in run.observations)


def test_keyword_ties_abstain_without_using_catalog_order(dataset_dict):
    dataset_dict["catalog"][1]["description"] = dataset_dict["catalog"][0]["description"]
    run = run_keyword(BenchmarkDataset.model_validate(dataset_dict))
    assert run.observations[0].outcome == "defer"


@pytest.mark.parametrize(
    "mutation",
    [
        lambda d: d.update(schema_version=True),
        lambda d: d.update(schema_version="1"),
        lambda d: d.update(labels={"h1": "inspect"}),
        lambda d: d["catalog"].append(d["catalog"][0]),
        lambda d: d["catalog"][0].update(id="__defer__"),
        lambda d: d["cases"].append(d["cases"][0]),
        lambda d: d["cases"][0].update(split="test"),
        lambda d: d["cases"][2].update(goal="  which ACTORS are selected currently?  "),
        lambda d: d["cases"][2].update(group_id="train-family"),
        lambda d: d["cases"][0].update(expected_tool="inspect"),
        lambda d: d["cases"][0].update(goal="\x00bad"),
        lambda d: d["cases"][0].update(goal=" " * 4),
        lambda d: d["cases"][0].update(goal="x" * 8001),
    ],
)
def test_dataset_rejects_label_leaks_duplicates_and_cross_split_groups(dataset_dict, mutation):
    mutation(dataset_dict)
    with pytest.raises(ValidationError):
        BenchmarkDataset.model_validate(dataset_dict)


@pytest.mark.parametrize(
    "label",
    [
        {"case_id": "a", "accepted_tools": [], "expect_defer": False},
        {"case_id": "a", "accepted_tools": ["inspect"], "expect_defer": True},
        {"case_id": "a", "accepted_tools": ["__defer__"], "expect_defer": False},
        {"case_id": "a", "accepted_tools": ["inspect", "inspect"], "expect_defer": False},
        {"case_id": "a", "accepted_tools": ["inspect"], "expect_defer": 1},
    ],
)
def test_answer_defer_semantics_are_strict(label):
    with pytest.raises(ValidationError):
        AnswerLabel.model_validate(label)


def test_canonical_hash_ignores_object_key_order_but_binds_catalog_and_goals(dataset_dict):
    dataset = BenchmarkDataset.model_validate(dataset_dict)
    assert fingerprint(dataset) == fingerprint(dataset.model_dump())
    assert fingerprint({"b": 2, "a": 1}) == fingerprint({"a": 1, "b": 2})
    dataset_dict["catalog"][0]["description"] += " New guidance."
    assert fingerprint(dataset) != fingerprint(BenchmarkDataset.model_validate(dataset_dict))


@pytest.mark.parametrize("change", ["wrong_hash", "missing", "foreign_tool", "duplicate"])
def test_scoring_requires_complete_matching_valid_labels(dataset, key, change):
    data = key.model_dump()
    if change == "wrong_hash":
        data["dataset_sha256"] = "0" * 64
    elif change == "missing":
        data["labels"].pop()
    elif change == "foreign_tool":
        data["labels"][0]["accepted_tools"] = ["delete_all"]
    else:
        data["labels"].append(data["labels"][0])
    with pytest.raises((JevError, ValidationError)):
        score_routing(dataset, AnswerKey.model_validate(data), run_keyword(dataset))


@pytest.mark.parametrize("change", ["missing", "duplicate", "hash", "input", "foreign", "split"])
def test_scoring_rejects_cherry_picking_and_wrong_inputs(dataset, key, change):
    data = run_keyword(dataset).model_dump()
    if change == "missing":
        data["observations"].pop()
    elif change == "duplicate":
        data["observations"].append(data["observations"][0])
    elif change == "hash":
        data["catalog_sha256"] = "0" * 64
    elif change == "input":
        data["observations"][0]["input_sha256"] = "0" * 64
    elif change == "foreign":
        data["observations"][0]["selected"] = "delete_all"
    else:
        data["split"] = "dev"
    with pytest.raises((JevError, ValidationError)):
        score_routing(dataset, key, RoutingRun.model_validate(data))


@pytest.mark.parametrize(
    "change",
    [
        {"outcome": "recommend", "selected": None},
        {"outcome": "defer", "selected": "inspect"},
        {"outcome": "error", "error_code": None},
        {"outcome": "defer", "error_code": "agent_error"},
        {"latency_ms": float("nan")},
        {"cost_usd": float("inf")},
        {"cost_usd": -1.0},
        {"cost_usd": True},
        {"latency_ms": "1.0"},
        {"cost_usd": 1.0, "cost_source": None},
        {"cost_usd": None, "cost_source": "human_reported"},
        {"cost_usd": 1.0, "cost_source": "local_no_provider"},
        {"raw_provider_payload": "never allowed"},
    ],
)
def test_observation_types_cannot_forge_safe_results(change):
    data = {"case_id": "h1", "input_sha256": "a" * 64, "outcome": "defer", **change}
    with pytest.raises(ValidationError):
        RoutingObservation.model_validate(data)


@pytest.mark.parametrize(
    "field,value",
    [
        ("human_reviewed", False),
        ("human_reviewed", 1),
        ("reviewer_id", None),
        ("evidence_sha256", None),
        ("evidence_sha256", "not-a-hash"),
    ],
)
def test_direct_agent_import_requires_reviewed_evidence(dataset, field, value):
    data = direct_data(dataset)
    data[field] = value
    with pytest.raises(ValidationError):
        import_direct_agent_run(dataset, data)


def test_imported_direct_agent_errors_are_not_counted_as_correct_defer(dataset, key):
    data = direct_data(dataset)
    data["observations"][1].update(outcome="error", error_code="agent_error")
    run = import_direct_agent_run(dataset, data)
    report = score_routing(dataset, key, run)
    assert report["routing"]["accuracy_over_all_cases"] == 0.5
    assert report["routing"]["correct_abstentions"] == 0
    assert report["routing"]["errors"] == 1
    assert report["routing"]["cost_usd"]["reported_sum"] is None
    assert report["workflow"]["scope"] == "not_measured"


def test_workflow_observations_do_not_replace_routing_metrics(dataset, key):
    run = import_direct_agent_run(dataset, direct_data(dataset))
    outcomes = import_workflow_outcomes(dataset, run, workflow_data(dataset, run))
    report = score_routing(dataset, key, run, outcomes=outcomes)
    assert report["routing"]["accuracy_over_all_cases"] == 1.0
    assert report["workflow"]["completion_rate_over_all_cases"] == 0.5
    assert report["workflow"]["missing"] == 1
    assert report["workflow"]["correction_seconds"]["reported_sum"] == 10.0
    assert report["workflow"]["correction_seconds"]["missing_count"] == 1
    assert report["workflow"]["failure_events"] == 1
    assert report["workflow"]["total_workflow_cost_usd"]["reported_sum"] is None


@pytest.mark.parametrize("change", ["run_hash", "dataset_hash", "foreign", "duplicate"])
def test_workflow_evidence_is_bound_to_exact_run(dataset, change):
    run = run_keyword(dataset)
    data = workflow_data(dataset, run)
    if change == "run_hash":
        data["routing_run_sha256"] = "0" * 64
    elif change == "dataset_hash":
        data["dataset_sha256"] = "0" * 64
    elif change == "foreign":
        data["observations"][0]["case_id"] = "development"
    else:
        data["observations"].append(data["observations"][0])
    with pytest.raises((ValidationError, JevError)):
        import_workflow_outcomes(dataset, run, data)


@pytest.mark.parametrize(
    "change",
    [
        {"completion_seconds": -1.0},
        {"correction_seconds": 31.0},
        {"failure_count": True},
        {"failure_count": -1},
        {"tool_call_count": 1.5},
        {"status": "passed", "evidence_sha256": None},
        {"status": "failed", "failure_count": 0},
        {"completion_seconds": float("nan")},
    ],
)
def test_workflow_evidence_times_and_counters_are_bounded(dataset, change):
    data = workflow_data(dataset, run_keyword(dataset))["observations"][0]
    data.update(change)
    with pytest.raises(ValidationError):
        WorkflowObservation.model_validate(data)


def test_comparison_requires_matching_complete_partitions_and_no_speedup_claim(dataset, key):
    keyword = run_keyword(dataset)
    direct = import_direct_agent_run(dataset, direct_data(dataset))
    report = compare_runs(dataset, key, [keyword, direct])
    assert report["efficiency_conclusion"] is None
    assert len(report["reports"]) == 2
    with pytest.raises(JevError):
        compare_runs(dataset, key, [keyword, keyword])
    with pytest.raises(JevError):
        compare_runs(dataset, key, [keyword, run_keyword(dataset, "dev")])
    with pytest.raises(JevError):
        compare_runs(dataset, key, [keyword, direct], outcomes={"0" * 64: None})


def provider_response(choice="inspect", cost=0.001):
    result = {
        "model": "typesafe/test-model",
        "answers": {
            "route": {
                "type": "choice",
                "choice": choice,
                "confidence": 0.98,
                "probabilities": {
                    tool: 0.98 if tool == choice else 0.01
                    for tool in ("inspect", "capture", "__defer__")
                },
            }
        },
        "raw_secret_payload": "DO_NOT_STORE",
    }
    if cost is not None:
        result["usage"] = {"cost": cost, "input_tokens": 10, "output_tokens": 2}
    return result


async def test_live_harness_uses_decisions_and_never_sends_labels_split_or_case_ids(dataset, key):
    captured = []

    def handle(request):
        assert request.url.path == "/api/alpha/decisions"
        body = json.loads(request.content)
        captured.append(body)
        choice = "inspect" if body["state"]["goal"].startswith("Inspect") else "__defer__"
        return httpx.Response(200, json=provider_response(choice))

    client = DecisionClient(
        Settings(api_key="private-test-secret", max_requests=2, cache_seconds=0),
        transport=httpx.MockTransport(handle),
    )
    try:
        run = await run_jev(dataset, client, request_budget=2)
    finally:
        await client.close()
    assert len(captured) == 2
    for request in captured:
        assert set(request["state"]) == {"goal"}
        assert set(request) == {"model", "state", "questions"}
        assert "accepted_tools" not in json.dumps(request)
        assert "heldout" not in json.dumps(request)
    report = score_routing(dataset, key, run)
    assert report["routing"]["correct"] == 2
    assert report["routing"]["cost_usd"]["reported_sum"] == 0.002
    encoded = json.dumps(run.model_dump())
    assert "DO_NOT_STORE" not in encoded and "private-test-secret" not in encoded
    assert "probabilities" not in encoded and "answers" not in encoded


async def test_live_failures_are_observations_and_missing_cost_is_not_zero(dataset, key):
    calls = []

    def handle(request):
        calls.append(request)
        if len(calls) == 1:
            return httpx.Response(500, text="private provider diagnostic")
        return httpx.Response(200, json=provider_response("__defer__", cost=None))

    client = DecisionClient(
        Settings(api_key="test", max_requests=2, cache_seconds=0),
        transport=httpx.MockTransport(handle),
    )
    try:
        run = await run_jev(dataset, client, request_budget=2)
    finally:
        await client.close()
    assert len(calls) == 2
    report = score_routing(dataset, key, run)
    assert report["routing"]["errors"] == 1
    assert report["routing"]["cost_usd"]["reported_sum"] is None
    assert report["routing"]["cost_usd"]["missing_count"] == 2
    assert "private provider diagnostic" not in json.dumps(run.model_dump())


@pytest.mark.parametrize("budget", [0, 13, True, 1.5, "2", 1])
async def test_request_budget_is_enforced_before_any_call(dataset, budget):
    client = DecisionClient(Settings(api_key="test", max_requests=2, cache_seconds=0))
    try:
        with pytest.raises(JevError):
            await run_jev(dataset, client, request_budget=budget)
        assert client.requests == 0
    finally:
        await client.close()


@pytest.mark.parametrize(
    "change",
    [
        {"cache_seconds": 60},
        {"max_requests": 100},
        {"max_requests": 1},
        {"model": "sk-test-secret"},
        {"model": "evil\nmodel"},
    ],
)
async def test_live_client_configuration_preflight(dataset, change):
    settings = replace(Settings(api_key="test", max_requests=2, cache_seconds=0), **change)
    client = DecisionClient(settings)
    try:
        with pytest.raises(JevError):
            await run_jev(dataset, client, request_budget=2)
        assert client.requests == 0
    finally:
        await client.close()


async def test_private_project_inputs_require_explicit_cloud_opt_in(dataset_dict):
    dataset_dict["provenance"] = "real_project"
    dataset = BenchmarkDataset.model_validate(dataset_dict)
    client = DecisionClient(Settings(api_key="test", max_requests=2, cache_seconds=0))
    try:
        for opt_in in (False, 1, "yes"):
            with pytest.raises(JevError):
                await run_jev(dataset, client, request_budget=2, allow_private_inputs=opt_in)
        assert client.requests == 0
    finally:
        await client.close()


@pytest.mark.parametrize(
    "raw",
    [
        b'{"a":1,"a":2}',
        b'{"a":NaN}',
        b"[]",
        b"\xff",
        b'{"nested":{"a":1,"a":2}}',
        b"{" * 5000,
        b" " * (MAX_FILE_BYTES + 1),
    ],
    ids=["duplicate", "nan", "array", "encoding", "nested-duplicate", "depth", "oversize"],
)
def test_file_reader_rejects_ambiguous_unbounded_json_without_echoing_it(tmp_path, raw):
    path = tmp_path / "input.json"
    path.write_bytes(raw)
    with pytest.raises(JevError) as error:
        read_json(path)
    assert str(path) not in str(error.value)


def test_cli_local_baseline_and_scoring(dataset, key, tmp_path, capsys):
    dataset_file, key_file = tmp_path / "dataset.json", tmp_path / "key.json"
    dataset_file.write_text(dataset.model_dump_json(), encoding="utf-8")
    key_file.write_text(key.model_dump_json(), encoding="utf-8")
    run_path, report_path = tmp_path / "run.json", tmp_path / "report.json"
    assert main(["validate", str(dataset_file), "--answer-key", str(key_file)]) == 0
    assert main(["keyword", str(dataset_file), "--output", str(run_path)]) == 0
    assert (
        main(
            ["score", str(dataset_file), str(key_file), str(run_path), "--output", str(report_path)]
        )
        == 0
    )
    report = json.loads(report_path.read_text())
    assert report["workflow"]["scope"] == "not_measured"
    assert load_dataset(dataset_file) == dataset
    assert load_answer_key(key_file, dataset) == key
    assert main(["validate", str(tmp_path / "missing.json")]) == 1
    assert "invalid_or_unavailable_benchmark_input" in capsys.readouterr().err


def test_public_fixture_fingerprint_and_partitions():
    dataset = load_dataset("examples/benchmarks/public-synthetic-v1.dataset.json")
    key = load_answer_key("examples/benchmarks/public-synthetic-v1.answers.json", dataset)
    assert {case.split for case in dataset.cases} == {"train", "dev", "heldout"}
    run = run_keyword(dataset)
    validate_run(dataset, run)
    assert 1 <= len(run.observations) <= 12
    assert score_routing(dataset, key, run)["workflow"]["scope"] == "not_measured"


def test_candidate_id_length_matches_provider_limit(dataset_dict):
    long_id = "x" * 128
    dataset_dict["catalog"][0]["id"] = long_id
    dataset = BenchmarkDataset.model_validate(dataset_dict)
    assert run_keyword(dataset).observations[0].selected == long_id


def test_outcomes_cannot_be_reassigned_to_different_run_timings(dataset, key):
    run = run_keyword(dataset)
    outcomes = import_workflow_outcomes(dataset, run, workflow_data(dataset, run))
    altered = copy.deepcopy(run.model_dump())
    altered["observations"][0]["latency_ms"] += 1.0
    with pytest.raises(JevError):
        score_routing(dataset, key, RoutingRun.model_validate(altered), outcomes=outcomes)


def test_evidence_file_import_checks_actual_bytes_and_keeps_review_attested(dataset, tmp_path):
    evidence = tmp_path / "reviewed-evidence.txt"
    evidence.write_bytes(b"Synthetic test evidence only: no real project acceptance.")
    digest = hash_evidence(evidence)
    data = direct_data(dataset)
    data["evidence_sha256"] = digest
    run = import_direct_agent_run(dataset, data, evidence_files={digest: evidence})
    outcome_data = workflow_data(dataset, run)
    outcome_data["observations"][0]["evidence_sha256"] = digest
    import_workflow_outcomes(dataset, run, outcome_data, evidence_files={digest: evidence})
    evidence.write_bytes(b"Changed evidence")
    with pytest.raises(JevError, match="SHA-256"):
        import_direct_agent_run(dataset, data, evidence_files={digest: evidence})
    with pytest.raises(JevError):
        import_workflow_outcomes(dataset, run, outcome_data, evidence_files={})
    assert str(evidence) not in run.model_dump_json()


@pytest.mark.parametrize("content", [b"", b"x" * (16 * 1024 * 1024 + 1)], ids=["empty", "oversize"])
def test_evidence_hash_requires_bounded_nonempty_file(tmp_path, content):
    evidence = tmp_path / "evidence.bin"
    evidence.write_bytes(content)
    with pytest.raises(JevError):
        hash_evidence(evidence)


def test_cli_preserves_existing_outputs_and_input_files(dataset, tmp_path, capsys):
    dataset_path = tmp_path / "dataset.json"
    dataset_path.write_text(dataset.model_dump_json(), encoding="utf-8")
    output = tmp_path / "run.json"
    output.write_text("previous-evidence", encoding="utf-8")
    assert main(["keyword", str(dataset_path), "--output", str(output)]) == 1
    assert output.read_text() == "previous-evidence"
    assert main(["keyword", str(dataset_path), "--output", str(output), "--overwrite"]) == 0
    assert json.loads(output.read_text())["method"] == "keyword"
    assert main(["keyword", str(dataset_path), "--output", str(dataset_path), "--overwrite"]) == 1
    assert load_dataset(dataset_path) == dataset
    assert "previous-evidence" not in capsys.readouterr().err


def test_existing_live_output_is_rejected_before_client_creation(dataset, tmp_path, monkeypatch):
    dataset_path = tmp_path / "dataset.json"
    dataset_path.write_text(dataset.model_dump_json(), encoding="utf-8")
    output = tmp_path / "existing.json"
    output.write_text("existing evidence", encoding="utf-8")

    def no_client(*args, **kwargs):
        raise AssertionError("Provider client must not be created")

    monkeypatch.setattr("jev_unreal.benchmarks.DecisionClient", no_client)
    assert main(["jev", str(dataset_path), "--request-budget", "2", "--output", str(output)]) == 1
