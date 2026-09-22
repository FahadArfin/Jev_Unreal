# Changelog

## 0.2.0a1

- Added explicit local MCP catalog discovery, bounded pagination, versioned exact
  schemas, searchable action presets, local retrieval and optional Jev selection.
- Added compact editor context, static mesh metadata, deterministic scene warnings,
  native viewport framing and MCP image capture.
- Added existing static mesh placement and measured grid, stair and room previews
  through the same single-use native Undo transaction.
- Verify native actor identity/transform readbacks after applying tracked previews.
- Added local asset hard filters and diagnostic grouping with optional batched Jev
  assistance, explicit cloud status and offline fallback.
- Added CLI doctor/context/catalog/layout commands, two MCP workflow prompts,
  recipes and detailed user documentation.
- Reject numeric coercion in operation inputs, cap native UTF-8 responses, validate
  returned PNG structure and retain applied results when verification data is bad.

This remains an alpha. See `docs/VALIDATION.md` for separately recorded Python,
protocol, provider, native editor and rendered viewport evidence. No production
accuracy, broad engine compatibility or developer speedup is claimed.

## 0.1.0a2

- Fixed native PowerShell security-module resolution and added credential regression tests.

## 0.1.0a1

- Initial typed Jev client, stdio MCP server, authenticated editor bridge, primitive
  preview/apply transactions, Unreal sandbox and public evaluation harness.
