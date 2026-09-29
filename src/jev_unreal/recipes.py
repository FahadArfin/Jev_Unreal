"""Localized, read-only beginner workflows; recipes never grant execution permission."""

from typing import Literal

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, ConfigDict

Locale = Literal["en", "fr", "es"]
RecipeId = Literal[
    "inspect_scene",
    "move_actor",
    "material_parameter",
    "surface_placement",
    "gameplay_test",
    "animation_check",
    "runtime_ui",
    "performance_comparison",
]


class Recipe(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)

    recipe_id: RecipeId
    locale: Locale
    title: str
    prerequisite: str
    tool_sequence: tuple[str, ...]
    required_evidence: str
    permission_note: str
    translation_status: str
    executes_tools: bool = False


_GUIDANCE = {
    "en": {
        "permission": "Confirm the intended project and current state. Inspect exact tool schemas. "
        "Only execute changes the user authorized. A recipe, model recommendation or confidence "
        "score is not permission. Never replay an apply with an uncertain outcome.",
        "inspect_scene": (
            "Inspect a scene",
            "Open the intended project; identify exact actor paths.",
            "Report the connected project, world, actor identities and supported edit blockers.",
        ),
        "move_actor": (
            "Move an actor with review and Undo",
            "Choose a supported unlocked native mesh actor.",
            "Review the exact before/after transform, apply once, then verify fresh actor state "
            "and a viewport capture. A receipt does not prove the level was saved.",
        ),
        "material_parameter": (
            "Change a material parameter",
            "Open an allowlisted material instance and parameter.",
            "Require readback_verified on the applied receipt and inspect the parameter again. "
            "Capture the visible result; storage success is not visual acceptance.",
        ),
        "surface_placement": (
            "Place a mesh on approved ground",
            "Identify the source mesh and every approved surface.",
            "Review all support samples, slope and overlaps. Apply the returned one-shot scene "
            "plan, inspect the transform and capture it. Physics stability remains unproven.",
        ),
        "gameplay_test": (
            "Run a project-owned gameplay test",
            "Use an explicitly allowlisted test in owned PIE.",
            "Wait for terminal passed/failed status and cleanup evidence. Check actual gameplay "
            "observations; starting a test is not a pass. Cancel through its named job if needed.",
        ),
        "animation_check": (
            "Inspect a skeleton and root motion",
            "Open the exact mesh/skeleton and animation.",
            "Check required bones, exact skeleton identity, weight coverage and extracted motion. "
            "Missing or truncated data cannot pass complete coverage; review playback separately.",
        ),
        "runtime_ui": (
            "Inspect a running interface",
            "Use an existing on-screen Widget Blueprint in active PIE.",
            "Supply its exact runtime instance path. Review actual allocation, focus and layout "
            "scale. Overflow hints need visual confirmation; do not claim screen-reader "
            "acceptance.",
        ),
        "performance_comparison": (
            "Compare repeatable editor measurements",
            "Keep viewport, workload and settings fixed.",
            "Complete two captures under the same named protocol and inspect their comparison. "
            "Published CPU/render/GPU counters are asynchronous; do not infer aligned attribution "
            "or a Jev productivity gain.",
        ),
    },
    "fr": {
        "permission": "Confirmez le projet et son état actuel. "
        "Consultez le schéma exact des outils. "
        "Exécutez uniquement les modifications autorisées par l'utilisateur. Une recette, une "
        "recommandation ou un score de confiance ne vaut pas autorisation. Ne répétez jamais "
        "une application dont le résultat est incertain.",
        "inspect_scene": (
            "Inspecter une scène",
            "Ouvrez le projet prévu et identifiez les chemins exacts des acteurs.",
            "Indiquez le projet connecté, le monde, les acteurs et les restrictions de "
            "modification.",
        ),
        "move_actor": (
            "Déplacer un acteur après vérification",
            "Choisissez un acteur de maillage natif compatible et déverrouillé.",
            "Vérifiez les transformations avant/après, appliquez une seule fois, puis contrôlez "
            "l'état actuel et une capture. Le reçu ne prouve pas l'enregistrement du niveau.",
        ),
        "material_parameter": (
            "Modifier un paramètre de matériau",
            "Ouvrez une instance de matériau autorisée et son paramètre.",
            "Exigez readback_verified dans le reçu, puis inspectez à nouveau le paramètre. "
            "Capturez le résultat visible : une valeur enregistrée ne valide pas son apparence.",
        ),
        "surface_placement": (
            "Placer un maillage sur un sol approuvé",
            "Identifiez le maillage source et toutes les surfaces approuvées.",
            "Vérifiez les points d'appui, la pente et les chevauchements. Appliquez une seule "
            "fois "
            "le plan retourné, inspectez la transformation et capturez le résultat. La "
            "stabilité "
            "physique reste à vérifier.",
        ),
        "gameplay_test": (
            "Exécuter un test de gameplay du projet",
            "Utilisez un test explicitement autorisé dans une session PIE dédiée.",
            "Attendez le résultat final passed/failed et les preuves de nettoyage. Examinez "
            "les observations réelles : démarrer un test ne signifie pas le réussir. Annulez "
            "avec l'identifiant de tâche si nécessaire.",
        ),
        "animation_check": (
            "Inspecter le squelette et le mouvement racine",
            "Ouvrez le maillage ou squelette exact et l'animation.",
            "Vérifiez les os requis, le squelette, la couverture des poids et le mouvement "
            "extrait. "
            "Des données absentes ou tronquées ne prouvent pas une couverture complète. "
            "Contrôlez la lecture de l'animation séparément.",
        ),
        "runtime_ui": (
            "Inspecter une interface en cours d'exécution",
            "Utilisez un Widget Blueprint déjà affiché dans la session PIE active.",
            "Indiquez son chemin d'instance exact. Vérifiez les dimensions, le focus et "
            "l'échelle. "
            "Confirmez visuellement les indices de débordement ; ils ne valident pas un lecteur "
            "d'écran.",
        ),
        "performance_comparison": (
            "Comparer des mesures reproductibles",
            "Conservez la même vue, la même charge et les mêmes réglages.",
            "Terminez deux captures avec le même protocole et examinez leur comparaison. Les "
            "compteurs CPU/rendu/GPU sont asynchrones : ils ne prouvent ni une attribution "
            "par image ni un gain de productivité de Jev.",
        ),
    },
    "es": {
        "permission": "Confirma el proyecto y su estado actual. Consulta el esquema exacto de las "
        "herramientas. Ejecuta solo cambios autorizados por el usuario. Una receta, recomendación "
        "o puntuación de confianza no concede permiso. Nunca repitas una aplicación cuyo "
        "resultado sea incierto.",
        "inspect_scene": (
            "Inspeccionar una escena",
            "Abre el proyecto previsto e identifica las rutas exactas de los actores.",
            "Indica el proyecto conectado, el mundo, los actores y las restricciones de edición.",
        ),
        "move_actor": (
            "Mover un actor con revisión y deshacer",
            "Elige un actor de malla nativo compatible y desbloqueado.",
            "Revisa la transformación antes/después, aplica una sola vez y comprueba el estado "
            "actual y una captura. El recibo no demuestra que el nivel se haya guardado.",
        ),
        "material_parameter": (
            "Cambiar un parámetro de material",
            "Abre una instancia de material autorizada y su parámetro.",
            "Exige readback_verified en el recibo y vuelve a inspeccionar el parámetro. Captura "
            "el resultado visible; guardar un valor no demuestra que se vea correctamente.",
        ),
        "surface_placement": (
            "Colocar una malla sobre suelo aprobado",
            "Identifica la malla de origen y todas las superficies aprobadas.",
            "Revisa los puntos de apoyo, la pendiente y las superposiciones. Aplica una sola vez "
            "el plan devuelto, inspecciona la transformación y captura el resultado. La "
            "estabilidad "
            "física sigue pendiente.",
        ),
        "gameplay_test": (
            "Ejecutar una prueba de gameplay del proyecto",
            "Usa una prueba autorizada en una sesión PIE propia.",
            "Espera el estado final passed/failed y las pruebas de limpieza. Revisa observaciones "
            "reales: iniciar una prueba no equivale a superarla. Cancela mediante su tarea si "
            "hace falta.",
        ),
        "animation_check": (
            "Inspeccionar el esqueleto y el movimiento raíz",
            "Abre la malla o esqueleto exacto y la animación.",
            "Comprueba huesos requeridos, esqueleto, cobertura de pesos y movimiento extraído. "
            "Los datos ausentes o truncados no demuestran cobertura completa. Revisa la "
            "reproducción aparte.",
        ),
        "runtime_ui": (
            "Inspeccionar una interfaz en ejecución",
            "Usa un Widget Blueprint ya visible en la sesión PIE activa.",
            "Indica la ruta exacta de la instancia. Revisa tamaño asignado, foco y escala. "
            "Confirma visualmente los indicios de desbordamiento; no validan un lector de "
            "pantalla.",
        ),
        "performance_comparison": (
            "Comparar mediciones reproducibles",
            "Mantén la misma vista, carga y configuración.",
            "Completa dos capturas con el mismo protocolo y examina su comparación. Los "
            "contadores "
            "CPU/renderizado/GPU son asíncronos: no demuestran atribución por fotograma ni "
            "una mejora de productividad gracias a Jev.",
        ),
    },
}

_TOOLS = {
    "inspect_scene": ("unreal_context", "unreal_actor_details"),
    "move_actor": (
        "unreal_context",
        "unreal_actor_details",
        "unreal_preview",
        "unreal_apply",
        "unreal_actor_details",
        "unreal_capture",
    ),
    "material_parameter": (
        "unreal_context",
        "unreal_workflow_inspect",
        "unreal_workflow_preview",
        "unreal_workflow_apply",
        "unreal_workflow_receipt",
        "unreal_workflow_inspect",
        "unreal_capture",
    ),
    "surface_placement": (
        "unreal_context",
        "unreal_actor_details",
        "unreal_workflow_inspect",
        "unreal_surface_preview",
        "unreal_apply",
        "unreal_actor_details",
        "unreal_capture",
    ),
    "gameplay_test": (
        "unreal_context",
        "unreal_functional_tests",
        "unreal_functional_start",
        "unreal_functional_job",
    ),
    "animation_check": ("unreal_context", "unreal_workflow_inspect"),
    "runtime_ui": ("unreal_context", "unreal_workflow_inspect"),
    "performance_comparison": (
        "unreal_context",
        "unreal_performance_start",
        "unreal_performance_job",
        "unreal_performance_start",
        "unreal_performance_job",
        "unreal_performance_compare",
    ),
}


def get_recipe(recipe_id: RecipeId, locale: Locale = "en") -> Recipe:
    if locale not in _GUIDANCE or recipe_id not in _TOOLS:
        raise ValueError("Use an exact published recipe id and locale en, fr or es.")
    title, prerequisite, evidence = _GUIDANCE[locale][recipe_id]
    return Recipe(
        recipe_id=recipe_id,
        locale=locale,
        title=title,
        prerequisite=prerequisite,
        tool_sequence=_TOOLS[recipe_id],
        required_evidence=evidence,
        permission_note=_GUIDANCE[locale]["permission"],
        translation_status="source" if locale == "en" else "draft_requires_human_review",
    )


def recipe_catalog(locale: Locale = "en") -> dict:
    return {
        "locale": locale,
        "recipes": [get_recipe(recipe_id, locale).model_dump(mode="json") for recipe_id in _TOOLS],
        "executes_tools": False,
    }


def register_recipe_resources(server: FastMCP) -> None:
    @server.resource("jev://recipes")
    def beginner_recipe_index() -> dict:
        """Discover beginner workflows and the available language-specific resources."""
        return {
            **recipe_catalog(),
            "available_locales": ["en", "fr", "es"],
            "localized_resources": ["jev://recipes/en", "jev://recipes/fr", "jev://recipes/es"],
        }

    @server.resource("jev://recipes/{locale}")
    def beginner_recipe_catalog(locale: Locale) -> dict:
        """Read localized beginner workflows in English, French or Spanish; no tools execute."""
        return recipe_catalog(locale)

    @server.resource("jev://recipes/{locale}/{recipe_id}")
    def beginner_recipe(recipe_id: RecipeId, locale: Locale) -> dict:
        """Read one bounded workflow, its prerequisites and required evidence."""
        return get_recipe(recipe_id, locale).model_dump(mode="json")

    @server.prompt()
    def beginner_workflow(recipe_id: RecipeId, locale: Locale = "en") -> str:
        """Explain a beginner workflow in the requested language without granting permissions."""
        recipe = get_recipe(recipe_id, locale)
        return (
            f"{recipe.title}\n{recipe.prerequisite}\n"
            f"{' → '.join(recipe.tool_sequence)}\n{recipe.required_evidence}\n"
            f"{recipe.permission_note}\ntranslation_status: {recipe.translation_status}"
        )
