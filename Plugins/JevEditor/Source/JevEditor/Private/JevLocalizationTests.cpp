#if WITH_DEV_AUTOMATION_TESTS

#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevLocalizationSwitch, "Jev.Localization.CultureSwitch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevLocalizationSwitch::RunTest(const FString&)
{
    auto& Internationalization = FInternationalization::Get(); const FString PreviousLanguage = Internationalization.GetCurrentLanguage()->GetName(); const FString PreviousLocale = Internationalization.GetCurrentLocale()->GetName();
    ON_SCOPE_EXIT { Internationalization.SetCurrentLanguage(PreviousLanguage); Internationalization.SetCurrentLocale(PreviousLocale); FTextLocalizationManager::Get().RefreshResources(); };
    const FText Inspect = NSLOCTEXT("JevEditorReviewPanel", "Inspect", "Inspect selected actors");
    const FText Apply = NSLOCTEXT("JevEditorReviewPanel", "Apply", "Apply reviewed plan once");
    Internationalization.SetCurrentLanguageAndLocale(TEXT("fr")); FTextLocalizationManager::Get().RefreshResources();
    TestEqual(TEXT("French native panel text loads from compiled resource"), Inspect.ToString(), FString(TEXT("Inspecter les acteurs sélectionnés"))); TestEqual(TEXT("French apply keeps one-shot meaning"), Apply.ToString(), FString(TEXT("Appliquer une seule fois le plan vérifié")));
    Internationalization.SetCurrentLanguageAndLocale(TEXT("es")); FTextLocalizationManager::Get().RefreshResources();
    TestEqual(TEXT("Spanish native panel text switches in the same session"), Inspect.ToString(), FString(TEXT("Inspeccionar actores seleccionados"))); TestEqual(TEXT("Spanish apply keeps one-shot meaning"), Apply.ToString(), FString(TEXT("Aplicar una sola vez el plan revisado")));
    Internationalization.SetCurrentLanguageAndLocale(TEXT("en")); FTextLocalizationManager::Get().RefreshResources();
    TestEqual(TEXT("English source restored"), Inspect.ToString(), FString(TEXT("Inspect selected actors"))); return true;
}

#endif
