#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBlueprintTools.h"
#include "JevEditorProjectTools.h"
#include "JevBlueprintTestTypes.h"
#include "Animation/AnimBlueprint.h"
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

#endif
