# Reusable gameplay test recipes

The source-only `JevSandbox` example now contains four project-owned
`AFunctionalTest` recipes. They use the existing named functional-test adapter;
there is no new execution endpoint or model-generated callback. Each recipe runs
in PIE, observes actual native gameplay state, and reports Unreal's native result.

| Recipe class | What it exercises | Regression it detects |
| --- | --- | --- |
| `AJevDoorRecipe` | Locked opening denial; unlocked 90-degree opening and disabled blocking collision; closing restores both | `bJammedDoor=true` prevents the unlocked door from opening |
| `AJevInteractionRecipe` | A native interaction interface; out-of-range rejection; accepted nearby use; consumed-target rejection | `bDisableInteraction=true` prevents the nearby use |
| `AJevCombatRecipe` | `UGameplayStatics::ApplyDamage`; 25% armor; health reduction; overkill clamping and dead-target rejection | `DamageScale=0` prevents health from changing |
| `AJevNavigationRecipe` | Projection of two configured endpoints onto existing navigation and a complete, finite native path connecting them | A destination outside the built navmesh fails projection; partial paths cannot pass |

These are working examples of the test pattern, not proof that another project's
doors, interactions, combat or AI work. Navigation verifies path reachability,
not controller movement, animation, avoidance or network replication.

## Use the example in Unreal

1. Build the supplied `examples/JevSandbox/JevSandbox.uproject` and open it.
2. In your own disposable test map, place the relevant `Jev Door Recipe`,
   `Jev Interaction Recipe`, `Jev Combat Recipe` or `Jev Navigation Recipe` actor.
3. For navigation, provide a built navmesh and place the recipe on it. Its
   `DestinationOffset` defaults to 400 cm along X. Both endpoints must project
   within `EndpointTolerance` (default 50 cm). The recipe only queries existing
   navigation; missing navigation data is an error, never a successful skip.
4. Save the map yourself and copy the exact recipe actor path. Add only the
   approved aliases and their exact paths to `Config/DefaultGame.ini`, following
   [the example policy](../examples/recipes/DefaultGame.recipe-policy.example.ini).
   The paths in that file are illustrative and must be replaced.
5. Start one standalone PIE session. Run `unreal_functional_tests`, inspect the
   current project/session/world/revision, and invoke `unreal_functional_start`
   for the approved alias. Poll the resulting job; inspect its native result and
   cleanup evidence before accepting a pass.

The recipes themselves require PIE. The production MCP tool still does not create
the fixture map, build navigation, place actors, configure approval, start PIE or
save anything. Those remain deliberate project-authoring actions.

## Adapt the pattern to your game

Copy [the header](../examples/JevSandbox/Source/JevSandbox/JevGameplayRecipes.h)
and [implementation](../examples/JevSandbox/Source/JevSandbox/JevGameplayRecipes.cpp)
into your project's source module, adjust class names and dependencies, and
replace each recipe's subject and actions with your game's APIs. The supplied
module depends on `FunctionalTesting` and `NavigationSystem` in addition to the
ordinary engine modules. The sample interaction interface is native C++; it is
not a generic adapter to every project's Blueprint interaction interface.

Keep an independently stated expected result. For example, the combat recipe
expects 40 incoming damage and 25% armor to remove 30 health; it checks both the
return from `ApplyDamage` and the resulting state. A test that simply reuses the
subject's computed answer as its expectation will miss regressions.

For asynchronous behavior, override `Tick`, observe the final state over a bounded
time, and finish only when the outcome is established. Do not make a timeout or
missing dependency count as success. The sample native time limit is eight
seconds; the bridge's project-configured job budget also applies.

The fault-injection properties are sample regression controls set in the test
map. They are not MCP arguments. Run the broken variants when adapting a recipe:
a useful check must fail when the relevant gameplay behavior is broken.

## Setup, teardown and evidence

`AJevGameplayRecipe` tracks the actors it spawns as transient, assigns each actor
to the recipe as owner, and destroys only those still-owned actors in `CleanUp`.
It also tears them down before a new run and during `EndPlay`, so ending PIE does
not depend on a later MCP callback. Cleanup is repeatable. Actors transferred to
another owner are intentionally left alone; custom project teardown must account
for such ownership transfers.

The navigation recipe spawns no actor and changes no navmesh. The other recipes
spawn their own small native subjects at the test actor's location and do not
use the player's pawn. Keep fixture locations away from unrelated gameplay and
provide any additional project isolation your adapted subjects require.

`ObservationCount`, `LastObservation`, and `CleanupCount` are transient properties
on the native recipe. Navigation also records `ObservedPathLength`. The MCP
receipt uses the existing bounded native result, elapsed time and cleanup fields;
it does not upload arbitrary actor data. As with all project callbacks, the
bridge cannot prove that unrelated custom code avoided every side effect.

## Native acceptance fixture

`Jev.Editor.GameplayRecipes` discovers only the four exact classes in the loaded
`JevSandbox` source module. It constructs an unsaved floor and bounds using Unreal's
native cube builder, builds actual navigation data, and waits for a usable route.
It then starts its own standalone PIE session and configures exact temporary
actor aliases in memory. Through the real functional adapter it runs:

- One successful and one deliberately broken case for each of the four recipes.
- A repeated door run to check fresh setup and cleanup across runs.
- Observed native result, observation count, exactly one cleanup per run, removal
  of owned subjects, and survival of an unrelated editor actor.

It ends its own PIE session and restores the previous in-memory policy and PIE
settings. No example map, navmesh binary or engine asset is committed. Outside
JevSandbox the suite reports that its project module is missing and does not
claim recipe acceptance. A buildable fixture is not a completed run; current
executed results belong in [validation evidence](VALIDATION.md).

See [project-owned functional tests](FUNCTIONAL_TESTS.md) for authorization,
identity checks, cancellation, limitations and receipt semantics.
