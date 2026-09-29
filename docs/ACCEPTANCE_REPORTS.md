# Evidence-bound acceptance reports

The local acceptance reporter implements the bookkeeping in
[COMMUNITY_ACCEPTANCE.md](COMMUNITY_ACCEPTANCE.md). It checks local evidence hashes,
reports all six immediate priorities, and retains failed, blocked and missing
attempts. It does not create user observations or establish clean-machine,
human-accessibility or independent-study acceptance.

Run `jev-unreal acceptance schema` for the exact JSON schema and
`jev-unreal acceptance report --manifest C:/PrivateAcceptance/session.json` to
validate a report. The Python API is
`jev_unreal.acceptance.inspect_acceptance(manifest_path)`. It reads only the named
manifest and its explicitly referenced local evidence. No engine/provider runs,
file writes, uploads, participant contact or background discovery occur.

A minimal deliberately incomplete manifest is:

```json
{
  "schema_version": 1,
  "report_id": "session-001",
  "contexts": [
    {
      "id": "participant-a-machine-1",
      "project_kind": "real_project",
      "attestor_id": "observer-a",
      "attestation_evidence_id": "session-notes",
      "attestations": {
        "human_observed": false,
        "participant_not_implementer": false,
        "all_attempts_included": false
      },
      "plugin_version": "record-exact-version",
      "engine_version": "record-exact-version",
      "operating_system": "record-exact-version"
    }
  ],
  "evidence": [
    {
      "id": "session-notes",
      "relative_path": "evidence/session-notes.txt",
      "sha256": "REPLACE_WITH_EXACT_64_LOWERCASE_HEX_DIGEST"
    }
  ],
  "observations": [],
  "installation_matrix": []
}
```

Replace the placeholder hash with the actual SHA-256; the placeholder fails
validation. Evidence is nonempty regular files of at most 16 MiB each, 512 MiB
in total, with at most 512 entries. Paths are canonical forward-slash relative
paths under the manifest's directory. Absolute paths, parent traversal, network
paths, symbolic links and reparse points are rejected. Keep original private
evidence locally. Reports include pseudonyms, version metadata and explicit notes;
review those fields before sharing. Hash matching proves supplied byte identity,
not that the evidence depicts the claimed event or that omitted attempts do not
exist.

Every observation includes a unique `attempt_id`, `priority`, `task_id`,
`context_id`, `outcome` (`passed`, `failed` or `blocked`), `evidence_ids`,
`assistance_count`, and optional bounded `note`. Passed observations require
`elapsed_seconds`. Use a new attempt ID for a retry and preserve the earlier
failure. A passing retry does not erase a failed/blocked report status. Describe
the fix and run a new separately identified acceptance session when appropriate;
keep earlier reports as history.

| Priority | Required task IDs |
| --- | --- |
| `review` | `inspect_units`, `preview_translation`, `keyboard_copy`, `apply_verify`, `undo_receipt`, `stale_recovery`, `assistive_errors`, `narrow_translation` |
| `installation` | `prerequisites`, `install_build_connect`, `previous_release_upgrade`, `modified_source_refusal`, `interruption_recovery`, `locked_file_exclusion` |
| `validators` | `valid_asset`, `invalid_asset`, `not_applicable`, `diagnostics`, `dirty_state`, `cancellation`, `cleanup` |
| `blueprints` | `known_good`, `known_broken`, `instance_effects`, `dependent_asset_effects` |
| `gameplay` | Each of `door`, `navigation`, `interaction`, `combat` combined with `_passing`, `_regression`, `_repeat`, `_cleanup` |
| `study` | `registered_workflow_study` plus the existing frozen study evidence |

Create separately identified contexts and retain rule/asset/project acceptance
criteria in referenced evidence. The reporter counts this protocol's task
coverage; it cannot prove representative coverage of every validator, subclass,
project or supported machine. State the tested subset explicitly in the evidence
and published summary.

## Attestations and matrix coverage

Every context names an attestor pseudonym and hash-bound attestation evidence.
Boolean claims default to false. Every priority requires an explicit
`all_attempts_included` attestation. Review observations require `human_observed`
and `participant_not_implementer`; specific tasks also require `physical_keyboard`,
`assistive_software` or `reviewed_translation`. Record display scaling, panel width,
input device, language and, where relevant, assistive software/version. Synthetic
key events and expanded English labels are not those attestations.

Installation contexts require `clean_windows_host` and `licensed_engine` claims,
exact Python/compiler/SDK metadata, and `project_kind` of `blueprint_only` or `cpp`.
`installation_matrix` lists each intended context ID. Both project kinds and all
six tasks in every declared cell are required. Extra observations in undeclared
cells do not silently establish matrix coverage. A context could represent a
clean VM or a separate clean Windows host; record its preparation and missing
dependencies in the attestation evidence. No host is inferred to be clean from a
successful subprocess test or metadata scan.

Validator, Blueprint and gameplay observations require `real_project`,
`licensed_engine` and `project_owned_criteria` attestations. Keep actual rule
classes, prerequisites, good/broken assets, instance effects and repeated cleanup
evidence in the referenced artifacts. Study observations additionally require
`independent_evaluation`; a synthetic project context cannot satisfy real-project
acceptance merely by setting flags.

Each priority reports `incomplete`, `attestation_incomplete`, `failed`, `blocked`
or `attested_pass`, along with missing task/matrix cells and attestation gaps.
`attested_pass` means the submitted records satisfy this report contract. The
output always preserves `human_acceptance_verified: false`,
`clean_host_verified: false`, `independence_verified: false` and
`production_readiness_established: false`. External reviewers assess those claims.

## Reuse the registered workflow study

Prepare trials and observations with the existing
[study protocol](BENCHMARKS.md). The acceptance manifest's optional `study` object
references that frozen manifest, one or more `StudyObservations` bundles and a
mapping from required SHA-256 hashes to local evidence IDs:

```json
{
  "manifest_evidence_id": "frozen-study",
  "observations_evidence_ids": ["study-observations"],
  "evidence_map": {
    "REPLACE_WITH_REQUIRED_STUDY_EVIDENCE_SHA256": "referenced-evidence-id"
  }
}
```

The reporter calls the existing study scorer rather than recomputing a second
comparison protocol. It checks the frozen schedule, exact artifact map, method
bindings, all observed failures, missing trials and missing costs. A pilot remains
a pilot. Independent-study provenance must be explicitly declared and attested;
even a complete linked study does not turn that declaration into independently
verified evidence or emit a speedup conclusion.

## Installer readiness diagnostics

`setup inspect --engine-root ...` now includes structured `readiness.checks` and
actionable guidance. It compares numeric project/engine version associations,
reports custom/GUID associations as unverified, checks explicit editor/UBT
entrypoints and assemblies, reads bounded UBT runtime requirement metadata, and
looks for bundled .NET hosts without executing them. On Windows it checks the
compiler, linker, headers and runtime library plus SDK headers, libraries and
resource compiler in bounded standard-location scans.

`metadata_prerequisites_satisfied` is a metadata result. Nonstandard toolchain
locations and actual supported-version compatibility may require manual review.
These diagnostics never claim a native build, successful runtime load, clean-host
installation or imported-Python-dependency check merely because files exist.
