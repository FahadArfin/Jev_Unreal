# Working on Jev_Unreal

- Work on a `codex/` feature branch; open a PR with validation evidence.
- This is an independent MIT community project, not an official Epic or TypeSafe product.
- Keep Jev recommendations separate from execution and permissions. Never make confidence a security boundary.
- Never add arbitrary Python, console, shell, filesystem, or C++ execution to the authenticated editor bridge.
- Preserve explicit project identity, loopback binding, bearer authentication, bounded inputs, stale-plan checks, and one-shot preview plans.
- Do not commit API keys, bridge tokens, private game data, Unreal binaries, engine source, Saved/Intermediate files, or raw provider payloads.
- Use the OpenRouter Decisions endpoint, not chat completions. Check current primary provider docs before changing the alpha wire contract.
- Run `uv sync --locked --all-extras`, `uv run ruff check .`, `uv run pytest`, and `uv build` for Python changes.
- Build changed C++ against a licensed Unreal installation. Run `Jev.Editor` automation and the live bridge smoke test in the isolated sandbox.
- Verify the connected project before edits. Serialize Unreal operations. Never modify another game's open editor during integration tests.
- Describe mock tests, live provider evaluations, engine builds, editor tests, and visual acceptance separately. Never claim unmeasured efficiency or production readiness.
- Update user docs and `docs/VALIDATION.md` when public behavior or validation evidence changes.
