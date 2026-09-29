# Translations and beginner recipes

Jev includes eight read-only beginner workflows in English, French and Spanish:
scene inspection, reviewed actor movement, material parameters, surface placement,
gameplay testing, rig/root-motion checks, runtime UI inspection and performance
comparisons.

Read `jev://recipes/en`, `jev://recipes/fr` or `jev://recipes/es` through the MCP
client. Individual resources use a stable recipe ID, for example
`jev://recipes/fr/move_actor`. The `beginner_workflow` MCP prompt accepts
`recipe_id` and an optional `locale` (`en`, `fr` or `es`).

Every recipe supplies prerequisites, the exact tool names in sequence, the
evidence to inspect and the permission boundary. It never executes tools or
fills an apply plan automatically. Read current tool schemas, inspect the intended
project, and supply exact actor/asset identities from that inspection. Unknown
recipe IDs and unsupported locales are refused rather than interpreted as paths
or provider prompts.

French and Spanish text is a draft and is marked
`draft_requires_human_review`. Native-speaker review, beginner studies and actual
screen-reader acceptance remain open.

## Native Unreal panel translations

Source PO catalogs cover 60 of the panel's 64 messages in each of French and
Spanish. The remaining panel messages and technical before/after presentation
text fall back to English. This is partial localization, not complete translated
product acceptance. Technical field names and tool identifiers are intentionally
unchanged.

The plugin declares an editor localization target. Build the source plugin first
with `Build-Unreal.ps1`, close the sandbox editor, then compile its local resources
with the same licensed Unreal installation:

```powershell
.\scripts\Build-Localization.ps1 -EngineRoot 'C:\Program Files\UE_5.8'
.\scripts\Launch-Unreal.ps1 -LocalizationTests
```

The source-only GatherText pipeline reads this plugin's C++ headers and sources,
builds manifests/archives, imports the checked-in PO catalogs with source checks
enabled, and compiles English/French/Spanish resources. It neither gathers game
assets nor saves them. Intermediate output stays in `artifacts/localization`;
compiled `.locres` and `.locmeta` files are copied to the plugin's ignored
`Content/Localization/JevEditor` directory. They are local generated resources,
not checked-in engine binaries. Rebuild after translating a message or changing
its English source.

Restart the isolated editor after compiling resources, then select French or
Spanish as the editor language. Without compiled resources the panel uses its
English source text. `Jev.Localization.CultureSwitch` is a separate opt-in native
automation test: it requires the compiled resources, switches French → Spanish →
English, checks actual translated FText values, and restores the previous
language and locale. It is separate from the default editor suite so a source-only
checkout does not silently pretend localization resources were built.

Python checks verify that every translated source still matches the C++ message,
that format arguments such as `{0}` remain intact, and that resources/prompts are
callable without an editor or provider. These checks do not replace visual review
of expanded text, accented glyphs, focus order or assistive technology.

The implementation follows Unreal's
[localization commandlet pipeline](https://dev.epicgames.com/documentation/unreal-engine/localization-tools-in-unreal-engine).
