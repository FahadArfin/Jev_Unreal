#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBlueprintTools.h"
#include "JevEditorProjectTools.h"
#include "JevBlueprintTestTypes.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Blueprint/UserWidget.h"
#include "Factories/AnimBlueprintFactory.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Editor.h"
#include "Misc/ScopeExit.h"

namespace JevBlueprintTests
{
TSharedRef<FJsonObject> Identity()
{
    auto Value = MakeShared<FJsonObject>();
    Value->SetStringField(TEXT("project_file"), TEXT("/fixture/JevSandbox.uproject"));
    Value->SetStringField(TEXT("session_id"), TEXT("compile-fixture"));
    Value->SetStringField(TEXT("world_path"), TEXT("/Game/FixtureWorld"));
    Value->SetStringField(TEXT("revision"), TEXT("fixture-revision"));
    Value->SetBoolField(TEXT("play_in_editor"), false);
    Value->SetBoolField(TEXT("simulating"), false);
    return Value;
}

FString ErrorCode(const TSharedRef<FJsonObject>& Value)
{
    return Value->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : Value->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
}

TSharedRef<FJsonObject> PreviewParams()
{
    auto Value = MakeShared<FJsonObject>();
    const auto Current = Identity();
    auto State = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) State->SetStringField(Key, Current->GetStringField(Key));
    Value->SetStringField(TEXT("target_id"), TEXT("fixture"));
    Value->SetStringField(TEXT("expected_project"), Current->GetStringField(TEXT("project_file")));
    Value->SetObjectField(TEXT("expected_state"), State);
    return Value;
}

TSharedRef<FJsonObject> CommitParams(const TSharedRef<FJsonObject>& Preview)
{
    auto Value = MakeShared<FJsonObject>();
    Value->SetStringField(TEXT("plan_id"), Preview->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")));
    Value->SetStringField(TEXT("expected_project"), Identity()->GetStringField(TEXT("project_file")));
    return Value;
}

struct FFixture
{
    UPackage* Package = nullptr;
    TArray<UObject*> Assets;
    bool bHadEnabled = false, bEnabled = false;
    TArray<FString> Targets;
    FFixture()
    {
        Package = CreatePackage(*(TEXT("/Game/JevCompileFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        Package->AddToRoot();
        bHadEnabled = GConfig->GetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnabled"), bEnabled, GGameIni);
        GConfig->GetArray(TEXT("JevEditor.BlueprintCompilation"), TEXT("Targets"), Targets, GGameIni);
        GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnabled"), true, GGameIni);
    }
    void Approve(UBlueprint* BP)
    {
        GConfig->SetArray(TEXT("JevEditor.BlueprintCompilation"), TEXT("Targets"), {TEXT("fixture|") + BP->GetPathName()}, GGameIni);
    }
    UBlueprint* CompileAsset()
    {
        auto* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, FName(TEXT("BP_CompileFixture")), BPTYPE_Normal, FName(TEXT("JevAutomation")));
        Assets.Add(BP); FAssetRegistryModule::AssetCreated(BP); Approve(BP); return BP;
    }
    UBlueprint* InspectionAsset(UClass* Class, const TCHAR* Name)
    {
        auto* BP = NewObject<UBlueprint>(Package, Class, Name, RF_Public | RF_Standalone);
        auto* Graph = NewObject<UEdGraph>(BP, TEXT("StoredGraph"));
        Graph->GraphGuid = FGuid::NewGuid(); BP->UbergraphPages.Add(Graph);
        BP->Status = BS_Dirty;
        Assets.Add(BP); FAssetRegistryModule::AssetCreated(BP); return BP;
    }
    ~FFixture()
    {
        if (bHadEnabled) GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnabled"), bEnabled, GGameIni);
        else GConfig->RemoveKey(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnabled"), GGameIni);
        GConfig->SetArray(TEXT("JevEditor.BlueprintCompilation"), TEXT("Targets"), Targets, GGameIni);
        for (UObject* Asset : Assets) { FAssetRegistryModule::AssetDeleted(Asset); Asset->ClearFlags(RF_Public | RF_Standalone); }
        Package->SetDirtyFlag(false); Package->RemoveFromRoot();
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintPinWorkflow, "Jev.Editor.BlueprintPinWorkflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintPinWorkflow::RunTest(const FString&)
{
    using namespace JevBlueprintTests; FFixture Fixture; UBlueprint* BP = Fixture.CompileAsset();
    bool OldPinEdits = false; const bool HadPinEdits = GConfig->GetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnablePinEdits"), OldPinEdits, GGameIni);
    ON_SCOPE_EXIT { if (HadPinEdits) GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnablePinEdits"), OldPinEdits, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnablePinEdits"), GGameIni); };
    GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnablePinEdits"), true, GGameIni);
    UEdGraph* Graph = BP->UbergraphPages[0]; auto* Node = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional); Node->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Add_IntInt"))); Node->CreateNewGuid(); Node->AllocateDefaultPins(); Graph->AddNode(Node, false, false);
    auto* Pin = Node->FindPin(TEXT("A")); if (!TestNotNull(TEXT("native math input"), Pin)) return false;
    Pin->DefaultValue = TEXT("2"); BP->Status = BS_Dirty; FJevBlueprintTools Tools;
    auto Params = PreviewParams(); auto Edit = MakeShared<FJsonObject>(); Edit->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString()); Edit->SetStringField(TEXT("pin_id"), Pin->PinId.ToString()); Edit->SetStringField(TEXT("value"), TEXT("7")); Params->SetObjectField(TEXT("pin_edit"), Edit);
    auto Plan = Tools.Execute(TEXT("blueprint_pin_preview"), Params, Identity()); if (!TestTrue(TEXT("reviewable primitive edit"), Plan->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("preview preserves literal"), Pin->DefaultValue, FString(TEXT("2")));
    auto Result = Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity()); if (!TestTrue(TEXT("edit compiles"), Result->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("fresh compiler success"), Result->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("passed"))); TestEqual(TEXT("literal applied"), Pin->DefaultValue, FString(TEXT("7")));
    GEditor->UndoTransaction(); TestEqual(TEXT("Undo restores literal"), Pin->DefaultValue, FString(TEXT("2")));
    Edit->SetStringField(TEXT("value"), TEXT("1+2")); TestEqual(TEXT("expressions refused"), ErrorCode(Tools.Execute(TEXT("blueprint_pin_preview"), Params, Identity())), FString(TEXT("unsupported_asset")));
    Edit->SetStringField(TEXT("value"), TEXT("8")); auto* Output = Node->FindPin(TEXT("ReturnValue")); Pin->MakeLinkTo(Output); TestEqual(TEXT("linked input refused"), ErrorCode(Tools.Execute(TEXT("blueprint_pin_preview"), Params, Identity())), FString(TEXT("unsupported_asset"))); Pin->BreakAllPinLinks();
    Plan = Tools.Execute(TEXT("blueprint_pin_preview"), Params, Identity()); if (!TestTrue(TEXT("stale literal plan"), Plan->GetBoolField(TEXT("ok")))) return false;
    Pin->DefaultValue = TEXT("3"); TestEqual(TEXT("unnotified literal change refused"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity())), FString(TEXT("stale_plan")));
    TestEqual(TEXT("cannot smuggle edit into compile-only action"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), Params, Identity())), FString(TEXT("bad_request")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintVariants, "Jev.Editor.BlueprintVariants", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintVariants::RunTest(const FString& Parameters)
{
    using namespace JevBlueprintTests;
    FFixture Fixture; FJevProjectTools Tools;
    auto Inspect = [&](UBlueprint* BP)
    {
        auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
        return Tools.Execute(TEXT("blueprint_inspect"), Params, Identity());
    };
    UBlueprint* Widget = Fixture.InspectionAsset(UWidgetBlueprint::StaticClass(), TEXT("WidgetFixture"));
    UBlueprint* Anim = Fixture.InspectionAsset(UAnimBlueprint::StaticClass(), TEXT("AnimFixture"));
    UBlueprint* Custom = Fixture.InspectionAsset(UJevBlueprintSubclassFixture::StaticClass(), TEXT("CustomFixture"));
    Fixture.Package->SetDirtyFlag(false);
    const auto WidgetResponse = Inspect(Widget), AnimResponse = Inspect(Anim);
    if (!TestTrue(TEXT("exact WidgetBlueprint accepted"), WidgetResponse->GetBoolField(TEXT("ok"))) || !TestTrue(TEXT("exact AnimBlueprint accepted"), AnimResponse->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("widget kind"), WidgetResponse->GetObjectField(TEXT("result"))->GetStringField(TEXT("blueprint_kind")), FString(TEXT("widget")));
    TestEqual(TEXT("animation kind"), AnimResponse->GetObjectField(TEXT("result"))->GetStringField(TEXT("blueprint_kind")), FString(TEXT("animation")));
    TestEqual(TEXT("stored animation graphs returned"), AnimResponse->GetObjectField(TEXT("result"))->GetNumberField(TEXT("graph_count")), 1.0);
    TestFalse(TEXT("inspection never compiles widget"), WidgetResponse->GetObjectField(TEXT("result"))->GetBoolField(TEXT("compiled")));
    TestFalse(TEXT("inspection preserves package cleanliness"), Fixture.Package->IsDirty());
    TestEqual(TEXT("custom Blueprint subclasses remain unsupported"), ErrorCode(Inspect(Custom)), FString(TEXT("unsupported_asset")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintCompileWorkflow, "Jev.Editor.BlueprintCompileWorkflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintCompileWorkflow::RunTest(const FString& Parameters)
{
    using namespace JevBlueprintTests;
    FFixture Fixture; UBlueprint* BP = Fixture.CompileAsset(); FJevBlueprintTools Tools;
    const auto Preview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity());
    if (!TestTrue(TEXT("approved loaded Blueprint previews"), Preview->GetBoolField(TEXT("ok")))) return false;
    TestFalse(TEXT("preview is not compilation"), Preview->GetObjectField(TEXT("result"))->GetBoolField(TEXT("compiled")));
    const auto Commit = CommitParams(Preview);
    const auto Compiled = Tools.Execute(TEXT("blueprint_compile"), Commit, Identity());
    if (!TestTrue(TEXT("explicit compile responds"), Compiled->GetBoolField(TEXT("ok")))) return false;
    const auto Result = Compiled->GetObjectField(TEXT("result"));
    TestEqual(TEXT("valid actor Blueprint passes fresh compile"), Result->GetStringField(TEXT("status")), FString(TEXT("passed")));
    TestEqual(TEXT("fresh compiler has no errors"), Result->GetNumberField(TEXT("error_count")), 0.0);
    TestFalse(TEXT("adapter never requests save"), Result->GetBoolField(TEXT("save_requested")));
    TestTrue(TEXT("callbacks saving is explicitly unknown"), Result->HasTypedField<EJson::Null>(TEXT("saved")));
    TestFalse(TEXT("no rollback claim"), Result->GetBoolField(TEXT("rollback_available")));
    TestEqual(TEXT("successful plan cannot replay"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), Commit, Identity())), FString(TEXT("plan_consumed")));
    auto Receipt = MakeShared<FJsonObject>(); Receipt->SetStringField(TEXT("plan_id"), Commit->GetStringField(TEXT("plan_id")));
    TestEqual(TEXT("receipt survives client reconnect boundary"), Tools.Execute(TEXT("blueprint_compile_receipt"), Receipt, Identity())->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("passed")));
    if (!TestTrue(TEXT("created Blueprint has event graph"), !BP->UbergraphPages.IsEmpty())) return false;
    auto* Node = NewObject<UJevBlueprintErrorFixtureNode>(BP->UbergraphPages[0]);
    Node->CreateNewGuid(); BP->UbergraphPages[0]->AddNode(Node, false, false);
    BP->Status = BS_Dirty;
    const auto BadPreview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity());
    if (!TestTrue(TEXT("failing asset can be explicitly reviewed"), BadPreview->GetBoolField(TEXT("ok")))) return false;
    const auto Failed = Tools.Execute(TEXT("blueprint_compile"), CommitParams(BadPreview), Identity());
    if (!TestTrue(TEXT("compiler failure still provides a receipt"), Failed->GetBoolField(TEXT("ok")))) return false;
    const auto Failure = Failed->GetObjectField(TEXT("result"));
    TestEqual(TEXT("fresh compile failure never reported passed"), Failure->GetStringField(TEXT("status")), FString(TEXT("failed")));
    TestTrue(TEXT("fresh compiler error count present"), Failure->GetNumberField(TEXT("error_count")) > 0);
    bool bMessage = false;
    for (const auto& Message : Failure->GetArrayField(TEXT("diagnostics"))) bMessage |= Message->AsObject()->GetStringField(TEXT("message")).Contains(TEXT("Jev deliberate compile diagnostic"));
    TestTrue(TEXT("fresh diagnostic text retained"), bMessage);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintCompileGuards, "Jev.Editor.BlueprintCompileGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintCompileGuards::RunTest(const FString& Parameters)
{
    using namespace JevBlueprintTests;
    FFixture Fixture; UBlueprint* BP = Fixture.CompileAsset(); double Now = 10;
    FJevBlueprintTools Tools([&Now] { return Now; });
    for (int32 Flag = 0; Flag < 3; ++Flag)
    {
        auto SetBusy = [BP, Flag](bool bBusy)
        {
            if (Flag == 0) GCompilingBlueprint = bBusy;
            else if (Flag == 1) BP->bBeingCompiled = bBusy;
            else BP->bQueuedForCompilation = bBusy;
        };
        SetBusy(true);
        const auto BusyPreview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity());
        SetBusy(false);
        TestEqual(TEXT("global, target and queued compiler activity blocks preview"), ErrorCode(BusyPreview), FString(TEXT("editor_busy")));
        const auto ReadyPreview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity());
        if (!TestTrue(TEXT("compiler guard fixture previews before activity starts"), ReadyPreview->GetBoolField(TEXT("ok")))) return false;
        const auto GuardCommit = CommitParams(ReadyPreview);
        SetBusy(true);
        const auto BusyCommit = Tools.Execute(TEXT("blueprint_compile"), GuardCommit, Identity());
        SetBusy(false);
        TestEqual(TEXT("global, target and queued compiler activity blocks commit"), ErrorCode(BusyCommit), FString(TEXT("editor_busy")));
        TestEqual(TEXT("compiler-busy attempt still consumes plan"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), GuardCommit, Identity())), FString(TEXT("plan_consumed")));
    }
    auto Params = PreviewParams(); Params->SetStringField(TEXT("target_id"), TEXT("not-approved"));
    TestEqual(TEXT("unapproved aliases rejected"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), Params, Identity())), FString(TEXT("target_not_allowed")));
    Params = PreviewParams(); Params->SetStringField(TEXT("expected_project"), TEXT("/other/Project.uproject"));
    TestEqual(TEXT("exact project binding"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), Params, Identity())), FString(TEXT("wrong_project")));
    Params = PreviewParams(); Params->GetObjectField(TEXT("expected_state"))->SetStringField(TEXT("revision"), TEXT("old"));
    TestEqual(TEXT("stale inspected state rejected"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), Params, Identity())), FString(TEXT("stale_plan")));
    auto Playing = Identity(); Playing->SetBoolField(TEXT("play_in_editor"), true);
    TestEqual(TEXT("PIE refused"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Playing)), FString(TEXT("editor_busy")));
    auto Preview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity());
    if (!TestTrue(TEXT("guard fixture preview"), Preview->GetBoolField(TEXT("ok")))) return false;
    auto Commit = CommitParams(Preview);
    FCoreUObjectDelegates::OnObjectModified.Broadcast(BP);
    TestEqual(TEXT("notified object edits invalidate preview"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), Commit, Identity())), FString(TEXT("stale_plan")));
    TestEqual(TEXT("failed attempt is one-shot"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), Commit, Identity())), FString(TEXT("plan_consumed")));
    Preview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity()); Commit = CommitParams(Preview);
    GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnabled"), false, GGameIni);
    TestEqual(TEXT("revoked approval invalidates preview"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), Commit, Identity())), FString(TEXT("policy_invalid")));
    TestEqual(TEXT("disabled policy cannot preview"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity())), FString(TEXT("blueprint_compile_disabled")));
    GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnabled"), true, GGameIni);
    Preview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity()); Commit = CommitParams(Preview);
    Now += 120;
    TestEqual(TEXT("expiry boundary rejects commit"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), Commit, Identity())), FString(TEXT("expired_plan")));
    Preview = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity()); Commit = CommitParams(Preview);
    auto Changed = Identity(); Changed->SetStringField(TEXT("world_path"), TEXT("/other/World"));
    TestEqual(TEXT("world changes invalidate preview"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), Commit, Changed)), FString(TEXT("stale_plan")));
    GConfig->SetArray(TEXT("JevEditor.BlueprintCompilation"), TEXT("Targets"), {TEXT("fixture|") + BP->GetPathName(), TEXT("fixture|") + BP->GetPathName()}, GGameIni);
    TestEqual(TEXT("ambiguous policy fails closed"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity())), FString(TEXT("blueprint_compile_disabled")));
    Fixture.Approve(BP);
    Now += 901;
    auto Receipt = MakeShared<FJsonObject>(); Receipt->SetStringField(TEXT("plan_id"), Commit->GetStringField(TEXT("plan_id")));
    TestEqual(TEXT("old receipts pruned"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_receipt"), Receipt, Identity())), FString(TEXT("unknown_plan")));
    auto* Custom = Fixture.InspectionAsset(UJevBlueprintSubclassFixture::StaticClass(), TEXT("UnsupportedCompile")); Fixture.Approve(Custom);
    TestEqual(TEXT("custom subclasses cannot compile"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity())), FString(TEXT("asset_not_loaded")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintGraphWorkflow, "Jev.Editor.BlueprintGraphWorkflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintGraphWorkflow::RunTest(const FString&)
{
    using namespace JevBlueprintTests; FFixture Fixture; UBlueprint* BP = Fixture.CompileAsset(); UEdGraph* Graph = BP->UbergraphPages[0];
    bool Old = false; const bool Had = GConfig->GetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnableGraphEdits"), Old, GGameIni);
    ON_SCOPE_EXIT { if (Had) GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnableGraphEdits"), Old, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnableGraphEdits"), GGameIni); };
    GConfig->SetBool(TEXT("JevEditor.BlueprintCompilation"), TEXT("bEnableGraphEdits"), true, GGameIni); FJevBlueprintTools Tools;
    auto PreviewGraph = [&](const TSharedRef<FJsonObject>& Edit) { auto P = PreviewParams(); P->SetObjectField(TEXT("graph_edit"), Edit); return Tools.Execute(TEXT("blueprint_graph_preview"), P, Identity()); };
    auto Edit = MakeShared<FJsonObject>(); Edit->SetStringField(TEXT("operation"), TEXT("add_math_node")); Edit->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString()); Edit->SetStringField(TEXT("function"), TEXT("Add_IntInt")); Edit->SetNumberField(TEXT("x"), 320); Edit->SetNumberField(TEXT("y"), 80);
    const int32 OriginalCount = Graph->Nodes.Num(); auto Plan = PreviewGraph(Edit); if (!TestTrue(TEXT("add math node previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("preview preserves graph"), Graph->Nodes.Num(), OriginalCount); const FString AddedId = Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("added_node_id"));
    auto Applied = Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity()); if (!TestTrue(TEXT("add and compile responds"), Applied->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("add graph compile passes"), Applied->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("passed"))); TestEqual(TEXT("one node added"), Graph->Nodes.Num(), OriginalCount + 1);
    UK2Node_CallFunction* First = nullptr; for (UEdGraphNode* N : Graph->Nodes) if (N->NodeGuid.ToString() == AddedId) First = Cast<UK2Node_CallFunction>(N); if (!TestNotNull(TEXT("planned node identity retained"), First)) return false;
    GEditor->UndoTransaction(); TestEqual(TEXT("Undo removes added node"), Graph->Nodes.Num(), OriginalCount);
    auto MakeNode = [&](const TCHAR* FunctionName) { auto* N = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional); N->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName)); N->CreateNewGuid(); N->AllocateDefaultPins(); Graph->AddNode(N, false, false); return N; };
    First = MakeNode(TEXT("Add_IntInt")); auto* Second = MakeNode(TEXT("Multiply_IntInt")); auto* Boolean = MakeNode(TEXT("Not_PreBool")); BP->Status = BS_Dirty;
    auto Link = MakeShared<FJsonObject>(); Link->SetStringField(TEXT("operation"), TEXT("connect")); Link->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString()); Link->SetStringField(TEXT("output_node_id"), First->NodeGuid.ToString()); Link->SetStringField(TEXT("output_pin_id"), First->FindPin(TEXT("ReturnValue"))->PinId.ToString()); Link->SetStringField(TEXT("input_node_id"), Second->NodeGuid.ToString()); Link->SetStringField(TEXT("input_pin_id"), Second->FindPin(TEXT("A"))->PinId.ToString());
    Plan = PreviewGraph(Link); if (!TestTrue(TEXT("exact typed connection previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    Applied = Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity()); TestTrue(TEXT("typed connection applies"), Applied->GetBoolField(TEXT("ok"))); TestTrue(TEXT("actual pins linked"), First->FindPin(TEXT("ReturnValue"))->LinkedTo.Contains(Second->FindPin(TEXT("A"))));
    TestEqual(TEXT("connection cannot replay"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity())), FString(TEXT("plan_consumed")));
    auto Remove = MakeShared<FJsonObject>(); Remove->SetStringField(TEXT("operation"), TEXT("remove_math_node")); Remove->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString()); Remove->SetStringField(TEXT("node_id"), Second->NodeGuid.ToString());
    TestEqual(TEXT("linked removal refused"), ErrorCode(PreviewGraph(Remove)), FString(TEXT("unsupported_graph_edit")));
    auto Cycle = MakeShared<FJsonObject>(); Cycle->Values = Link->Values; Cycle->SetStringField(TEXT("output_node_id"), Second->NodeGuid.ToString()); Cycle->SetStringField(TEXT("output_pin_id"), Second->FindPin(TEXT("ReturnValue"))->PinId.ToString()); Cycle->SetStringField(TEXT("input_node_id"), First->NodeGuid.ToString()); Cycle->SetStringField(TEXT("input_pin_id"), First->FindPin(TEXT("A"))->PinId.ToString());
    TestEqual(TEXT("cycle refused"), ErrorCode(PreviewGraph(Cycle)), FString(TEXT("unsupported_graph_edit")));
    auto Mismatch = MakeShared<FJsonObject>(); Mismatch->Values = Link->Values; Mismatch->SetStringField(TEXT("input_node_id"), Boolean->NodeGuid.ToString()); Mismatch->SetStringField(TEXT("input_pin_id"), Boolean->FindPin(TEXT("A"))->PinId.ToString()); TestEqual(TEXT("implicit numeric boolean coercion refused"), ErrorCode(PreviewGraph(Mismatch)), FString(TEXT("unsupported_graph_edit")));
    Link->SetStringField(TEXT("operation"), TEXT("disconnect")); Plan = PreviewGraph(Link); if (!TestTrue(TEXT("disconnect previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    Applied = Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity()); TestTrue(TEXT("disconnect applies"), Applied->GetBoolField(TEXT("ok"))); TestTrue(TEXT("actual input disconnected"), Second->FindPin(TEXT("A"))->LinkedTo.IsEmpty());
    Plan = PreviewGraph(Remove); if (!TestTrue(TEXT("unlinked removal previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    Second->NodePosX += 1; TestEqual(TEXT("silent graph change invalidates"), ErrorCode(Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity())), FString(TEXT("stale_plan")));
    Plan = PreviewGraph(Remove); Applied = Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity()); TestTrue(TEXT("reviewed removal applies"), Applied->GetBoolField(TEXT("ok"))); TestFalse(TEXT("removed node absent"), Graph->Nodes.Contains(Second)); GEditor->UndoTransaction(); TestTrue(TEXT("Undo restores removed node"), Graph->Nodes.Contains(Second));
    const FGuid SecondId = Second->NodeGuid; Second->NodeGuid = First->NodeGuid; TestEqual(TEXT("ambiguous node IDs refused"), ErrorCode(PreviewGraph(Edit)), FString(TEXT("unsupported_graph_edit"))); Second->NodeGuid = SecondId;
    const FGuid OriginalPinId = First->FindPin(TEXT("B"))->PinId; First->FindPin(TEXT("B"))->PinId = First->FindPin(TEXT("A"))->PinId; TestEqual(TEXT("ambiguous pins within one node refused"), ErrorCode(PreviewGraph(Edit)), FString(TEXT("unsupported_graph_edit"))); First->FindPin(TEXT("B"))->PinId = OriginalPinId;
    Edit->SetStringField(TEXT("function"), TEXT("ExecuteConsoleCommand")); TestEqual(TEXT("arbitrary functions refused"), ErrorCode(PreviewGraph(Edit)), FString(TEXT("unsupported_graph_edit")));
    auto Smuggled = PreviewParams(); Smuggled->SetObjectField(TEXT("graph_edit"), Remove); TestEqual(TEXT("explicit action required"), ErrorCode(Tools.Execute(TEXT("blueprint_compile_preview"), Smuggled, Identity())), FString(TEXT("bad_request")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintSpecializedCompilation, "Jev.Editor.BlueprintSpecializedCompilation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintSpecializedCompilation::RunTest(const FString&)
{
    using namespace JevBlueprintTests; FFixture Fixture; FJevBlueprintTools Tools;
    auto* WidgetFactory = NewObject<UWidgetBlueprintFactory>(); WidgetFactory->ParentClass = UUserWidget::StaticClass();
    auto* Widget = Cast<UWidgetBlueprint>(WidgetFactory->FactoryCreateNew(UWidgetBlueprint::StaticClass(), Fixture.Package, TEXT("WBP_CompileFixture"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    if (!TestNotNull(TEXT("native Widget Blueprint factory"), Widget)) return false; Fixture.Assets.Add(Widget); FAssetRegistryModule::AssetCreated(Widget);
    auto* Skeleton = NewObject<USkeleton>(Fixture.Package, TEXT("FixtureSkeleton"), RF_Public | RF_Standalone); Fixture.Assets.Add(Skeleton); FAssetRegistryModule::AssetCreated(Skeleton);
    { FReferenceSkeletonModifier Modifier(Skeleton); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
    auto* AnimFactory = NewObject<UAnimBlueprintFactory>(); AnimFactory->ParentClass = UAnimInstance::StaticClass(); AnimFactory->TargetSkeleton = Skeleton;
    auto* Animation = Cast<UAnimBlueprint>(AnimFactory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Fixture.Package, TEXT("ABP_CompileFixture"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    if (!TestNotNull(TEXT("native Animation Blueprint factory"), Animation)) return false; Fixture.Assets.Add(Animation); FAssetRegistryModule::AssetCreated(Animation);
    for (UBlueprint* BP : {static_cast<UBlueprint*>(Widget), static_cast<UBlueprint*>(Animation)})
    {
        Fixture.Approve(BP); auto Plan = Tools.Execute(TEXT("blueprint_compile_preview"), PreviewParams(), Identity()); if (!TestTrue(TEXT("specialized native Blueprint preview"), Plan->GetBoolField(TEXT("ok")))) return false;
        auto Result = Tools.Execute(TEXT("blueprint_compile"), CommitParams(Plan), Identity()); if (!TestTrue(TEXT("specialized native compile responds"), Result->GetBoolField(TEXT("ok")))) return false;
        TestEqual(TEXT("specialized asset fresh compiler acceptance"), Result->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("passed"))); TestEqual(TEXT("specialized asset no compiler errors"), Result->GetObjectField(TEXT("result"))->GetNumberField(TEXT("error_count")), 0.0);
        TestFalse(TEXT("specialized compile does not request save"), Result->GetObjectField(TEXT("result"))->GetBoolField(TEXT("save_requested")));
    }
    TestEqual(TEXT("animation skeleton preserved"), Animation->TargetSkeleton.Get(), Skeleton);
    return true;
}

#endif
