#include "JevEditorReviewPanel.h"

#include "JevEditorBridge.h"
#include "JevEditorReviewPresentation.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/App.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "JevEditorReviewPanel"

struct FJevReviewAccess
{
    FJevEditorBridge* Bridge = nullptr;
    FString DisabledReason = LOCTEXT("DisabledBridge", "Bridge disabled. Set JEV_BRIDGE_TOKEN before launching Unreal; see Jev_Unreal setup instructions.").ToString();
};

namespace JevReview
{
const FName TabName(TEXT("JevReview"));

TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const TCHAR* Action, const TSharedRef<FJsonObject>& Params = MakeShared<FJsonObject>())
{
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), Action);
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

FString String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    if (Object) Object->TryGetStringField(Field, Value);
    return Value;
}

FString DisplayVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object || !Object->TryGetArrayField(Field, Values) || Values->Num() != 3) return LOCTEXT("Unavailable", "unavailable").ToString();
    return FString::Printf(TEXT("(%s, %s, %s)"), *FString::SanitizeFloat((*Values)[0]->AsNumber(), 2), *FString::SanitizeFloat((*Values)[1]->AsNumber(), 2), *FString::SanitizeFloat((*Values)[2]->AsNumber(), 2));
}

}

TSharedRef<FJsonObject> FJevEditorReviewPanel::InspectSelection(FJevEditorBridge& Bridge)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetNumberField(TEXT("limit"), 20);
    const auto Context = JevReview::Call(Bridge, TEXT("context"), Params);
    if (!Context->GetBoolField(TEXT("ok"))) return Context;
    const auto Result = Context->GetObjectField(TEXT("result"));
    const auto Paths = Result->GetArrayField(TEXT("selected_actor_paths"));
    if (Result->GetBoolField(TEXT("selection_truncated")) || Paths.Num() > 20)
        return FJevEditorBridge::Error(TEXT("selection_too_large"), TEXT("Select at most 20 exact actors before inspecting or previewing edits."));
    if (Paths.IsEmpty()) return FJevEditorBridge::Error(TEXT("selection_empty"), TEXT("Select one to 20 actors in the World Outliner or viewport, then inspect again."));
    auto Details = MakeShared<FJsonObject>();
    Details->SetArrayField(TEXT("actor_paths"), Paths);
    return JevReview::Call(Bridge, TEXT("actor_details"), Details);
}

TSharedRef<FJsonObject> FJevEditorReviewPanel::PreviewSelection(FJevEditorBridge& Bridge, const FVector* Translation, const FString* Label, const FString* Folder)
{
    if ((!Translation && !Label && !Folder) || (Translation && (Label || Folder)))
        return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Preview a translation or selected metadata fields, separately."));
    if (Translation && (Translation->ContainsNaN() || Translation->GetAbsMax() > 1000000))
        return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Translation values must be finite and between -1,000,000 and 1,000,000 cm."));
    const auto Inspection = InspectSelection(Bridge);
    if (!Inspection->GetBoolField(TEXT("ok"))) return Inspection;
    const auto Details = Inspection->GetObjectField(TEXT("result"));
    const auto Actors = Details->GetArrayField(TEXT("actors"));
    if (Label && Actors.Num() != 1)
        return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Select exactly one actor when changing its label. Folder changes may target up to 20 actors."));
    TArray<TSharedPtr<FJsonValue>> Operations;
    for (const auto& Value : Actors)
    {
        const auto Actor = Value->AsObject();
        if (!Actor->GetBoolField(TEXT("editable")))
            return FJevEditorBridge::Error(TEXT("actor_unsupported"), TEXT("Every selected actor must be an editable native StaticMeshActor without locks or attachments. Inspect selection for blockers."));
        auto Operation = MakeShared<FJsonObject>();
        Operation->SetStringField(TEXT("actor_path"), Actor->GetStringField(TEXT("path")));
        if (Translation)
        {
            Operation->SetStringField(TEXT("op"), TEXT("set_transform"));
            const auto Location = Actor->GetArrayField(TEXT("location"));
            TArray<TSharedPtr<FJsonValue>> Target;
            for (int32 Axis = 0; Axis < 3; ++Axis) Target.Add(MakeShared<FJsonValueNumber>(Location[Axis]->AsNumber() + (*Translation)[Axis]));
            Operation->SetArrayField(TEXT("location"), Target);
        }
        else
        {
            Operation->SetStringField(TEXT("op"), TEXT("set_metadata"));
            if (Label) Operation->SetStringField(TEXT("label"), *Label);
            if (Folder) Operation->SetStringField(TEXT("folder"), *Folder);
        }
        Operations.Add(MakeShared<FJsonValueObject>(Operation));
    }
    auto Expected = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) Expected->SetStringField(Key, Details->GetStringField(Key));
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("operations"), Operations);
    Params->SetObjectField(TEXT("expected_state"), Expected);
    return JevReview::Call(Bridge, TEXT("preview"), Params);
}

FString FJevEditorReviewPanel::DescribeReview(const TSharedPtr<FJsonObject>& Record)
{
    const auto Presentation = FJevEditorReviewPresentation::Build(Record);
    return Presentation.Summary.ToString() + TEXT("\n\n") + Presentation.Body.ToString()
        + TEXT("\n\n") + Presentation.TechnicalDetails.ToString();
}

class SJevReviewWidget : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SJevReviewWidget) {}
        SLATE_ARGUMENT(TSharedPtr<FJevReviewAccess>, Access)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Access = Args._Access;
        ChildSlot
        [
            SNew(SBorder).Tag(TEXT("Jev.Review.Root")).Padding(12)
            [
                SAssignNew(RootScroll, SScrollBox).Tag(TEXT("Jev.Review.Scroll")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                + SScrollBox::Slot()
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ Heading(LOCTEXT("Title", "Jev — inspect, review, apply")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(STextBlock).Tag(TEXT("Jev.Review.Status")).Text_Lambda([this]
                        { return Access->Bridge ? LOCTEXT("Connected", "Connected to this editor. Preview changes locally, review them, then apply once.") : FText::FromString(Access->DisabledReason); }).AutoWrapText(true) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [ SNew(STextBlock).Text(LOCTEXT("KeyboardHelp", "Tab / Shift+Tab move between controls. Review text supports selection, copying and arrow-key scrolling. Apply has no global shortcut.")).AutoWrapText(true) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ Heading(LOCTEXT("InspectHeading", "1. Inspect and prepare")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.Inspect")).Text(LOCTEXT("Inspect", "Inspect selected actors")).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; }).OnClicked(this, &SJevReviewWidget::Inspect) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                    [ SNew(SBox).MaxDesiredHeight(160)
                        [ ReadOnly(SelectionBox, TEXT("Jev.Review.Selection"), LOCTEXT("SelectionName", "Selection inspection"),
                            LOCTEXT("SelectionHint", "Select actors in the World Outliner, then inspect. Only supported native static mesh actors can be edited.")) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                    [ SNew(STextBlock).Text(LOCTEXT("Translation", "Translate selection (centimeters)")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0) [ Coordinate(0, TEXT("X")) ]
                        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0) [ Coordinate(1, TEXT("Y")) ]
                        + SHorizontalBox::Slot().FillWidth(1) [ Coordinate(2, TEXT("Z")) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.PreviewTranslation")).Text(LOCTEXT("PreviewTranslation", "Preview translation")).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; }).OnClicked_Lambda([this] { return Preview(true); }) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(SCheckBox).Tag(TEXT("Jev.Review.ChangeLabel")).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bLabel = State == ECheckBoxState::Checked; })
                        [ SNew(STextBlock).Text(LOCTEXT("ChangeLabel", "Change label — select exactly one actor")) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 8)
                    [ SNew(SEditableTextBox).Tag(TEXT("Jev.Review.Label")).AccessibleText(LOCTEXT("LabelName", "New actor label"))
                        .ToolTipText(LOCTEXT("LabelHelp", "New actor label, 1 to 80 characters. Enable Change label to include it in the preview."))
                        .HintText(LOCTEXT("LabelHint", "Actor label (1–80 characters)"))
                        .IsEnabled_Lambda([this] { return Access->Bridge != nullptr; })
                        .OnTextChanged_Lambda([this](const FText& Text) { Label = Text.ToString(); }) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(SCheckBox).Tag(TEXT("Jev.Review.ChangeFolder")).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bFolder = State == ECheckBoxState::Checked; })
                        [ SNew(STextBlock).Text(LOCTEXT("ChangeFolder", "Change folder — leave empty to move to root")) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 6)
                    [ SNew(SEditableTextBox).Tag(TEXT("Jev.Review.Folder")).AccessibleText(LOCTEXT("FolderName", "New actor folder"))
                        .ToolTipText(LOCTEXT("FolderHelp", "Relative folder path. An empty value moves actors to root when Change folder is enabled."))
                        .HintText(LOCTEXT("FolderHint", "Example: Environment/Props"))
                        .IsEnabled_Lambda([this] { return Access->Bridge != nullptr; })
                        .OnTextChanged_Lambda([this](const FText& Text) { Folder = Text.ToString(); }) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.PreviewMetadata")).Text(LOCTEXT("PreviewMetadata", "Preview label / folder"))
                        .IsEnabled_Lambda([this] { return Access->Bridge && (bLabel || bFolder); }).OnClicked_Lambda([this] { return Preview(false); }) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ Heading(LOCTEXT("ReviewHeading", "2. Review exact changes")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.Refresh")).Text(LOCTEXT("RefreshPlans", "Refresh pending plans"))
                        .IsEnabled_Lambda([this] { return Access->Bridge != nullptr; })
                        .OnClicked_Lambda([this] { if (!SelectedPlanId.IsEmpty()) Review(SelectedPlanId, false, true); RefreshPlans(true); return FReply::Handled(); }) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                    [ SNew(SBox).MaxDesiredHeight(140) [ SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                        + SScrollBox::Slot() [ SAssignNew(PendingBox, SVerticalBox) ] ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(STextBlock).Tag(TEXT("Jev.Review.PlanSummary")).Text_Lambda([this] { return PlanSummary; }).AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(STextBlock).Tag(TEXT("Jev.Review.PlanStatus")).Text(this, &SJevReviewWidget::StatusText).AutoWrapText(true) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SNew(SBox).MaxDesiredHeight(320)
                        [ ReadOnly(PlanBox, TEXT("Jev.Review.Plan"), LOCTEXT("PlanName", "Reviewed changes, before and after"),
                            LOCTEXT("NoReview", "Choose a pending plan, or preview an edit to the current selection.")) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                    [ SNew(SExpandableArea).Tag(TEXT("Jev.Review.TechnicalToggle")).InitiallyCollapsed(true)
                        .HeaderContent()[ SNew(STextBlock).Text(LOCTEXT("TechnicalHeading", "Technical details — exact reviewed records")) ]
                        .BodyContent()[ SNew(SBox).MaxDesiredHeight(260)
                            [ ReadOnly(TechnicalBox, TEXT("Jev.Review.Technical"), LOCTEXT("TechnicalName", "Complete reviewed technical details"),
                                LOCTEXT("NoTechnical", "Choose a plan to inspect its complete technical details.")) ] ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ Heading(LOCTEXT("ApplyHeading", "3. Apply once, then verify")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(STextBlock).Text(LOCTEXT("ApplyHelp", "Apply uses the exact reviewed plan, even if preparation inputs change. Scene changes or expiry can reject it. Edits use Unreal Undo; Apply requests no save.")).AutoWrapText(true) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.Apply")).Text(LOCTEXT("Apply", "Apply reviewed plan once")).IsEnabled(this, &SJevReviewWidget::CanApply).OnClicked(this, &SJevReviewWidget::Apply) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.InspectApplied")).Text(LOCTEXT("InspectApplied", "Inspect applied actors now"))
                        .IsEnabled_Lambda([this] { return Access->Bridge && !AppliedActorPaths.IsEmpty(); }).OnClicked(this, &SJevReviewWidget::InspectApplied) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(SBox).MaxDesiredHeight(240)
                        [ ReadOnly(ResultBox, TEXT("Jev.Review.Result"), LOCTEXT("ResultName", "Action result and next step"),
                            LOCTEXT("NoResult", "Results and recovery steps appear here. Fresh inspection is separate from verification of your task requirements.")) ] ]
                ]
            ]
        ];
        RefreshPlans();
        RegisterActiveTimer(2.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SJevReviewWidget::RefreshTimer));
    }

private:
    static TSharedRef<SWidget> Heading(const FText& Text)
    {
        return SNew(STextBlock).Text(Text).Font(FAppStyle::GetFontStyle("HeadingExtraSmall"));
    }

    TSharedRef<SWidget> ReadOnly(TSharedPtr<SMultiLineEditableTextBox>& Box, FName WidgetTag, const FText& Name, const FText& Text)
    {
        return SAssignNew(Box, SMultiLineEditableTextBox).Tag(WidgetTag).AccessibleText(Name).ToolTipText(Name)
            .Text(Text).IsReadOnly(true).AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
            .ClearKeyboardFocusOnCommit(false).ClearTextSelectionOnFocusLoss(false).IsCaretMovedWhenGainFocus(false)
            .Padding(8).OnKeyDownHandler_Lambda([](const FGeometry&, const FKeyEvent& Event)
            {
                // Reading or copying never dispatches an action, commits a field or moves focus to Apply.
                return Event.GetKey() == EKeys::Enter ? FReply::Handled() : FReply::Unhandled();
            });
    }

    void Focus(const TSharedPtr<SMultiLineEditableTextBox>& Box)
    {
        if (!Box) return;
        RootScroll->ScrollDescendantIntoView(Box, false, EDescendantScrollDestination::IntoView);
        FSlateApplication::Get().SetKeyboardFocus(Box, EFocusCause::SetDirectly);
    }

    TSharedRef<SWidget> Coordinate(int32 Axis, const TCHAR* Name)
    {
        const FText AccessibleName = FText::Format(LOCTEXT("CoordinateName", "Translate {0}, centimeters"), FText::FromString(Name));
        return SNew(SNumericEntryBox<double>).Tag(FName(*FString::Printf(TEXT("Jev.Review.Translate.%s"), Name)))
            .AccessibleText(AccessibleName).ToolTipText(AccessibleName).AllowSpin(true).MinValue(-1000000).MaxValue(1000000)
            .IsEnabled_Lambda([this] { return Access->Bridge != nullptr; })
            .Value_Lambda([this, Axis] { return TOptional<double>(Translation[Axis]); })
            .OnValueChanged_Lambda([this, Axis](double Value) { Translation[Axis] = Value; })
            .Label()[ SNew(STextBlock).Text(FText::FromString(Name)) ];
    }

    FText DescribeActors(const TSharedPtr<FJsonObject>& Details) const
    {
        TArray<FString> Lines;
        Lines.Add(FText::Format(LOCTEXT("InspectionProject", "Project: {0}"), FText::FromString(JevReview::String(Details, TEXT("project_file")))).ToString());
        for (const auto& Value : Details->GetArrayField(TEXT("actors")))
        {
            const auto Actor = Value->AsObject();
            const FString FolderName = JevReview::String(Actor, TEXT("folder"));
            Lines.Add(FText::Format(LOCTEXT("ActorInspection", "\n{0}\n{1}\nLocation (X, Y, Z), cm: {2}\nRotation (pitch, yaw, roll), degrees: {3}\nScale: {4}\nFolder: {5}"),
                FText::FromString(JevReview::String(Actor, TEXT("label"))), FText::FromString(JevReview::String(Actor, TEXT("path"))),
                FText::FromString(JevReview::DisplayVector(Actor, TEXT("location"))), FText::FromString(JevReview::DisplayVector(Actor, TEXT("rotation"))),
                FText::FromString(JevReview::DisplayVector(Actor, TEXT("scale"))), FolderName.IsEmpty() ? LOCTEXT("RootFolder", "(root)") : FText::FromString(FolderName)).ToString());
            const TSharedPtr<FJsonObject>* Bounds = nullptr;
            if (Actor->TryGetObjectField(TEXT("bounds_cm"), Bounds))
                Lines.Add(FText::Format(LOCTEXT("WorldBounds", "World bounds size (X, Y, Z), cm: {0}"), FText::FromString(JevReview::DisplayVector(*Bounds, TEXT("size")))).ToString());
            if (!Actor->GetBoolField(TEXT("editable")))
            {
                TArray<FString> Blockers;
                for (const auto& Blocker : Actor->GetArrayField(TEXT("edit_blockers"))) Blockers.Add(Blocker->AsString());
                Lines.Add(FText::Format(LOCTEXT("EditingBlocked", "Editing blocked: {0}"), FText::FromString(FString::Join(Blockers, TEXT(", ")))).ToString());
            }
            else Lines.Add(LOCTEXT("EditableActor", "Editable native static mesh actor.").ToString());
        }
        return FText::FromString(FString::Join(Lines, TEXT("\n")));
    }

    void ShowResult(const FText& Text, bool bFocus)
    {
        if (!ResultBox->GetText().EqualTo(Text)) ResultBox->SetText(Text);
        if (bFocus) Focus(ResultBox);
    }

    static FText ErrorMessage(const TSharedPtr<FJsonObject>& Response, bool bApplyAttempted)
    {
        const TSharedPtr<FJsonObject>* Error = nullptr;
        FString Code(TEXT("unexpected_response")), Message;
        if (Response && Response->TryGetObjectField(TEXT("error"), Error))
        {
            Code = JevReview::String(*Error, TEXT("code"));
            Message = JevReview::String(*Error, TEXT("message"));
        }
        return FJevEditorReviewPresentation::RecoveryMessage(Code, Message, bApplyAttempted);
    }

    void ShowError(const TSharedPtr<FJsonObject>& Response, bool bApplyAttempted, bool bFocus)
    {
        ShowResult(ErrorMessage(Response, bApplyAttempted), bFocus);
    }

    FReply Inspect()
    {
        if (!Access->Bridge) return FReply::Handled();
        const auto Response = FJevEditorReviewPanel::InspectSelection(*Access->Bridge);
        if (Response->GetBoolField(TEXT("ok")))
        {
            SelectionBox->SetText(DescribeActors(Response->GetObjectField(TEXT("result"))));
            Focus(SelectionBox);
        }
        else ShowError(Response, false, true);
        return FReply::Handled();
    }

    void ClearReview()
    {
        Reviewed.Reset();
        SelectedPlanId.Empty();
        DisplayedPlanId.Empty();
        bPresentationValid = false;
        ReceiptDiagnostic = FText::GetEmpty();
        PlanSummary = FText::GetEmpty();
        PlanBox->SetText(LOCTEXT("NoReview", "Choose a pending plan, or preview an edit to the current selection."));
        TechnicalBox->SetText(LOCTEXT("NoTechnical", "Choose a plan to inspect its complete technical details."));
    }

    FReply Preview(bool bTranslation)
    {
        if (!Access->Bridge) return FReply::Handled();
        const auto Response = FJevEditorReviewPanel::PreviewSelection(*Access->Bridge, bTranslation ? &Translation : nullptr,
            !bTranslation && bLabel ? &Label : nullptr, !bTranslation && bFolder ? &Folder : nullptr);
        if (Response->GetBoolField(TEXT("ok")))
        {
            ShowResult(LOCTEXT("PreviewCreated", "Preview created without editing the scene. Read Before and After, including any copy limits, before applying this exact plan."), false);
            Review(Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")), true, true);
        }
        else { ClearReview(); ShowError(Response, false, true); }
        RefreshPlans();
        return FReply::Handled();
    }

    void Review(FString PlanId, bool bFocusReview = false, bool bFocusError = false)
    {
        if (!Access->Bridge) { Reviewed.Reset(); bPresentationValid = false; return; }
        SelectedPlanId = PlanId;
        auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("plan_id"), PlanId);
        const auto Response = JevReview::Call(*Access->Bridge, TEXT("plan_status"), Params);
        if (!Response->GetBoolField(TEXT("ok")))
        {
            Reviewed.Reset();
            bPresentationValid = false;
            ReceiptDiagnostic = ErrorMessage(Response, false);
            if (bFocusError) ShowResult(ReceiptDiagnostic, true);
            return;
        }
        ReceiptDiagnostic = FText::GetEmpty();
        Reviewed = Response->GetObjectField(TEXT("result"));
        ObservedAt = FPlatformTime::Seconds();
        // A retained native review is immutable. Status polls must not replace selectable text.
        if (DisplayedPlanId != PlanId || !bPresentationValid)
        {
            const auto Presentation = FJevEditorReviewPresentation::Build(Reviewed);
            bPresentationValid = Presentation.bValid && JevReview::String(Reviewed, TEXT("plan_id")) == PlanId;
            DisplayedPlanId = PlanId;
            PlanSummary = Presentation.Summary;
            if (!PlanBox->GetText().EqualTo(Presentation.Body)) PlanBox->SetText(Presentation.Body);
            if (!TechnicalBox->GetText().EqualTo(Presentation.TechnicalDetails)) TechnicalBox->SetText(Presentation.TechnicalDetails);
        }
        if (bFocusReview) Focus(PlanBox);
    }

    double RemainingSeconds() const
    {
        double Seconds = 0;
        if (!Reviewed || !Reviewed->TryGetNumberField(TEXT("expires_in_seconds"), Seconds) || !FMath::IsFinite(Seconds)) return 0;
        return FMath::Max(0.0, Seconds - FMath::Max(0.0, FPlatformTime::Seconds() - ObservedAt));
    }

    bool CanApply() const
    {
        return Access->Bridge && !bApplying && ReceiptDiagnostic.IsEmpty() && ListDiagnostic.IsEmpty() && bPresentationValid && Reviewed
            && JevReview::String(Reviewed, TEXT("plan_id")) == SelectedPlanId
            && JevReview::String(Reviewed, TEXT("status")) == TEXT("pending") && RemainingSeconds() > 0;
    }

    FText StatusText() const
    {
        if (!ReceiptDiagnostic.IsEmpty() || !ListDiagnostic.IsEmpty())
            return FText::Format(LOCTEXT("RefreshUnavailable", "Refresh unavailable. Applying is disabled; the last action result is retained.\n{0}"),
                !ReceiptDiagnostic.IsEmpty() ? ReceiptDiagnostic : ListDiagnostic);
        if (!Reviewed) return SelectedPlanId.IsEmpty() ? LOCTEXT("NoPlanStatus", "No plan selected. Applying is disabled.") : LOCTEXT("UnavailableStatus", "Plan receipt unavailable. Applying is disabled; refresh or inspect the editor.");
        const FString Status = JevReview::String(Reviewed, TEXT("status"));
        if (!bPresentationValid) return LOCTEXT("IncompleteReview", "Review data is incomplete or unsupported. Applying is disabled. Inspect the editor and create a supported preview.");
        if (Status == TEXT("pending"))
            return RemainingSeconds() > 0 ? FText::Format(LOCTEXT("PendingStatus", "Pending · {0} seconds remaining. The editor checks scene state again on Apply."), FText::AsNumber(FMath::CeilToInt(RemainingSeconds())))
                : LOCTEXT("ElapsedStatus", "Preview expired. Inspect current state and create a new preview; nothing is retried automatically.");
        if (Status == TEXT("applied")) return LOCTEXT("AppliedStatus", "Applied receipt · historical result. Inspect and verify the current scene, including any later edits or Undo.");
        if (Status == TEXT("expired")) return LOCTEXT("ExpiredStatus", "Preview expired. Inspect current state and create a new preview.");
        if (Status == TEXT("unknown") || Status == TEXT("applying")) return LOCTEXT("UnknownStatus", "Execution outcome is uncertain or in progress. Inspect the scene and receipt before taking another action. Do not repeat Apply.");
        if (Status == TEXT("rolled_back")) return LOCTEXT("RolledBackStatus", "Native transaction rolled back. Inspect current state; external callback effects are outside the rollback guarantee.");
        if (Status == TEXT("rejected")) return LOCTEXT("RejectedStatus", "Plan rejected. Inspect the current scene and the reported reason before creating a new preview.");
        return LOCTEXT("UnsupportedStatus", "Unsupported receipt status. Applying is disabled; inspect the editor.");
    }

    void RefreshPlans(bool bFocusError = false)
    {
        if (!Access->Bridge)
        {
            if (PendingSignature != TEXT("disabled")) PendingBox->ClearChildren();
            PendingSignature = TEXT("disabled");
            return;
        }
        const auto Response = JevReview::Call(*Access->Bridge, TEXT("pending_plans"));
        if (!Response->GetBoolField(TEXT("ok")))
        {
            Reviewed.Reset(); // A failed refresh never leaves an apparently actionable old receipt.
            bPresentationValid = false;
            ListDiagnostic = ErrorMessage(Response, false);
            if (bFocusError) ShowResult(ListDiagnostic, true);
            return;
        }
        ListDiagnostic = FText::GetEmpty();
        const auto Result = Response->GetObjectField(TEXT("result"));
        const auto Plans = Result->GetArrayField(TEXT("plans"));
        FString Signature = Result->GetBoolField(TEXT("truncated")) ? TEXT("truncated") : TEXT("complete");
        PendingSummaries.Empty();
        for (const auto& Value : Plans)
        {
            const auto Plan = Value->AsObject();
            const FString Id = Plan->GetStringField(TEXT("plan_id"));
            Signature += TEXT("|") + Id;
            PendingSummaries.Add(Id, Plan);
        }
        if (PendingSignature == Signature) return; // Preserve focused buttons while only countdowns change.
        PendingSignature = Signature;
        PendingBox->ClearChildren();
        if (Plans.IsEmpty()) PendingBox->AddSlot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("NoPending", "No pending plans. New MCP previews appear here automatically.")).AutoWrapText(true) ];
        for (const auto& Value : Plans)
        {
            const auto Plan = Value->AsObject();
            const FString Id = Plan->GetStringField(TEXT("plan_id"));
            PendingBox->AddSlot().AutoHeight().Padding(0, 2)
            [ SNew(SButton).Tag(FName(*(FString(TEXT("Jev.Review.Pending.")) + Id)))
                .Text_Lambda([this, Id]
                {
                    const auto* Current = PendingSummaries.Find(Id);
                    return Current ? FText::Format(LOCTEXT("PendingButton", "Review {0} · {1} changes · {2}s"), FText::FromString(Id.Left(8)),
                        FText::AsNumber(static_cast<int32>((*Current)->GetNumberField(TEXT("operation_count")))), FText::AsNumber(FMath::CeilToInt((*Current)->GetNumberField(TEXT("expires_in_seconds"))))) : LOCTEXT("MissingPlan", "Plan unavailable");
                })
                .ToolTipText(FText::FromString(Plan->GetStringField(TEXT("world_path")) + TEXT("\n") + Id))
                .OnClicked_Lambda([this, Id] { Review(Id, true, true); return FReply::Handled(); }) ];
        }
        if (Result->GetBoolField(TEXT("truncated"))) PendingBox->AddSlot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("PendingTruncated", "Showing the newest 20 pending plans. Additional plans remain accessible by MCP plan ID.")).AutoWrapText(true) ];
    }

    FReply Apply()
    {
        if (!CanApply()) return FReply::Handled();
        const FString Id = SelectedPlanId;
        auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("plan_id"), Id);
        bApplying = true;
        Reviewed.Reset(); // Disable before execution; never retry implicitly.
        AppliedActorPaths.Empty();
        const auto Response = JevReview::Call(*Access->Bridge, TEXT("apply"), Params);
        bApplying = false;
        if (Response->GetBoolField(TEXT("ok")))
        {
            for (const auto& Actor : Response->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors")))
                AppliedActorPaths.Add(MakeShared<FJsonValueString>(Actor->AsObject()->GetStringField(TEXT("path"))));
            ShowResult(LOCTEXT("ApplySuccess", "Applied once. The bridge requested no save. Unreal Undo is available. Inspect the applied actors now, then run MCP verification checks for your task requirements."), true);
        }
        else ShowError(Response, true, true);
        Review(Id);
        RefreshPlans();
        return FReply::Handled();
    }

    FReply InspectApplied()
    {
        if (!Access->Bridge || AppliedActorPaths.IsEmpty()) return FReply::Handled();
        auto Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("actor_paths"), AppliedActorPaths);
        const auto Response = JevReview::Call(*Access->Bridge, TEXT("actor_details"), Params);
        if (Response->GetBoolField(TEXT("ok")))
            ShowResult(FText::Format(LOCTEXT("FreshInspection", "Fresh inspection of the applied actor paths (not a task pass/fail verdict):\n{0}"), DescribeActors(Response->GetObjectField(TEXT("result")))), true);
        else ShowError(Response, false, true);
        return FReply::Handled();
    }

    EActiveTimerReturnType RefreshTimer(double, float)
    {
        if (!SelectedPlanId.IsEmpty() && Access->Bridge) Review(SelectedPlanId);
        RefreshPlans();
        return EActiveTimerReturnType::Continue;
    }

    TSharedPtr<FJevReviewAccess> Access;
    TSharedPtr<SVerticalBox> PendingBox;
    TSharedPtr<SScrollBox> RootScroll;
    TSharedPtr<SMultiLineEditableTextBox> SelectionBox, PlanBox, TechnicalBox, ResultBox;
    TMap<FString, TSharedPtr<FJsonObject>> PendingSummaries;
    FString PendingSignature, SelectedPlanId, DisplayedPlanId;
    FText PlanSummary, ReceiptDiagnostic, ListDiagnostic;
    TSharedPtr<FJsonObject> Reviewed;
    TArray<TSharedPtr<FJsonValue>> AppliedActorPaths;
    FVector Translation = FVector::ZeroVector;
    FString Label, Folder;
    double ObservedAt = 0;
    bool bLabel = false, bFolder = false, bPresentationValid = false, bApplying = false;
};

#if WITH_DEV_AUTOMATION_TESTS
TSharedRef<SWidget> FJevEditorReviewPanel::CreateForTesting(FJevEditorBridge& Bridge)
{
    auto TestAccess = MakeShared<FJevReviewAccess>();
    TestAccess->Bridge = &Bridge;
    return SNew(SJevReviewWidget).Access(TestAccess);
}
#endif

FJevEditorReviewPanel::FJevEditorReviewPanel() : Access(MakeShared<FJevReviewAccess>()) {}
FJevEditorReviewPanel::~FJevEditorReviewPanel() { Unregister(); }

void FJevEditorReviewPanel::SetBridge(FJevEditorBridge* Bridge, const FString& DisabledReason)
{
    Access->Bridge = Bridge;
    if (!DisabledReason.IsEmpty()) Access->DisabledReason = DisabledReason;
}

void FJevEditorReviewPanel::Register()
{
    if (bRegistered || IsRunningCommandlet() || !FApp::CanEverRender() || !FSlateApplication::IsInitialized()) return;
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(JevReview::TabName, FOnSpawnTab::CreateRaw(this, &FJevEditorReviewPanel::SpawnTab))
        .SetDisplayName(LOCTEXT("TabName", "Jev Review")).SetTooltipText(LOCTEXT("TabTooltip", "Inspect selection and review bounded editor plans."))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FJevEditorReviewPanel::RegisterMenus));
    bRegistered = true;
}

void FJevEditorReviewPanel::RegisterMenus()
{
    FToolMenuOwnerScoped Owner(this);
    if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window")))
        Menu->FindOrAddSection(TEXT("WindowLayout")).AddMenuEntry(TEXT("JevReview"), LOCTEXT("TabName", "Jev Review"),
            LOCTEXT("MenuTooltip", "Inspect actors, review MCP plans, and apply exact bounded changes."), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(JevReview::TabName); })));
}

TSharedRef<SDockTab> FJevEditorReviewPanel::SpawnTab(const FSpawnTabArgs& Args)
{
    const auto NewTab = SNew(SDockTab).TabRole(ETabRole::NomadTab)[ SNew(SJevReviewWidget).Access(Access) ];
    Tab = NewTab;
    return NewTab;
}

void FJevEditorReviewPanel::Unregister()
{
    Access->Bridge = nullptr;
    Access->DisabledReason = LOCTEXT("Shutdown", "Jev editor module is shutting down. Reopen the editor to reconnect.").ToString();
    if (!bRegistered) return;
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    if (const auto LiveTab = Tab.Pin()) LiveTab->RequestCloseTab();
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JevReview::TabName);
    bRegistered = false;
}

#undef LOCTEXT_NAMESPACE
