#include "JevEditorReviewPanel.h"

#include "JevEditorBridge.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/App.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

struct FJevReviewAccess
{
    FJevEditorBridge* Bridge = nullptr;
    FString DisabledReason = TEXT("Bridge disabled. Set JEV_BRIDGE_TOKEN before launching Unreal; see Jev_Unreal setup instructions.");
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
    if (!Object || !Object->TryGetArrayField(Field, Values) || Values->Num() != 3) return TEXT("unavailable");
    return FString::Printf(TEXT("(%s, %s, %s)"), *FString::SanitizeFloat((*Values)[0]->AsNumber(), 2), *FString::SanitizeFloat((*Values)[1]->AsNumber(), 2), *FString::SanitizeFloat((*Values)[2]->AsNumber(), 2));
}

FString ErrorText(const TSharedPtr<FJsonObject>& Response)
{
    const TSharedPtr<FJsonObject>* Error = nullptr;
    if (!Response || !Response->TryGetObjectField(TEXT("error"), Error)) return TEXT("Unexpected editor response. Refresh inspection.");
    return String(*Error, TEXT("code")) + TEXT(": ") + String(*Error, TEXT("message"));
}

FString MeshDetails(const TSharedPtr<FJsonObject>& Operation, const TSharedPtr<FJsonObject>& Baseline)
{
    // Keep the complete bounded native review visible, including collision channel
    // responses and null material overrides which a compact summary would lose.
    auto Details = MakeShared<FJsonObject>();
    auto Before = MakeShared<FJsonObject>();
    if (Baseline)
        for (const TCHAR* Field : {TEXT("material_slot_count"), TEXT("material_override_count"), TEXT("materials"), TEXT("mesh_settings")})
            if (const auto Value = Baseline->TryGetField(Field)) Before->SetField(Field, Value);
    Details->SetObjectField(TEXT("before"), Before);
    auto After = MakeShared<FJsonObject>();
    for (const TCHAR* Field : {TEXT("material_slot_count"), TEXT("material_override_count"), TEXT("materials"), TEXT("mesh_settings"), TEXT("mesh_review")})
        if (const auto Value = Operation->TryGetField(Field)) After->SetField(Field, Value);
    Details->SetObjectField(TEXT("after"), After);
    FString Encoded;
    FJsonSerializer::Serialize(Details, TJsonWriterFactory<>::Create(&Encoded));
    return TEXT("Reviewed material, collision and mesh details:\n") + Encoded + TEXT("\n");
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
    if (!Record) return TEXT("Choose a pending plan, or preview an edit to the current selection.");
    const auto Review = Record->GetObjectField(TEXT("review"));
    FString Text = FString::Printf(TEXT("Plan %s\nStatus: %s  |  Expires in %.0f seconds\nProject: %s\nWorld: %s\n\n"),
        *JevReview::String(Record, TEXT("plan_id")), *JevReview::String(Record, TEXT("status")), Record->GetNumberField(TEXT("expires_in_seconds")),
        *JevReview::String(Review, TEXT("project_file")), *JevReview::String(Review, TEXT("world_path")));
    const auto Operations = Review->GetArrayField(TEXT("operations"));
    const auto Before = Review->GetArrayField(TEXT("before"));
    for (int32 Index = 0; Index < Operations.Num(); ++Index)
    {
        const auto Operation = Operations[Index]->AsObject();
        const FString Op = JevReview::String(Operation, TEXT("op"));
        const auto Baseline = Before.IsValidIndex(Index) && Before[Index]->Type == EJson::Object ? Before[Index]->AsObject() : TSharedPtr<FJsonObject>();
        Text += FString::Printf(TEXT("%d. %s\n"), Index + 1, Baseline ? *JevReview::String(Baseline, TEXT("label")) : *JevReview::String(Operation, TEXT("label")));
        if (Baseline) Text += JevReview::String(Baseline, TEXT("path")) + TEXT("\n");
        if (Op == TEXT("set_metadata"))
        {
            if (Operation->HasField(TEXT("label"))) Text += TEXT("Label: ") + JevReview::String(Baseline, TEXT("label")) + TEXT(" → ") + JevReview::String(Operation, TEXT("label")) + TEXT("\n");
            if (Operation->HasField(TEXT("folder"))) Text += TEXT("Folder: ") + (JevReview::String(Baseline, TEXT("folder")).IsEmpty() ? TEXT("(root)") : JevReview::String(Baseline, TEXT("folder"))) + TEXT(" → ") + (JevReview::String(Operation, TEXT("folder")).IsEmpty() ? TEXT("(root)") : JevReview::String(Operation, TEXT("folder"))) + TEXT("\n");
            Text += TEXT("Transform unchanged.\n");
        }
        else if (Op == TEXT("set_material"))
        {
            Text += FString::Printf(TEXT("Material slot %d: %s → %s\nTransform unchanged.\n"), static_cast<int32>(Operation->GetNumberField(TEXT("slot"))),
                *JevReview::String(Baseline, TEXT("material_path")), *JevReview::String(Operation, TEXT("material_path")));
        }
        else if (Op == TEXT("replace_mesh") || Op == TEXT("duplicate_mesh"))
        {
            if (Op == TEXT("replace_mesh"))
            {
                Text += TEXT("Replace mesh: ") + JevReview::String(Baseline, TEXT("static_mesh_path")) + TEXT(" → ") + JevReview::String(Operation, TEXT("asset_path")) + TEXT("\n");
                Text += TEXT("Material policy: ") + JevReview::String(Operation, TEXT("material_policy")) + TEXT("\nActor identity, transform, label and folder remain. Geometry, local bounds and collision shapes follow the replacement mesh.\n");
                Text += JevReview::String(Operation, TEXT("material_policy")) == TEXT("preserve_slots")
                    ? TEXT("Keep effective source materials by slot index; slot names are not matched.\n")
                    : TEXT("Clear all component overrides and use the replacement mesh's default materials.\n");
            }
            else
            {
                Text += TEXT("Create controlled mesh copy: ") + JevReview::String(Operation, TEXT("label")) + TEXT("\nSource: ") + JevReview::String(Operation, TEXT("actor_path")) + TEXT("\nMesh: ") + JevReview::String(Operation, TEXT("asset_path")) + TEXT("\n");
                Text += TEXT("Creates a new actor with the reviewed materials and settings. Source stays unchanged. Extra components, physics simulation and unsupported customized properties are refused.\n");
                for (const TCHAR* Field : {TEXT("location"), TEXT("rotation"), TEXT("scale")})
                    Text += FString(Field) + TEXT(": ") + JevReview::DisplayVector(Baseline, Field) + TEXT(" → ") + JevReview::DisplayVector(Operation, Field) + TEXT("\n");
            }
            Text += TEXT("Folder: ") + (JevReview::String(Operation, TEXT("folder")).IsEmpty() ? TEXT("(root)") : JevReview::String(Operation, TEXT("folder"))) + TEXT("\n");
            Text += JevReview::MeshDetails(Operation, Baseline);
        }
        else
        {
            if (Op.StartsWith(TEXT("spawn_"))) Text += TEXT("Create ") + (Op == TEXT("spawn_primitive") ? JevReview::String(Operation, TEXT("shape")) : JevReview::String(Operation, TEXT("asset_path"))) + TEXT("\n");
            for (const TCHAR* Field : {TEXT("location"), TEXT("rotation"), TEXT("scale")})
                Text += FString(Field) + TEXT(": ") + (Baseline ? JevReview::DisplayVector(Baseline, Field) + TEXT(" → ") : FString()) + JevReview::DisplayVector(Operation, Field) + TEXT("\n");
        }
        Text += TEXT("\n");
    }
    Text += TEXT("Apply consumes this exact plan once. Any scene change or expiry rejects it. Changes participate in Unreal Undo; this panel never saves the level. Fresh verification is still required after applying.");
    return Text;
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
                SAssignNew(RootScroll, SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                + SScrollBox::Slot()
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("Jev — inspect, preview, apply"))).Font(FAppStyle::GetFontStyle("HeadingExtraSmall")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SNew(STextBlock).Tag(TEXT("Jev.Review.Status")).Text_Lambda([this] { return FText::FromString(Access->Bridge ? TEXT("Connected to this Unreal editor. All edits use bounded preview plans. No API key or model call is needed for these controls.") : Access->DisabledReason); }).AutoWrapText(true) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.Inspect")).Text(FText::FromString(TEXT("Inspect selected actors"))).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; }).OnClicked(this, &SJevReviewWidget::Inspect) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [ SNew(SBox).MaxDesiredHeight(160) [ SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                        + SScrollBox::Slot() [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(SelectionText); }).AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping) ] ] ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("Translate selection (centimeters)"))) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0) [ Coordinate(0, TEXT("X")) ]
                        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0) [ Coordinate(1, TEXT("Y")) ]
                        + SHorizontalBox::Slot().FillWidth(1) [ Coordinate(2, TEXT("Z")) ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Preview translation"))).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; }).OnClicked_Lambda([this] { return Preview(true); }) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(SCheckBox).OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bLabel = State == ECheckBoxState::Checked; }) [ SNew(STextBlock).Text(FText::FromString(TEXT("Change label — select exactly one actor"))) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 8)
                    [ SNew(SEditableTextBox).Tag(TEXT("Jev.Review.Label")).AccessibleText(FText::FromString(TEXT("New actor label"))).ToolTipText(FText::FromString(TEXT("New actor label, 1 to 80 characters. Enable Change label to include it in the preview."))).HintText(FText::FromString(TEXT("Actor label (1–80 characters)"))).OnTextChanged_Lambda([this](const FText& Text) { Label = Text.ToString(); }) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(SCheckBox).OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bFolder = State == ECheckBoxState::Checked; }) [ SNew(STextBlock).Text(FText::FromString(TEXT("Change folder — leave empty to move to root"))) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
                    [ SNew(SEditableTextBox).Tag(TEXT("Jev.Review.Folder")).AccessibleText(FText::FromString(TEXT("New actor folder"))).ToolTipText(FText::FromString(TEXT("Relative folder path. An empty value moves actors to root when Change folder is enabled."))).HintText(FText::FromString(TEXT("Example: Environment/Props"))).OnTextChanged_Lambda([this](const FText& Text) { Folder = Text.ToString(); }) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Preview label / folder"))).IsEnabled_Lambda([this] { return Access->Bridge && (bLabel || bFolder); }).OnClicked_Lambda([this] { return Preview(false); }) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("Pending plans from people and MCP clients"))).Font(FAppStyle::GetFontStyle("HeadingExtraSmall")) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 6)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Refresh pending plans"))).IsEnabled_Lambda([this] { return Access->Bridge != nullptr; }).OnClicked_Lambda([this] { RefreshPlans(); return FReply::Handled(); }) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [ SNew(SBox).MaxDesiredHeight(180) [ SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                        + SScrollBox::Slot() [ SAssignNew(PendingBox, SVerticalBox) ] ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SAssignNew(ReviewBox, SBox).MaxDesiredHeight(320) [ SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                        + SScrollBox::Slot() [ SNew(STextBlock).Tag(TEXT("Jev.Review.Plan")).Text_Lambda([this] { return ReviewText; }).AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping) ] ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SNew(SButton).Tag(TEXT("Jev.Review.Apply")).Text(FText::FromString(TEXT("Apply reviewed plan once"))).IsEnabled_Lambda([this] { return Access->Bridge && Reviewed && JevReview::String(Reviewed, TEXT("status")) == TEXT("pending") && Reviewed->GetNumberField(TEXT("expires_in_seconds")) > 0; }).OnClicked(this, &SJevReviewWidget::Apply) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Inspect applied actors now"))).IsEnabled_Lambda([this] { return Access->Bridge && !AppliedActorPaths.IsEmpty(); }).OnClicked(this, &SJevReviewWidget::InspectApplied) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(LastResult); }).AutoWrapText(true) ]
                ]
            ]
        ];
        RefreshPlans();
        RegisterActiveTimer(2.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SJevReviewWidget::RefreshTimer));
    }

private:
    TSharedRef<SWidget> Coordinate(int32 Axis, const TCHAR* Name)
    {
        const FText AccessibleName = FText::FromString(FString::Printf(TEXT("Translate %s, centimeters"), Name));
        return SNew(SNumericEntryBox<double>).Tag(FName(*FString::Printf(TEXT("Jev.Review.Translate.%s"), Name)))
            .AccessibleText(AccessibleName).ToolTipText(AccessibleName).AllowSpin(true).MinValue(-1000000).MaxValue(1000000)
            .Value_Lambda([this, Axis] { return TOptional<double>(Translation[Axis]); })
            .OnValueChanged_Lambda([this, Axis](double Value) { Translation[Axis] = Value; })
            .Label()[ SNew(STextBlock).Text(FText::FromString(Name)) ];
    }

    FString DescribeActors(const TSharedPtr<FJsonObject>& Details) const
    {
        FString Text = TEXT("Project: ") + JevReview::String(Details, TEXT("project_file")) + TEXT("\n");
        for (const auto& Value : Details->GetArrayField(TEXT("actors")))
        {
            const auto Actor = Value->AsObject();
            Text += JevReview::String(Actor, TEXT("label")) + TEXT(" — ") + JevReview::DisplayVector(Actor, TEXT("location")) + TEXT(" cm\n") + JevReview::String(Actor, TEXT("path")) + TEXT("\n");
            Text += TEXT("Rotation (pitch, yaw, roll): ") + JevReview::DisplayVector(Actor, TEXT("rotation")) + TEXT(" degrees\nScale: ") + JevReview::DisplayVector(Actor, TEXT("scale")) + TEXT("\nFolder: ") + (JevReview::String(Actor, TEXT("folder")).IsEmpty() ? TEXT("(root)") : JevReview::String(Actor, TEXT("folder"))) + TEXT("\n");
            const TSharedPtr<FJsonObject>* Bounds = nullptr;
            if (Actor->TryGetObjectField(TEXT("bounds_cm"), Bounds))
                Text += TEXT("World bounds size: ") + JevReview::DisplayVector(*Bounds, TEXT("size")) + TEXT(" cm\n");
            if (!Actor->GetBoolField(TEXT("editable")))
            {
                TArray<FString> Blockers;
                for (const auto& Blocker : Actor->GetArrayField(TEXT("edit_blockers"))) Blockers.Add(Blocker->AsString());
                Text += TEXT("Editing blocked: ") + FString::Join(Blockers, TEXT(", ")) + TEXT("\n");
            }
            else Text += TEXT("Editable native static mesh actor.\n");
        }
        return Text;
    }

    FReply Inspect()
    {
        if (!Access->Bridge) return FReply::Handled();
        const auto Response = FJevEditorReviewPanel::InspectSelection(*Access->Bridge);
        SelectionText = Response->GetBoolField(TEXT("ok")) ? DescribeActors(Response->GetObjectField(TEXT("result"))) : JevReview::ErrorText(Response);
        return FReply::Handled();
    }

    FReply Preview(bool bTranslation)
    {
        if (!Access->Bridge) return FReply::Handled();
        const auto Response = FJevEditorReviewPanel::PreviewSelection(*Access->Bridge, bTranslation ? &Translation : nullptr,
            !bTranslation && bLabel ? &Label : nullptr, !bTranslation && bFolder ? &Folder : nullptr);
        if (Response->GetBoolField(TEXT("ok")))
        {
            Review(Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")), true);
            LastResult = TEXT("Preview created without editing the scene. Read the exact changes above, then apply explicitly.");
        }
        else
        {
            Reviewed.Reset();
            ReviewText = FText::FromString(FJevEditorReviewPanel::DescribeReview(nullptr));
            LastResult = JevReview::ErrorText(Response);
        }
        RefreshPlans();
        return FReply::Handled();
    }

    void Review(const FString& PlanId, bool bBringIntoView = false)
    {
        if (!Access->Bridge) { Reviewed.Reset(); return; }
        auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("plan_id"), PlanId);
        const auto Response = JevReview::Call(*Access->Bridge, TEXT("plan_status"), Params);
        if (Response->GetBoolField(TEXT("ok"))) Reviewed = Response->GetObjectField(TEXT("result"));
        else { Reviewed.Reset(); LastResult = JevReview::ErrorText(Response); }
        ReviewText = FText::FromString(FJevEditorReviewPanel::DescribeReview(Reviewed));
        if (bBringIntoView) RootScroll->ScrollDescendantIntoView(ReviewBox, false);
    }

    void RefreshPlans()
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
            const FString Signature = TEXT("error:") + JevReview::ErrorText(Response);
            if (PendingSignature == Signature) return;
            PendingSignature = Signature;
            PendingBox->ClearChildren();
            PendingBox->AddSlot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(JevReview::ErrorText(Response))).AutoWrapText(true) ];
            return;
        }
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
        // Preserve the actual focused Slate buttons while only countdowns change.
        if (PendingSignature == Signature) return;
        PendingSignature = Signature;
        PendingBox->ClearChildren();
        if (Plans.IsEmpty()) PendingBox->AddSlot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(TEXT("No pending plans. New MCP previews appear here automatically."))).AutoWrapText(true) ];
        for (const auto& Value : Plans)
        {
            const auto Plan = Value->AsObject();
            const FString Id = Plan->GetStringField(TEXT("plan_id"));
            PendingBox->AddSlot().AutoHeight().Padding(0, 2)
            [ SNew(SButton).Tag(FName(*(FString(TEXT("Jev.Review.Pending.")) + Id)))
                .Text_Lambda([this, Id]
                {
                    const TSharedPtr<FJsonObject>* Current = PendingSummaries.Find(Id);
                    return FText::FromString(Current ? FString::Printf(TEXT("Review %s · %d changes · %.0fs"), *Id.Left(8), static_cast<int32>((*Current)->GetNumberField(TEXT("operation_count"))), (*Current)->GetNumberField(TEXT("expires_in_seconds"))) : TEXT("Plan unavailable"));
                })
                .ToolTipText(FText::FromString(Plan->GetStringField(TEXT("world_path")) + TEXT("\n") + Id)).OnClicked_Lambda([this, Id] { Review(Id, true); return FReply::Handled(); }) ];
        }
        if (Result->GetBoolField(TEXT("truncated"))) PendingBox->AddSlot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(TEXT("Showing the newest 20 pending plans. Additional plans remain accessible by MCP plan ID."))).AutoWrapText(true) ];
    }

    FReply Apply()
    {
        if (!Access->Bridge || !Reviewed) return FReply::Handled();
        const FString Id = Reviewed->GetStringField(TEXT("plan_id"));
        auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("plan_id"), Id);
        Reviewed.Reset(); // Disable the button before execution; never retry implicitly.
        AppliedActorPaths.Empty();
        const auto Response = JevReview::Call(*Access->Bridge, TEXT("apply"), Params);
        if (Response->GetBoolField(TEXT("ok")))
        {
            for (const auto& Actor : Response->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors")))
                AppliedActorPaths.Add(MakeShared<FJsonValueString>(Actor->AsObject()->GetStringField(TEXT("path"))));
            LastResult = TEXT("Applied once. The level is unsaved. Unreal Undo is available. Inspect the applied actors now and use MCP verification checks for explicit pass/fail evidence.");
        }
        else LastResult = JevReview::ErrorText(Response) + TEXT(" No retry was made. Inspect the scene before creating a new preview.");
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
        LastResult = Response->GetBoolField(TEXT("ok")) ? TEXT("Fresh inspection of the applied actor paths:\n") + DescribeActors(Response->GetObjectField(TEXT("result"))) : JevReview::ErrorText(Response);
        return FReply::Handled();
    }

    EActiveTimerReturnType RefreshTimer(double, float)
    {
        if (Reviewed && Access->Bridge) Review(Reviewed->GetStringField(TEXT("plan_id")));
        RefreshPlans();
        return EActiveTimerReturnType::Continue;
    }

    TSharedPtr<FJevReviewAccess> Access;
    TSharedPtr<SVerticalBox> PendingBox;
    TSharedPtr<SScrollBox> RootScroll;
    TSharedPtr<SBox> ReviewBox;
    TMap<FString, TSharedPtr<FJsonObject>> PendingSummaries;
    FString PendingSignature;
    FText ReviewText = FText::FromString(TEXT("Choose a pending plan, or preview an edit to the current selection."));
    TSharedPtr<FJsonObject> Reviewed;
    TArray<TSharedPtr<FJsonValue>> AppliedActorPaths;
    FVector Translation = FVector::ZeroVector;
    FString Label, Folder;
    bool bLabel = false, bFolder = false;
    FString SelectionText = TEXT("Select native static mesh actors in the World Outliner, then inspect. Other actor types remain visible with edit blockers.");
    FString LastResult;
};

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
        .SetDisplayName(FText::FromString(TEXT("Jev Review"))).SetTooltipText(FText::FromString(TEXT("Inspect selection and review bounded editor plans.")))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FJevEditorReviewPanel::RegisterMenus));
    bRegistered = true;
}

void FJevEditorReviewPanel::RegisterMenus()
{
    FToolMenuOwnerScoped Owner(this);
    if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window")))
        Menu->FindOrAddSection(TEXT("WindowLayout")).AddMenuEntry(TEXT("JevReview"), FText::FromString(TEXT("Jev Review")),
            FText::FromString(TEXT("Inspect actors, review MCP plans, and apply exact bounded changes.")), FSlateIcon(),
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
    Access->DisabledReason = TEXT("Jev editor module is shutting down. Reopen the editor to reconnect.");
    if (!bRegistered) return;
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    if (const auto LiveTab = Tab.Pin()) LiveTab->RequestCloseTab();
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JevReview::TabName);
    bRegistered = false;
}
