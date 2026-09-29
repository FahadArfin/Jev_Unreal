# Test value and efficient validation

Keep a test when its failure would identify a distinct user-visible defect,
permission or data-loss risk, protocol incompatibility, or previously observed
regression. A large case count is neither a quality goal nor a reason to delete
coverage. Test files are not sent to a model just because pytest executes them.
Writing/reviewing tests, reading logs and repeated agent turns consume context;
local test execution primarily costs compute and time. Exact token savings were
not measured in this audit. The default suite uses synthetic provider fixtures.
OpenAI documents [tool results and generated reasoning as token inputs/outputs](https://developers.openai.com/api/docs/guides/agents-api/observability#what-contributes-to-cost).

## September 2026 audit

At the 0.9 release, **522 test functions expanded into 1,626 cases** across 37
Python test modules. The local release run took **150.30 seconds**. Its JUnit
record attributed approximately 50.7 seconds to installer tests, 28.5 to jobs,
23.9 to catalog discovery and 16.6 to the real CLI. These are one-run timings,
not a controlled performance benchmark.

| Decision | Cases/workflows | Reason and retained coverage |
| --- | --- | --- |
| Keep | Authentication, exact project identity, bounded paths/inputs, stale and one-shot plans | Failures could expose data or change the wrong scene. Similar guards at transport, planning and native execution are distinct boundaries. |
| Keep | Installer interruption, modified-file preservation, process-tree cancellation, crash receipts and lease-loss tests | These exercise real failure states. They account for much of the runtime and are more valuable than their speed alone suggests. |
| Keep | Official stdio MCP and real CLI lifecycle tests | In-process mocks cannot establish argument routing, startup, structured output or cleanup across a process boundary. |
| Keep | Native build/editor/rendered suites and scoped live smoke workflows | Python mocks cannot prove native compilation, Undo, gameplay or visual behavior. Run when the changed layer requires them. |
| Keep | Distinct malformed schema, Unicode, nonfinite and identity cases | Identical assertion bodies can cover different defects. Scalar provider state and invalid nested JSON remain separate tests. |
| Reduce | `test_domain_workflows.py::test_invalid_timing_values_rejected` | Exercise all seven invalid values on `mean_ms`, plus invalid input on each remaining metric. Nine cases replace 21; all metric fields and failure classes remain represented. |
| Reduce | Verification identity text matrix | Exercise all four invalid strings on one field, plus one invalid string on each other field. Eight cases replace 20; all five field bindings remain checked. |
| Consolidate | Duplicate mesh readback assertion bodies | One parameterized test retains all 18 source-identity, state-change and malformed-identity cases. No case is dropped. |
| Speed up | Two malformed mock MCP catalog-response tests | Short fixture-specific timeouts replace ten-second waits. Handler-reached assertions retain non-vacuous budget/redaction checks. Production timeouts are unchanged. |
| Remove duplicate execution | Feature-branch push plus PR CI | PRs still run Windows/Linux and Python 3.12/3.13. Push CI runs on `main`; manual runs remain available. A normal PR update starts four jobs instead of eight. |

The revised suite has **521 test functions and 1,602 cases**. The two catalog
fixtures fell from approximately 10.01 seconds each to **0.52 and 0.50 seconds**
in a focused run. This removes roughly 19 seconds of artificial waiting on that
host; it does not measure model-token savings or promise a fixed CI duration.
The complete revised suite passed in **113.71 seconds**, with locked dependency
sync, full Ruff and package builds also passing. No native or provider run was
needed for these test/CI/documentation-only changes.

Do not remove different fields from a refusal test merely because their validators
are shared. For large Cartesian products, a representative matrix can cover each
field binding and each invalid class separately when no interaction is involved.
Retain interaction cases where behavior depends on both dimensions.

## Daily workflow

1. Run only affected tests while changing code, for example:
   `uv run pytest tests/test_meshes.py -q --tb=short`.
2. After the final Python/test edits, run locked dependency sync, full Ruff, the
   complete pytest suite and `uv build` as required by `AGENTS.md`.
3. Keep detailed logs under ignored `artifacts/`; inspect the summary and failures
   first. Use verbose output or durations only to investigate a specific problem.
4. Repeat a green check when another relevant edit or new evidence justifies it.
   Documentation-only changes do not require new tests or an Unreal rebuild.
5. Native changes require a licensed build, editor automation and exact-sandbox
   live bridge checks. Provider evaluations require separate explicit evidence.

Do not add tests merely to mirror an implementation, repeat a framework guarantee,
pin an incidental count/string, or check a reversible documentation edit. Public
schema compatibility, privacy contracts and meaningful failure messages can justify
such assertions; name the contract so future maintainers know why it matters.
