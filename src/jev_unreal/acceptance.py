"""Evidence-bound community acceptance reports; attestations are never external certification."""

from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator

from .benchmarks import fingerprint, hash_evidence, read_json
from .errors import JevError
from .studies import StudyObservations, score_study
from .team_policy import local_path

Priority = Literal["review", "installation", "validators", "blueprints", "gameplay", "study"]
Identifier = Annotated[str, Field(min_length=1, max_length=64, pattern=r"^[a-zA-Z0-9_-]+$")]
Digest = Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")]
Text = Annotated[str, Field(min_length=1, max_length=256, pattern=r"^[^\x00-\x1f\x7f]+$")]
TASKS = {
    "review": (
        "inspect_units",
        "preview_translation",
        "keyboard_copy",
        "apply_verify",
        "undo_receipt",
        "stale_recovery",
        "assistive_errors",
        "narrow_translation",
    ),
    "installation": (
        "prerequisites",
        "install_build_connect",
        "previous_release_upgrade",
        "modified_source_refusal",
        "interruption_recovery",
        "locked_file_exclusion",
    ),
    "validators": (
        "valid_asset",
        "invalid_asset",
        "not_applicable",
        "diagnostics",
        "dirty_state",
        "cancellation",
        "cleanup",
    ),
    "blueprints": ("known_good", "known_broken", "instance_effects", "dependent_asset_effects"),
    "gameplay": tuple(
        f"{recipe}_{test}"
        for recipe in ("door", "navigation", "interaction", "combat")
        for test in ("passing", "regression", "repeat", "cleanup")
    ),
    "study": ("registered_workflow_study",),
}


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)


class Evidence(Strict):
    id: Identifier
    relative_path: str = Field(min_length=1, max_length=512)
    sha256: Digest


class Attestations(Strict):
    human_observed: bool = False
    participant_not_implementer: bool = False
    physical_keyboard: bool = False
    assistive_software: bool = False
    reviewed_translation: bool = False
    clean_windows_host: bool = False
    licensed_engine: bool = False
    real_project: bool = False
    project_owned_criteria: bool = False
    independent_evaluation: bool = False
    all_attempts_included: bool = False


class Context(Strict):
    id: Identifier
    project_kind: Literal["blueprint_only", "cpp", "real_project", "synthetic_fixture"]
    attestor_id: Identifier
    attestation_evidence_id: Identifier
    attestations: Attestations
    plugin_version: Text
    engine_version: Text
    operating_system: Text
    python_version: Text | None = None
    compiler_version: Text | None = None
    windows_sdk_version: Text | None = None
    display_scaling: Text | None = None
    panel_width: Text | None = None
    input_device: Text | None = None
    language: Text | None = None
    assistive_software_version: Text | None = None


class Observation(Strict):
    attempt_id: Identifier
    priority: Priority
    task_id: Identifier
    context_id: Identifier
    outcome: Literal["passed", "failed", "blocked"]
    elapsed_seconds: float | None = Field(default=None, ge=0, le=86400)
    assistance_count: int = Field(default=0, ge=0, le=1000)
    evidence_ids: list[Identifier] = Field(min_length=1, max_length=8)
    note: str = Field(default="", max_length=2000)

    @model_validator(mode="after")
    def task_in_protocol(self):
        if self.task_id not in TASKS[self.priority]:
            raise ValueError("Task must belong to the selected community acceptance priority.")
        if self.outcome == "passed" and self.elapsed_seconds is None:
            raise ValueError("Passed observations require measured elapsed time.")
        return self


class StudyLink(Strict):
    manifest_evidence_id: Identifier
    observations_evidence_ids: list[Identifier] = Field(min_length=1, max_length=32)
    evidence_map: dict[Digest, Identifier] = Field(min_length=1, max_length=512)


class AcceptanceManifest(Strict):
    schema_version: Literal[1]
    report_id: Identifier
    contexts: list[Context] = Field(min_length=1, max_length=64)
    evidence: list[Evidence] = Field(min_length=1, max_length=512)
    observations: list[Observation] = Field(default_factory=list, max_length=4096)
    installation_matrix: list[Identifier] = Field(default_factory=list, max_length=32)
    study: StudyLink | None = None

    @model_validator(mode="after")
    def references(self):
        for values in (self.contexts, self.evidence):
            if len({value.id for value in values}) != len(values):
                raise ValueError("Context and evidence IDs must be unique.")
        if len({row.attempt_id for row in self.observations}) != len(self.observations):
            raise ValueError(
                "Attempt IDs must be unique; retain earlier failed and blocked attempts."
            )
        contexts = {context.id for context in self.contexts}
        evidence = {item.id for item in self.evidence}
        if len(set(self.installation_matrix)) != len(self.installation_matrix):
            raise ValueError("Installation matrix cells must be unique.")
        if not set(self.installation_matrix) <= contexts:
            raise ValueError("Installation matrix references an unknown context.")
        for context in self.contexts:
            if context.attestation_evidence_id not in evidence:
                raise ValueError("Each context needs hash-bound attestation evidence.")
        for row in self.observations:
            if row.context_id not in contexts or not set(row.evidence_ids) <= evidence:
                raise ValueError("Observation references an unknown context or evidence ID.")
        if self.study:
            ids = {
                self.study.manifest_evidence_id,
                *self.study.observations_evidence_ids,
                *self.study.evidence_map.values(),
            }
            if not ids <= evidence:
                raise ValueError("Study link references unknown local evidence.")
        return self


def _evidence_paths(manifest: AcceptanceManifest, directory: Path) -> dict[str, Path]:
    result, total = {}, 0
    for evidence in manifest.evidence:
        value = evidence.relative_path
        relative = PurePosixPath(value)
        if (
            relative.is_absolute()
            or "\\" in value
            or ":" in value
            or ".." in relative.parts
            or str(relative) != value
            or any(ord(c) < 32 for c in value)
            or any(part.endswith((" ", ".")) for part in relative.parts)
        ):
            raise JevError(
                "acceptance_contract", "Evidence paths must stay within the manifest folder."
            )
        path = local_path(str(directory / Path(*relative.parts)), exists=True)
        size = path.stat().st_size
        total += size
        if not 1 <= size <= 16 * 1048576 or total > 512 * 1048576:
            raise JevError(
                "acceptance_contract", "Acceptance evidence exceeds its bounded byte budget."
            )
        if hash_evidence(path) != evidence.sha256:
            raise JevError(
                "acceptance_contract", "Local acceptance evidence does not match its SHA-256."
            )
        result[evidence.id] = path
    return result


def _attestation_gaps(row: Observation, context: Context) -> list[str]:
    required = {"all_attempts_included"}
    metadata = {"plugin_version", "engine_version", "operating_system"}
    if row.priority == "review":
        required |= {"human_observed", "participant_not_implementer"}
        metadata |= {"display_scaling", "panel_width", "input_device", "language"}
        if row.task_id == "keyboard_copy":
            required.add("physical_keyboard")
        if row.task_id == "assistive_errors":
            required.add("assistive_software")
            metadata.add("assistive_software_version")
        if row.task_id == "narrow_translation":
            required.add("reviewed_translation")
    elif row.priority == "installation":
        required |= {"clean_windows_host", "licensed_engine"}
        metadata |= {"python_version", "compiler_version", "windows_sdk_version"}
    elif row.priority in {"validators", "blueprints", "gameplay"}:
        required |= {"real_project", "licensed_engine", "project_owned_criteria"}
    elif row.priority == "study":
        required |= {"real_project", "independent_evaluation", "project_owned_criteria"}
    gaps = [name for name in sorted(required) if not getattr(context.attestations, name)]
    gaps += ["metadata:" + name for name in sorted(metadata) if not getattr(context, name)]
    if row.priority == "installation" and context.project_kind not in {"blueprint_only", "cpp"}:
        gaps.append("blueprint_only_or_cpp_project")
    if row.priority in {"validators", "blueprints", "gameplay", "study"} and (
        context.project_kind == "synthetic_fixture"
    ):
        gaps.append("representative_project_context")
    return gaps


def _study_summary(link: StudyLink, paths: dict[str, Path]) -> dict:
    study_manifest = read_json(paths[link.manifest_evidence_id])
    observations = []
    for evidence_id in link.observations_evidence_ids:
        bundle = StudyObservations.model_validate(read_json(paths[evidence_id]))
        if bundle.manifest_sha256 != fingerprint(study_manifest):
            raise JevError(
                "acceptance_contract", "Study observations name a different frozen manifest."
            )
        observations.extend(row.model_dump(mode="json") for row in bundle.observations)
    return score_study(
        study_manifest,
        observations,
        evidence_files={
            expected: paths[evidence_id] for expected, evidence_id in link.evidence_map.items()
        },
    )


def inspect_acceptance(manifest_path: str | Path) -> dict:
    """Verify local evidence and report six priorities; never certify human attestations."""
    try:
        path = local_path(str(Path(manifest_path).absolute()), exists=True)
        manifest = AcceptanceManifest.model_validate(read_json(path))
        paths = _evidence_paths(manifest, path.parent)
        contexts = {context.id: context for context in manifest.contexts}
        study = _study_summary(manifest.study, paths) if manifest.study else None
        priorities = {}
        for priority, tasks in TASKS.items():
            rows = [row for row in manifest.observations if row.priority == priority]
            counts = Counter(row.outcome for row in rows)
            gaps = {
                row.attempt_id: _attestation_gaps(row, contexts[row.context_id]) for row in rows
            }
            gaps = {attempt: missing for attempt, missing in gaps.items() if missing}
            if priority == "installation":
                cells = manifest.installation_matrix
                required = {(context_id, task) for context_id in cells for task in tasks}
                observed = {(row.context_id, row.task_id) for row in rows}
                missing = [
                    {"context_id": context, "task_id": task}
                    for context, task in sorted(required - observed)
                ]
                kinds = {contexts[value].project_kind for value in cells}
                if not {"blueprint_only", "cpp"} <= kinds:
                    missing.append({"matrix": "Declare Blueprint-only and C++ clean-host cells."})
                undeclared = sorted({row.context_id for row in rows} - set(cells))
                if undeclared:
                    missing.append({"undeclared_installation_contexts": undeclared})
            else:
                missing = [
                    {"task_id": task} for task in tasks if task not in {row.task_id for row in rows}
                ]
            if priority == "study":
                if study is None:
                    missing.append(
                        {"study": "Link the existing frozen study manifest and observations."}
                    )
                else:
                    if study["provenance_declared"] != "independent_study":
                        missing.append(
                            {"study": "Linked protocol is an authored/real-project pilot."}
                        )
                    if any(
                        method["missing"] or method["cost_missing"]
                        for method in study["methods"].values()
                    ):
                        missing.append(
                            {"study": "Registered trials or cost observations remain missing."}
                        )
            status = "attested_pass"
            if counts["failed"]:
                status = "failed"
            elif counts["blocked"]:
                status = "blocked"
            elif missing or not rows:
                status = "incomplete"
            elif gaps:
                status = "attestation_incomplete"
            priorities[priority] = {
                "status": status,
                "counts": {outcome: counts[outcome] for outcome in ("passed", "failed", "blocked")},
                "required_task_count": len(tasks),
                "missing": missing,
                "attestation_gaps": gaps,
                "attempts": [row.model_dump(mode="json") for row in rows],
            }
        return {
            "schema_version": 1,
            "report_id": manifest.report_id,
            "manifest_sha256": hash_evidence(path),
            "evidence_hashes_checked": len(paths),
            "priorities": priorities,
            "study_summary": study,
            "attestations": [context.model_dump(mode="json") for context in manifest.contexts],
            "all_priorities_attested_pass": all(
                row["status"] == "attested_pass" for row in priorities.values()
            ),
            "independence_verified": False,
            "human_acceptance_verified": False,
            "clean_host_verified": False,
            "production_readiness_established": False,
            "scope": "Hashes verify bytes; participant, clean-host, project and independence "
            "claims remain explicit attestations. Failed/blocked attempts stay visible. "
            "This report never establishes a speedup or closes external acceptance by itself.",
        }
    except (OSError, ValueError, TypeError, ValidationError):
        raise JevError(
            "acceptance_contract", "Acceptance manifest or local evidence is invalid."
        ) from None


def acceptance_schema() -> dict:
    return AcceptanceManifest.model_json_schema()
