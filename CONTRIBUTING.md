# Contributing to Jev_Unreal

Thanks for helping build a dependable bridge between AI tools and Unreal Editor.
This project is an early implementation. Claims about supported editor versions,
operations, and performance must match evidence in the repository.

## Set up

Install Python 3.12 or 3.13 and [uv](https://docs.astral.sh/uv/getting-started/installation/),
then run from the repository root:

```console
uv sync --locked --all-extras
uv run ruff check .
uv run pytest
uv build
```

Use a feature branch, such as `codex/describe-your-change`, and open a pull request.
Keep changes focused. Explain the user-visible behavior, how you verified it, and
any limits that remain. Documentation fixes do not require new tests. Add or update
meaningful tests when changing parsing, decision validation, permissions, protocol
behavior, or editor mutations.

## Unreal changes

Use a disposable Unreal project for editor experiments. Do not use a production
level as a test fixture. Record the exact engine version, operation, observed result,
and any Undo or recovery behavior. A successful Python test suite does not prove a
plugin compiles, an editor operation works, or a packaged game behaves correctly.
Cloud CI runs the Python checks; Unreal acceptance requires a licensed local engine.

An editor operation should have a narrow schema, explicit capability checks,
bounded work, clear errors, and a testable outcome. Preserve the game thread boundary
and report failures without pretending partial mutations completed successfully.
Do not add arbitrary code execution, shell access, public network listening, silent
file deletion, or unrestricted content access as a shortcut around missing commands.

## Credentials and data

Use your own provider account for optional live model checks. No live API key is
required for the offline test suite. Never commit tokens, `.env` files, DPAPI files,
private project content, or prompt logs containing credentials. Redact output before
sharing it. The model provider may receive submitted prompts and scene descriptions;
avoid using proprietary game content without permission.

## Licensing and attribution

Original contributions are under this repository's MIT license. Submit only code
and assets you are allowed to distribute. Identify third-party dependencies and
their licenses in your pull request. Do not copy Unreal Engine source or restricted
marketplace assets into this repository. Preserve upstream attribution when reusing
permissively licensed material.

Please follow [the code of conduct](CODE_OF_CONDUCT.md). Report vulnerabilities using
[the security policy](SECURITY.md), not a public issue containing exploit details or
credentials.
