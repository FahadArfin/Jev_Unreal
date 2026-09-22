#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "JevEditorReviewPanel.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "InputCoreTypes.h"
#include "Layout/Children.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"

namespace JevReviewAccessibility
{
TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Root, const FName WidgetTag)
{
    if (Root->GetTag() == WidgetTag) return Root;
    FChildren* Children = Root->GetChildren();
    for (int32 Index = 0; Index < Children->Num(); ++Index)
        if (auto Match = Find(Children->GetChildAt(Index), WidgetTag)) return Match;
    return nullptr;
}

void CheckNames(FAutomationTestBase& Test, const TSharedRef<SWidget>& Root, const FString& Expected, int32& Count)
{
#if WITH_ACCESSIBILITY
    if (Root->SupportsKeyboardFocus())
    {
        ++Count;
        Test.TestEqual(TEXT("actual focusable descendant has the semantic field name"), Root->GetAccessibleText().ToString(), Expected);
    }
    FChildren* Children = Root->GetChildren();
    for (int32 Index = 0; Index < Children->Num(); ++Index) CheckNames(Test, Children->GetChildAt(Index), Expected, Count);
#endif
}

class FNarrowReview : public IAutomationLatentCommand
{
public:
    explicit FNarrowReview(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
    {
        for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            if (AActor* Actor = Cast<AActor>(*It)) Selected.Add(Actor);
        GEditor->SelectNone(false, true, false);
        Bridge = MakeUnique<FJevEditorBridge>();
        Content = FJevEditorReviewPanel::CreateForTesting(*Bridge, true, [this](const FString& Message)
        {
            ++Announcements;
            Test->TestTrue(TEXT("explicit announcement length is bounded"), Message.Len() > 0 && Message.Len() <= 512);
        });
        Window = SNew(SWindow).Title(FText::FromString(TEXT("Jev synthetic label expansion - not a translation")))
            .ClientSize(FVector2D(320, 720)).SupportsMaximize(false).SupportsMinimize(false)
            [ Content.ToSharedRef() ];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        ContinueAt = Started + 0.8;
    }

    virtual ~FNarrowReview() override { Cleanup(); }

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Started > 30)
        {
            Test->AddError(TEXT("Narrow review layout test timed out.")); Cleanup(); return true;
        }
        if (Now < ContinueAt) return false;
        if (Stage == 0)
        {
            Test->TestEqual(TEXT("initial and passive polling do not announce outcomes"), Announcements, 0);
            for (const TCHAR* Name : {TEXT("Inspect"), TEXT("ChangeLabel"), TEXT("ChangeFolder"), TEXT("PreviewTranslation"), TEXT("PreviewMetadata"), TEXT("Refresh"), TEXT("Apply"), TEXT("InspectApplied")})
            {
                const auto Widget = Control(Name);
                if (!Test->TestTrue(FString::Printf(TEXT("wrapped control exists: %s"), Name), Widget.IsValid())) { Cleanup(); return true; }
#if WITH_ACCESSIBILITY
                Test->TestFalse(TEXT("action has an explicit accessible name"), Widget->GetAccessibleText().IsEmpty());
#endif
            }
            const auto Inspect = Control(TEXT("Inspect"));
            const FVector2D Size = Inspect->GetCachedGeometry().GetLocalSize();
            Test->TestTrue(TEXT("narrow action stays inside 320-pixel panel"), Size.X > 100 && Size.X <= 296);
            Test->TestTrue(TEXT("synthetically expanded action wraps to multiple lines"), Size.Y > 30);
            Test->TestFalse(TEXT("layout work never enables unreviewed Apply"), Control(TEXT("Apply"))->IsEnabled());
            Capture(TEXT("ReviewPanelNarrowPseudo.png"));
            FSlateApplication::Get().SetKeyboardFocus(Inspect, EFocusCause::SetDirectly);
            FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::SpaceBar, FModifierKeysState(), 0, false, 0, 0));
            FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(EKeys::SpaceBar, FModifierKeysState(), 0, false, 0, 0));
            Test->TestEqual(TEXT("explicit failed inspection requests one announcement"), Announcements, 1);
            FocusAfterAction = FSlateApplication::Get().GetKeyboardFocusedWidget();
#if WITH_ACCESSIBILITY
            if (Test->TestTrue(TEXT("explicit error focuses a real text control"), FocusAfterAction.IsValid()))
                Test->TestEqual(TEXT("focused error text has semantic name"), FocusAfterAction->GetAccessibleText().ToString(), FString(TEXT("Action result and next step")));
#endif
            Stage = 1; ContinueAt = Now + 2.4; return false;
        }
        Test->TestEqual(TEXT("two-second status polling does not repeat announcement"), Announcements, 1);
        Test->TestTrue(TEXT("passive polling preserves focused result"), FSlateApplication::Get().GetKeyboardFocusedWidget() == FocusAfterAction);
        Capture(TEXT("ReviewPanelNarrowResult.png"));
        Test->AddInfo(TEXT("320px panel tested using expanded English labels, semantic focusable names and synthetic Slate keyboard events. Announcement hook observes requested messages, not OS delivery. Physical keyboards, actual translations, screen readers and representative artists remain separate human acceptance."));
        Cleanup(); return true;
    }

private:
    TSharedPtr<SWidget> Control(const TCHAR* Suffix) const
    {
        return Find(Content.ToSharedRef(), FName(*(FString(TEXT("Jev.Review.")) + Suffix)));
    }
    void Capture(const TCHAR* Filename)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test->TestTrue(TEXT("narrow panel native capture"), FSlateApplication::Get().TakeScreenshot(Content.ToSharedRef(), Pixels, Size))) return;
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        Test->TestTrue(TEXT("narrow review evidence saved"), FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Directory, Filename)));
    }
    void Cleanup()
    {
        if (Window)
        {
            Window->SetContent(SNullWidget::NullWidget);
            FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
            Window.Reset();
        }
        Content.Reset(); Bridge.Reset();
        if (bRestored) return;
        bRestored = true;
        GEditor->SelectNone(false, true, false);
        for (auto Actor : Selected) if (Actor.IsValid()) GEditor->SelectActor(Actor.Get(), true, false);
        GEditor->NoteSelectionChange();
    }
    FAutomationTestBase* Test;
    TUniquePtr<FJevEditorBridge> Bridge;
    TSharedPtr<SWidget> Content, FocusAfterAction;
    TSharedPtr<SWindow> Window;
    TArray<TWeakObjectPtr<AActor>> Selected;
    double Started = 0, ContinueAt = 0;
    int32 Stage = 0, Announcements = 0;
    bool bRestored = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewAccessibleNames, "Jev.Editor.ReviewAccessibleNames", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewAccessibleNames::RunTest(const FString& Parameters)
{
#if WITH_ACCESSIBILITY
    using namespace JevReviewAccessibility;
    FJevEditorBridge Bridge;
    const auto Content = FJevEditorReviewPanel::CreateForTesting(Bridge);
    const TMap<FString, FString> Expected = {
        {TEXT("Translate.X"), TEXT("Translate X, centimeters")}, {TEXT("Translate.Y"), TEXT("Translate Y, centimeters")},
        {TEXT("Translate.Z"), TEXT("Translate Z, centimeters")}, {TEXT("Label"), TEXT("New actor label")},
        {TEXT("Folder"), TEXT("New actor folder")}, {TEXT("Selection"), TEXT("Selection inspection")},
        {TEXT("Plan"), TEXT("Reviewed changes, before and after")}, {TEXT("Technical"), TEXT("Complete reviewed technical details")},
        {TEXT("Result"), TEXT("Action result and next step")}
    };
    for (const auto& Pair : Expected)
    {
        const auto Widget = Find(Content, FName(*(TEXT("Jev.Review.") + Pair.Key)));
        if (!TestTrue(TEXT("named input exists"), Widget.IsValid())) return false;
        TestTrue(TEXT("named field exposes an accessible representative"), Widget->IsAccessible());
        int32 Count = 0;
        CheckNames(*this, Widget.ToSharedRef(), Pair.Value, Count);
        TestTrue(TEXT("field contains actual keyboard-focusable widgets"), Count > 0);
    }
    return true;
#else
    AddError(TEXT("This acceptance test requires an engine built with accessibility enabled."));
    return false;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewNarrowAccessibility, "Jev.Rendered.ReviewNarrowAccessibility", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewNarrowAccessibility::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Rendered narrow-layout acceptance requires a rendered isolated editor.")); return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(JevReviewAccessibility::FNarrowReview(this));
    return true;
}

#endif
