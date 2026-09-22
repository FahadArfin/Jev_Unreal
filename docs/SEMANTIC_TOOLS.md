# Asset shortlists and diagnostic groups

These helpers operate on **explicit caller-supplied data**. They never read project files, connect to another MCP server, inspect the editor, or perform edits. Both default to local processing; optional Jev assistance requires `use_jev: true`. Recommendations never grant permission to execute an operation.

The `cloud` result distinguishes `requested` (caller opt-in), `attempted` (the DecisionClient was invoked), and `used` (a valid decision was returned). An unsuccessful attempt may already have sent data; `used: false` is not proof that nothing left the machine. The DecisionClient can also satisfy an attempt from its cache.

## Asset selection

`rank_candidates(goal, candidates, filters=None, limit=10, use_jev=False, client=None)` accepts 1–128 asset candidates and returns up to 20 shortlisted items. Every candidate has a unique `id` and unique asset `path`; optional fields are `description`, `class_name`, `dimensions_cm` (XYZ centimeters), `has_collision`, and `tags`. IDs must use letters, numbers, `_`, `.`, `:`, or `-`; `__defer__` is reserved. Aggregate candidate metadata is limited to 128 KiB. The goal is limited to 4096 characters.

Hard filters run first:

- `class_names`: exact allowed class names, not inferred inheritance.
- `min_dimensions_cm` and `max_dimensions_cm`: inclusive bounds for each supplied XYZ axis. No rotation, unit conversion, or dimension sorting is inferred.
- `require_collision`: requires the supplied boolean to match, including when it is `false`.

A missing field required by a filter causes an explicit rejection such as `unknown_collision`. A description claiming an asset has collision does not substitute for `has_collision`. Without a filter, missing metadata stays unknown. All metadata remains caller-supplied and is **not verified against the editor**.

Local ranking counts unique exact word overlap between the goal and supplied path, description, class, and tags. Ties use the candidate ID. `matched_terms` and `lexical_score` explain that ordering; they are not probability or confidence scores. Shortlist truncation and rejected candidates are reported.

With Jev enabled, only the goal, shortlist, and filters are sent through the existing bounded DecisionClient. Jev chooses one supplied candidate or defers; a recommended candidate moves to the front while the remainder keep their lexical order. This is **one semantic choice, not a complete semantic ranking**. Existing uncertainty gates apply. Missing keys, budget exhaustion, or provider failure leave local results usable and report an error code without the provider error message.

This helper evaluates text metadata, not rendered appearance. A description such as “rusty warehouse door” is a supplied claim. Visual suitability requires separate image review; collision, scale, and import quality require editor validation before use.

## Diagnostic grouping

`group_diagnostics(log_text, max_groups=16, excerpt_chars=1200, use_jev=False, client=None)` accepts up to 128 KiB and 8192 lines of explicit text. It strips leading common Unreal/ISO timestamps, normalizes whitespace, redacts common credential shapes, and groups identical remaining messages. Distinct asset paths, numbers, and error details remain distinct. No fuzzy grouping or root-cause inference is claimed.

Each group includes its count, original first/last line numbers, severity, a local pattern-based category, and a bounded excerpt. Groups are ordered by severity, then first occurrence. At most 32 groups are returned; omitted groups and truncated excerpts are reported. Severity/category patterns are navigation aids, not build or test verdicts. Exit codes and engine validation remain authoritative.

Optional Jev processing sends a single batch containing one category question per returned group. Each cloud excerpt is additionally bounded to 800 UTF-8 bytes; truncation is explicit. Model suggestions are returned alongside the original local category and evidence rather than overwriting them. Insufficient uncertainty data or ambiguity produces deferral. Provider failure preserves the local grouping.

Redaction handles common API-key/token/password assignments, Bearer/Basic authorization values, HTTP URL credentials, and recognizable `sk-` key strings **before excerpts are truncated**. It is best effort: proprietary paths, asset names, source snippets, and unfamiliar secret formats can remain. Inspect and minimize supplied excerpts before opting into cloud processing. No files or surrounding project state are uploaded automatically.

## Validation scope

Synthetic unit tests cover hard-filter rejection, unknown metadata, deterministic ordering, duplicate IDs/paths, malformed and oversized inputs, provider budgets/offline fallback, invalid provider choices, redaction, timestamp deduplication, group limits, Unicode payload budgets, and instructions embedded in supplied data. These tests establish implementation behavior; they do not establish resistance to every prompt injection, semantic accuracy, visual quality, or faster game development. New features need separate real-world evaluations before any such claims.
