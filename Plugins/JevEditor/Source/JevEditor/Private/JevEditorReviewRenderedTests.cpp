#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "JevEditorReviewPanel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Layout/Children.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace JevReviewRendered
{
TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget, FName Tag)
{
    if (Widget->GetTag() == Tag) return Widget;
    FChildren* Children = Widget->GetChildren();
    for (int32 Index = 0; Index < Children->Num(); ++Index)
        if (auto Found = Find(Children->GetChildAt(Index), Tag)) return Found;
    return nullptr;
}

class FCaptureReview : public IAutomationLatentCommand
{
public:
    FCaptureReview(FAutomationTestBase* InTest, TSharedPtr<SDockTab> InTab, bool bInAlreadyOpen)
        : Test(InTest), Tab(MoveTemp(InTab)), bAlreadyOpen(bInAlreadyOpen), Started(FPlatformTime::Seconds()) {}

    virtual bool Update() override
    {
        // Wait for normal Slate layout/paint; do not manually tick or block the editor.
        if (FPlatformTime::Seconds() - Started < 1.0) return false;
        if (!Tab.IsValid()) { Test->AddError(TEXT("Jev Review tab became unavailable.")); return true; }
        // SDockTab's own widget tree is its draggable header. The panel is hosted
        // separately by the docking stack and must be traversed/captured as content.
        const TSharedRef<SWidget> Content = Tab->GetContent();
        const FVector2D ContentSize = Content->GetCachedGeometry().GetLocalSize();
        Test->TestTrue(TEXT("Panel content has a meaningful arranged area (at least 240 by 200)"), ContentSize.X >= 240 && ContentSize.Y >= 200);
        if (bScrolledToApply)
        {
            const auto Apply = Find(Content, FName(TEXT("Jev.Review.Apply")));
            if (Test->TestTrue(TEXT("Apply remains present after native scrolling"), Apply.IsValid()))
            {
                const FGeometry& Geometry = Apply->GetCachedGeometry();
                const FVector2D Size = Geometry.GetLocalSize();
                Test->TestTrue(TEXT("Apply has nonzero arranged geometry after scrolling into view"), Size.X > 0 && Size.Y > 0);
                const FGeometry& ScrollGeometry = Scroll->GetCachedGeometry();
                const FVector2D Position = Geometry.GetAbsolutePosition();
                const FVector2D End = Position + Geometry.GetAbsoluteSize();
                const FVector2D ViewStart = ScrollGeometry.GetAbsolutePosition();
                const FVector2D ViewEnd = ViewStart + ScrollGeometry.GetAbsoluteSize();
                Test->TestTrue(TEXT("Apply is inside the visible scroll viewport after native scrolling"),
                    Position.Y >= ViewStart.Y - 1 && End.Y <= ViewEnd.Y + 1 && End.X > ViewStart.X && Position.X < ViewEnd.X);
                if (!bAlreadyOpen) Test->TestFalse(TEXT("Scrolling to Apply never enables it without review"), Apply->IsEnabled());
            }
            Capture(Content, TEXT("ReviewPanelActions.png"));
            Test->AddInfo(TEXT("Captured Saved/Automation/Jev/ReviewPanel.png and ReviewPanelActions.png before and after native scrolling. Controls were arranged across two views, not all at once. Jev.Rendered.ReviewWorkflow covers keyboard behavior separately; screen-reader and artist acceptance remain manual."));
            if (bAlreadyOpen) Scroll->SetScrollOffset(OriginalScrollOffset);
            else Tab->RequestCloseTab();
            return true;
        }
        for (const TCHAR* Name : {TEXT("Jev.Review.Root"), TEXT("Jev.Review.Status"), TEXT("Jev.Review.Inspect"),
            TEXT("Jev.Review.Translate.X"), TEXT("Jev.Review.Translate.Y"), TEXT("Jev.Review.Translate.Z"),
            TEXT("Jev.Review.Label"), TEXT("Jev.Review.Folder"), TEXT("Jev.Review.Plan"), TEXT("Jev.Review.Apply")})
        {
            const auto Widget = Find(Content, FName(Name));
            Test->TestTrue(FString::Printf(TEXT("Rendered control exists: %s"), Name), Widget.IsValid());
            if (!Widget) continue;
            // The initial viewport intentionally scrolls. Offscreen Apply receives
            // meaningful geometry only after bringing it into view on normal frames.
            if (FString(Name) != TEXT("Jev.Review.Apply"))
            {
                const FVector2D Size = Widget->GetCachedGeometry().GetLocalSize();
                Test->TestTrue(FString::Printf(TEXT("Control has a nonzero arranged size: %s"), Name), Size.X > 0 && Size.Y > 0);
            }
            if (!bAlreadyOpen && FString(Name) == TEXT("Jev.Review.Apply"))
                Test->TestFalse(TEXT("Opening a fresh tab never enables Apply without review"), Widget->IsEnabled());
#if WITH_ACCESSIBILITY
            if (FString(Name).Contains(TEXT("Translate.")) || FString(Name) == TEXT("Jev.Review.Label") || FString(Name) == TEXT("Jev.Review.Folder"))
                Test->TestFalse(FString::Printf(TEXT("Input has an accessible name: %s"), Name), Widget->GetAccessibleText().IsEmpty());
#endif
        }
        Capture(Content, TEXT("ReviewPanel.png"));
        const auto ScrollWidget = Find(Content, FName(TEXT("Jev.Review.Scroll")));
        const auto Apply = Find(Content, FName(TEXT("Jev.Review.Apply")));
        if (!Test->TestTrue(TEXT("Native scrolling control and Apply target exist"), ScrollWidget.IsValid() && Apply.IsValid()))
        {
            if (!bAlreadyOpen) Tab->RequestCloseTab();
            return true;
        }
        Scroll = StaticCastSharedPtr<SScrollBox>(ScrollWidget);
        OriginalScrollOffset = Scroll->GetScrollOffset();
        Scroll->ScrollDescendantIntoView(Apply, false);
        bScrolledToApply = true;
        Started = FPlatformTime::Seconds();
        return false;
    }

private:
    void Capture(const TSharedRef<SWidget>& Content, const TCHAR* Filename)
    {
        TArray<FColor> Pixels;
        FIntVector Size;
        if (Test->TestTrue(FString::Printf(TEXT("Native Slate panel-content screenshot captured: %s"), Filename), FSlateApplication::Get().TakeScreenshot(Content, Pixels, Size)))
        {
            if (!Test->TestTrue(TEXT("Screenshot contains panel content rather than just a tab header"), Size.X >= 240 && Size.Y >= 200 && static_cast<int64>(Size.X) * Size.Y == Pixels.Num()))
                return;
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev"));
            const FString Destination = FPaths::Combine(Directory, Filename);
            IFileManager::Get().MakeDirectory(*Directory, true);
            Test->TestTrue(TEXT("Review-panel PNG saved for human visual acceptance"), !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *Destination));
        }
    }

    FAutomationTestBase* Test;
    TSharedPtr<SDockTab> Tab;
    TSharedPtr<SScrollBox> Scroll;
    bool bAlreadyOpen;
    bool bScrolledToApply = false;
    float OriginalScrollOffset = 0;
    double Started;
};

bool Contains(const TSharedRef<SWidget>& Root, const TSharedPtr<SWidget>& Candidate)
{
    if (Root == Candidate) return true;
    FChildren* Children = Root->GetChildren();
    for (int32 Index = 0; Index < Children->Num(); ++Index)
        if (Contains(Children->GetChildAt(Index), Candidate)) return true;
    return false;
}

/** Real Slate events in a private test window; no network bridge or synthetic click delegates. */
class FReviewWorkflow : public IAutomationLatentCommand
{
public:
    FReviewWorkflow(FAutomationTestBase* InTest, UWorld* InWorld, AStaticMeshActor* InActor)
        : Test(InTest), World(InWorld), Actor(InActor), Started(FPlatformTime::Seconds())
    {
        Bridge = MakeUnique<FJevEditorBridge>([this] { return Now; });
        Bridge->OnApplyConsumedForTesting([this] { ++ApplyCount; });
        Content = FJevEditorReviewPanel::CreateForTesting(*Bridge);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("Jev review workflow acceptance")))
            .ClientSize(FVector2D(1000, 760)).SupportsMaximize(false).SupportsMinimize(false)
            [ Content.ToSharedRef() ];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
    }

    virtual ~FReviewWorkflow() override { Cleanup(); }

    virtual bool Update() override
    {
        if (FPlatformTime::Seconds() - Started > 45.0)
        {
            Test->AddError(TEXT("Rendered review workflow timed out; the fixture window and actor will be cleaned up."));
            Cleanup();
            return true;
        }
        if (FPlatformTime::Seconds() < ContinueAt) return false;
        if (!World.IsValid() || !Actor.IsValid() || GEditor->GetEditorWorldContext().World() != World.Get())
        {
            Test->AddError(TEXT("The isolated workflow world or actor changed unexpectedly."));
            Cleanup();
            return true;
        }

        switch (Stage++)
        {
        case 0:
            // Allow normal window layout and paint before routing keyboard events.
            Pause(0.6);
            return false;
        case 1:
        {
            for (const TCHAR* Tag : {TEXT("Inspect"), TEXT("Selection"), TEXT("Translate.X"), TEXT("Translate.Y"),
                TEXT("Translate.Z"), TEXT("PreviewTranslation"), TEXT("Label"), TEXT("Folder"), TEXT("PreviewMetadata"),
                TEXT("Refresh"), TEXT("Plan"), TEXT("Technical"), TEXT("TechnicalToggle"), TEXT("PlanStatus"),
                TEXT("Apply"), TEXT("InspectApplied"), TEXT("Result")})
                if (!Test->TestTrue(FString::Printf(TEXT("Workflow control exists: %s"), Tag), Control(Tag).IsValid())) return Finish();
            Test->TestFalse(TEXT("Fresh workflow cannot apply without a reviewed plan"), Control(TEXT("Apply"))->IsEnabled());
            Test->TestFalse(TEXT("Complete technical details begin collapsed"), StaticCastSharedPtr<SExpandableArea>(Control(TEXT("TechnicalToggle")))->IsExpanded());
            if (!Activate(TEXT("Inspect"))) return Finish();
            Test->TestTrue(TEXT("Explicit inspection focuses its selectable result"), FocusWithin(TEXT("Selection")));
            const auto Selection = TextBox(TEXT("Selection"));
            Test->TestTrue(TEXT("Inspection displays the exact selected actor"), Selection->GetText().ToString().Contains(TEXT("Rendered review cube")));
            if (!TabTo(TEXT("Translate.X")) || !TabTo(TEXT("Translate.Y")) || !TabTo(TEXT("Translate.Z")) ||
                !TabTo(TEXT("PreviewTranslation")) || !TabTo(TEXT("Label")) || !TabTo(TEXT("Folder")) ||
                !TabTo(TEXT("Refresh")) || !TabTo(TEXT("Plan")) || !TabTo(TEXT("Result"))) return Finish();
            // Reverse navigation must reach the same read-only review, not trap a keyboard user.
            if (!TabTo(TEXT("Plan"), true)) return Finish();
            if (!Focus(TEXT("Translate.X"))) return Finish();
            Press(EKeys::A, false, true);
            for (TCHAR Character : FString(TEXT("25")))
                FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(Character, FModifierKeysState(), 0, false));
            Press(EKeys::Tab); // Commit the edited number by normal focus navigation.
            if (!Activate(TEXT("PreviewTranslation"))) return Finish();
            Test->TestTrue(TEXT("Human preview focuses the read-only plan"), FocusWithin(TEXT("Plan")));
            Pause(0.1);
            return false;
        }
        case 2:
        {
            Test->TestEqual(TEXT("Preview does not execute an edit"), ApplyCount, 0);
            Test->TestTrue(TEXT("Human preview leaves actor transform untouched"), Actor->GetActorLocation().Equals(InitialLocation));
            if (!Test->TestTrue(TEXT("A valid human preview enables explicit Apply"), Control(TEXT("Apply"))->IsEnabled())) return Finish();
            PlanWidget = TextBox(TEXT("Plan"));
            PlanBody = PlanWidget->GetText().ToString();
            if (!Test->TestFalse(TEXT("Human review contains a populated plan"), PlanBody.IsEmpty())) return Finish();
            PlanWidget->SelectAllText();
            SelectedBody = PlanWidget->GetSelectedText().ToString();
            Test->TestFalse(TEXT("Review text is selectable for copying"), SelectedBody.IsEmpty());
            Press(EKeys::Enter);
            Press(EKeys::Enter, false, true);
            FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(TEXT('x'), FModifierKeysState(), 0, false));
            Test->TestEqual(TEXT("Read-only review rejects typed edits"), PlanWidget->GetText().ToString(), PlanBody);
            Test->TestEqual(TEXT("Enter and Control-Enter in review never execute Apply"), ApplyCount, 0);
            Test->TestTrue(TEXT("Enter in review retains focus"), FocusWithin(TEXT("Plan")));
            Now += 3.0;
            Pause(2.3);
            return false;
        }
        case 3:
            Test->TestTrue(TEXT("Periodic refresh preserves the same review widget"), PlanWidget == TextBox(TEXT("Plan")));
            Test->TestEqual(TEXT("Periodic refresh preserves the reviewed body"), PlanWidget->GetText().ToString(), PlanBody);
            Test->TestEqual(TEXT("Periodic refresh preserves text selection"), PlanWidget->GetSelectedText().ToString(), SelectedBody);
            Test->TestTrue(TEXT("Periodic refresh never steals review focus"), FocusWithin(TEXT("Plan")));
            Capture(TEXT("ReviewWorkflowPlan.png"));
            Capture(TEXT("ReviewWorkflowPlanBody.png"), Control(TEXT("Plan")));
            // Space must activate only after moving focus explicitly to the enabled Apply button.
            if (!Activate(TEXT("Apply"), EKeys::SpaceBar)) return Finish();
            Pause(0.1);
            return false;
        case 4:
            Test->TestEqual(TEXT("Explicit focused Space applies exactly once"), ApplyCount, 1);
            Test->TestTrue(TEXT("The typed 25 cm translation is actually applied"), Actor->GetActorLocation().Equals(FVector(125, 200, 300)));
            Test->TestFalse(TEXT("A consumed plan disables Apply"), Control(TEXT("Apply"))->IsEnabled());
            Test->TestTrue(TEXT("Apply outcome is keyboard accessible"), FocusWithin(TEXT("Result")));
            Press(EKeys::Enter);
            Test->TestEqual(TEXT("Enter on the outcome does not replay Apply"), ApplyCount, 1);
            if (!Activate(TEXT("InspectApplied"))) return Finish();
            Test->TestTrue(TEXT("Fresh inspection focuses the result"), FocusWithin(TEXT("Result")));
            Test->TestTrue(TEXT("Fresh actor inspection is visibly distinct from the apply receipt"),
                TextBox(TEXT("Result"))->GetText().ToString().Contains(TEXT("Fresh inspection")));
            Press(EKeys::A, false, true);
            ResultBody = TextBox(TEXT("Result"))->GetText().ToString();
            SelectedResult = TextBox(TEXT("Result"))->GetSelectedText().ToString();
            Test->TestFalse(TEXT("Fresh inspection is keyboard-selectable for copying"), SelectedResult.IsEmpty());
            // A passive refresh must not overwrite an explicit inspection once the
            // bridge's 15-minute receipt retention expires.
            Now += 901.0;
            Pause(2.3);
            return false;
        case 5:
            Test->TestEqual(TEXT("Passive receipt failure preserves the latest explicit result"), TextBox(TEXT("Result"))->GetText().ToString(), ResultBody);
            Test->TestEqual(TEXT("Passive receipt failure preserves the selected inspection text"), TextBox(TEXT("Result"))->GetSelectedText().ToString(), SelectedResult);
            Test->TestTrue(TEXT("Passive receipt failure preserves result focus"), FocusWithin(TEXT("Result")));
            Test->TestFalse(TEXT("An unavailable retained receipt cannot enable Apply"), Control(TEXT("Apply"))->IsEnabled());
            Test->TestTrue(TEXT("Passive receipt failure is reported separately in PlanStatus"),
                StaticCastSharedPtr<STextBlock>(Control(TEXT("PlanStatus")))->GetText().ToString().Contains(TEXT("unavailable"), ESearchCase::IgnoreCase));
            // The UI also handles a plan authored by another client through the same native path.
            if (!PreviewTranslation(FVector(25, -10, 5))) return Finish();
            if (!Activate(TEXT("Refresh"))) return Finish();
            Pause(0.1);
            return false;
        case 6:
            if (!Activate(*(FString(TEXT("Pending.")) + PlanId))) return Finish();
            Test->TestTrue(TEXT("Explicit pending-plan review focuses its body"), FocusWithin(TEXT("Plan")));
            Test->TestTrue(TEXT("Review contains the requested translated X coordinate"), TextBox(TEXT("Plan"))->GetText().ToString().Contains(TEXT("150.00")));
            PendingWidget = Control(*(FString(TEXT("Pending.")) + PlanId));
            if (!Focus(TEXT("Label"))) return Finish();
            Now += 3.0;
            Pause(2.3);
            return false;
        case 7:
            Test->TestTrue(TEXT("Periodic refresh does not steal focus from an input"), FocusWithin(TEXT("Label")));
            Test->TestTrue(TEXT("Unchanged pending list preserves button identity"), PendingWidget == Control(*(FString(TEXT("Pending.")) + PlanId)));
            // Expire native time, then let the panel's normal active timer observe the receipt.
            Now += 121.0;
            Pause(2.3);
            return false;
        case 8:
            Test->TestFalse(TEXT("Expiry disables Apply without requiring a failed execution"), Control(TEXT("Apply"))->IsEnabled());
            Test->TestTrue(TEXT("Expiry does not steal focus"), FocusWithin(TEXT("Label")));
            Test->TestTrue(TEXT("Expiry is visible in the status separate from the reviewed body"),
                StaticCastSharedPtr<STextBlock>(Control(TEXT("PlanStatus")))->GetText().ToString().Contains(TEXT("expired"), ESearchCase::IgnoreCase));
            Test->TestEqual(TEXT("Expired plan was never executed"), ApplyCount, 1);
            GEditor->SelectNone(false, true, false);
            if (!Activate(TEXT("PreviewTranslation"))) return Finish();
            Test->TestTrue(TEXT("An explicit preview error focuses the read-only result"), FocusWithin(TEXT("Result")));
            Test->TestTrue(TEXT("Empty-selection error includes the recovery action"),
                TextBox(TEXT("Result"))->GetText().ToString().Contains(TEXT("selection_empty")));
            Test->TestFalse(TEXT("A failed preview does not leave Apply enabled"), Control(TEXT("Apply"))->IsEnabled());
            Pause(0.3);
            return false;
        case 9:
            Capture(TEXT("ReviewWorkflowError.png"));
            Capture(TEXT("ReviewWorkflowErrorBody.png"), Control(TEXT("Result")));
            GEditor->SelectActor(Actor.Get(), true, false);
            if (!PreviewMesh()) return Finish();
            if (!Activate(TEXT("Refresh"))) return Finish();
            Pause(0.1);
            return false;
        case 10:
            if (!Activate(*(FString(TEXT("Pending.")) + PlanId))) return Finish();
            Test->TestTrue(TEXT("Mesh plan review is focused"), FocusWithin(TEXT("Plan")));
            Test->TestTrue(TEXT("Mesh review displays material-policy explanation"),
                TextBox(TEXT("Plan"))->GetText().ToString().Contains(TEXT("material"), ESearchCase::IgnoreCase));
            Test->TestTrue(TEXT("Native details are retained outside the concise review"),
                TextBox(TEXT("Technical"))->GetText().ToString().Contains(TEXT("mesh_settings")));
            Pause(0.3);
            return false;
        case 11:
            Capture(TEXT("ReviewWorkflowMesh.png"));
            Capture(TEXT("ReviewWorkflowMeshBody.png"), Control(TEXT("Plan")));
            Test->TestTrue(TEXT("Mesh preview leaves the source mesh unchanged"),
                Actor->GetStaticMeshComponent()->GetStaticMesh()->GetPathName() == TEXT("/Engine/BasicShapes/Cube.Cube"));
            Test->TestEqual(TEXT("Viewing a mesh preview never executes it"), ApplyCount, 1);
            if (!TabToTechnicalHeader()) return Finish();
            Press(EKeys::SpaceBar);
            Pause(0.3);
            return false;
        case 12:
        {
            Test->TestTrue(TEXT("Space on the technical disclosure expands its details"), StaticCastSharedPtr<SExpandableArea>(Control(TEXT("TechnicalToggle")))->IsExpanded());
            if (!TabTo(TEXT("Technical"))) return Finish();
            Press(EKeys::A, false, true);
            const auto Technical = TextBox(TEXT("Technical"));
            Test->TestFalse(TEXT("Keyboard users can select the complete technical details for copying"), Technical->GetSelectedText().IsEmpty());
            const FString Before = Technical->GetText().ToString();
            FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(TEXT('x'), FModifierKeysState(), 0, false));
            Test->TestEqual(TEXT("Technical details remain read-only"), Technical->GetText().ToString(), Before);
            if (!TabToTechnicalHeader(true)) return Finish();
            Press(EKeys::SpaceBar);
            Pause(0.3);
            return false;
        }
        case 13:
            Test->TestFalse(TEXT("Space collapses technical details again"), StaticCastSharedPtr<SExpandableArea>(Control(TEXT("TechnicalToggle")))->IsExpanded());
            if (!TabTo(TEXT("Apply"))) return Finish();
            Test->TestFalse(TEXT("Collapsed technical content does not trap keyboard focus"), FocusWithin(TEXT("Technical")));
            Test->TestEqual(TEXT("Disclosure navigation never executes the mesh plan"), ApplyCount, 1);
            Test->AddInfo(TEXT("Native Slate Tab/Shift-Tab, preview focus, read-only review and technical disclosure, explicit Space apply, fresh inspection, passive receipt failure, periodic refresh, expiry, and error recovery passed. Three full-window PNGs and three native text-widget PNGs were captured. Screen-reader output and physical keyboard/artist acceptance are not measured by this automation."));
            return Finish();
        default:
            return Finish();
        }
    }

private:
    TSharedPtr<SWidget> Control(const TCHAR* Suffix) const
    {
        return Find(Content.ToSharedRef(), FName(*(FString(TEXT("Jev.Review.")) + Suffix)));
    }

    TSharedPtr<SMultiLineEditableTextBox> TextBox(const TCHAR* Suffix) const
    {
        return StaticCastSharedPtr<SMultiLineEditableTextBox>(Control(Suffix));
    }

    bool FocusWithin(const TCHAR* Suffix) const
    {
        const auto Widget = Control(Suffix);
        return Widget && Contains(Widget.ToSharedRef(), FSlateApplication::Get().GetKeyboardFocusedWidget());
    }

    bool Focus(const TCHAR* Suffix)
    {
        const auto Widget = Control(Suffix);
        if (!Test->TestTrue(FString::Printf(TEXT("Focus target exists: %s"), Suffix), Widget.IsValid())) return false;
        FSlateApplication::Get().SetKeyboardFocus(Widget, EFocusCause::Navigation);
        return Test->TestTrue(FString::Printf(TEXT("Keyboard focus enters: %s"), Suffix), FocusWithin(Suffix));
    }

    void Press(FKey Key, bool bShift = false, bool bControl = false)
    {
        const FModifierKeysState Modifiers(bShift, false, bControl, false, false, false, false, false, false);
        const FKeyEvent Event(Key, Modifiers, 0, false, 0, 0);
        FSlateApplication::Get().ProcessKeyDownEvent(Event);
        FSlateApplication::Get().ProcessKeyUpEvent(Event);
    }

    bool TabTo(const TCHAR* Suffix, bool bBackwards = false)
    {
        for (int32 Count = 0; Count < 32; ++Count)
        {
            Press(EKeys::Tab, bBackwards);
            if (FocusWithin(Suffix)) return true;
        }
        Test->AddError(FString::Printf(TEXT("%s cannot reach %s within the bounded review window."), bBackwards ? TEXT("Shift-Tab") : TEXT("Tab"), Suffix));
        return false;
    }

    bool Activate(const TCHAR* Suffix, FKey Key = EKeys::Enter)
    {
        const auto Widget = Control(Suffix);
        if (!Widget || !Test->TestTrue(FString::Printf(TEXT("Explicit action is enabled: %s"), Suffix), Widget->IsEnabled()) || !Focus(Suffix)) return false;
        Press(Key);
        return true;
    }

    bool TabToTechnicalHeader(bool bBackwards = false)
    {
        for (int32 Count = 0; Count < 32; ++Count)
        {
            Press(EKeys::Tab, bBackwards);
            if (FocusWithin(TEXT("TechnicalToggle")) && !FocusWithin(TEXT("Technical"))) return true;
        }
        Test->AddError(TEXT("Keyboard navigation could not reach the technical-details disclosure header."));
        return false;
    }

    bool PreviewTranslation(const FVector& Delta)
    {
        const auto Response = FJevEditorReviewPanel::PreviewSelection(*Bridge, &Delta, nullptr, nullptr);
        if (!Test->TestTrue(TEXT("External-client translation preview succeeds"), Response->GetBoolField(TEXT("ok")))) return false;
        PlanId = Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"));
        return true;
    }

    bool PreviewMesh()
    {
        auto Operation = MakeShared<FJsonObject>();
        Operation->SetStringField(TEXT("op"), TEXT("replace_mesh"));
        Operation->SetStringField(TEXT("actor_path"), Actor->GetPathName());
        Operation->SetStringField(TEXT("asset_path"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
        Operation->SetStringField(TEXT("material_policy"), TEXT("preserve_slots"));
        auto Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Operation)});
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("preview"));
        Request->SetObjectField(TEXT("params"), Params);
        const auto Response = Bridge->Execute(Request);
        if (!Test->TestTrue(TEXT("External-client mesh preview succeeds"), Response->GetBoolField(TEXT("ok")))) return false;
        PlanId = Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"));
        return true;
    }

    void Capture(const TCHAR* Filename, TSharedPtr<SWidget> Widget = nullptr)
    {
        const bool bSubtree = Widget.IsValid();
        if (!Widget) Widget = Content;
        TArray<FColor> Pixels;
        FIntVector Size;
        if (!Test->TestTrue(FString::Printf(TEXT("Rendered workflow screenshot captured: %s"), Filename),
            FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size))) return;
        if (!Test->TestTrue(TEXT("Workflow capture has meaningful rendered content"), Size.X >= 240 && Size.Y >= (bSubtree ? 20 : 200) && static_cast<int64>(Size.X) * Size.Y == Pixels.Num())) return;
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        Test->TestTrue(FString::Printf(TEXT("Populated review screenshot saved: %s"), Filename),
            !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Directory, Filename)));
    }

    void Pause(double Seconds) { ContinueAt = FPlatformTime::Seconds() + Seconds; }
    bool Finish() { Cleanup(); return true; }

    void Cleanup()
    {
        if (bCleaned) return;
        bCleaned = true;
        // Drop the widget tree before its local bridge; never retain a dangling access pointer.
        if (Window)
        {
            if (Content && Contains(Content.ToSharedRef(), FSlateApplication::Get().GetKeyboardFocusedWidget()))
                FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::Cleared);
            Window->SetContent(SNullWidget::NullWidget);
            Window->RequestDestroyWindow();
        }
        PlanWidget.Reset();
        PendingWidget.Reset();
        Content.Reset();
        Window.Reset();
        Bridge.Reset();
        if (World.IsValid() && Actor.IsValid() && GEditor && GEditor->GetEditorWorldContext().World() == World.Get())
        {
            GEditor->SelectActor(Actor.Get(), false, false);
            World->EditorDestroyActor(Actor.Get(), false);
        }
    }

    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AStaticMeshActor> Actor;
    const FVector InitialLocation = FVector(100, 200, 300);
    double Now = 1000;
    TUniquePtr<FJevEditorBridge> Bridge;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SWidget> Content;
    TSharedPtr<SMultiLineEditableTextBox> PlanWidget;
    TSharedPtr<SWidget> PendingWidget;
    FString PlanBody;
    FString SelectedBody;
    FString ResultBody;
    FString SelectedResult;
    FString PlanId;
    int32 ApplyCount = 0;
    int32 Stage = 0;
    double Started;
    double ContinueAt = 0;
    bool bCleaned = false;
};
}

// Deliberately outside Jev.Editor: a NullRHI suite must never claim visual acceptance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewPanelRenderedTest, "Jev.Rendered.ReviewPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewPanelRenderedTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Jev.Rendered.ReviewPanel requires a rendered editor. Run it separately from NullRHI automation."));
        return false;
    }
    const FTabId Id(FName(TEXT("JevReview")));
    const bool bAlreadyOpen = FGlobalTabmanager::Get()->FindExistingLiveTab(Id).IsValid();
    const auto Tab = FGlobalTabmanager::Get()->TryInvokeTab(Id);
    if (!TestTrue(TEXT("Window-menu review tab opens"), Tab.IsValid())) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<JevReviewRendered::FCaptureReview>(this, Tab, bAlreadyOpen));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewWorkflowRenderedTest, "Jev.Rendered.ReviewWorkflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewWorkflowRenderedTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Jev.Rendered.ReviewWorkflow requires a rendered editor; NullRHI cannot validate focus or keyboard navigation."));
        return false;
    }
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("JevSandbox"))
    {
        AddError(TEXT("This mutating rendered acceptance test is restricted to the isolated JevSandbox project."));
        return false;
    }
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated review workflow world"), World)) return false;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanPathsSynchronous({TEXT("/Engine/BasicShapes")});
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Engine Cube fixture mesh"), Mesh)) return false;
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags |= RF_Transactional;
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector(100, 200, 300), FRotator::ZeroRotator, Spawn);
    if (!TestNotNull(TEXT("Native workflow fixture actor"), Actor)) return false;
    Actor->GetStaticMeshComponent()->SetFlags(RF_Transactional);
    Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
    Actor->SetActorLabel(TEXT("Rendered review cube"));
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(Actor, true, false);
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<JevReviewRendered::FReviewWorkflow>(this, World, Actor));
    return true;
}

#endif
