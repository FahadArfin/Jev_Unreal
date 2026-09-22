#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorProjectTools.h"
#include "JevProjectTestTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DataValidation.h"
#include "UObject/Package.h"

namespace JevProjectTests
{
TSharedRef<FJsonObject> Identity()
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("project_file"), TEXT("/fixture/JevSandbox.uproject"));
    Result->SetStringField(TEXT("session_id"), TEXT("fixture-session"));
    Result->SetStringField(TEXT("world_path"), TEXT("/Game/FixtureWorld"));
    Result->SetStringField(TEXT("revision"), TEXT("fixture-revision"));
    Result->SetBoolField(TEXT("play_in_editor"), false);
    Result->SetBoolField(TEXT("simulating"), false);
    return Result;
}

TSharedRef<FJsonObject> PathParams(const FString& Path)
{
    auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("asset_path"), Path); return Result;
}

FString ErrorCode(const TSharedRef<FJsonObject>& Response)
{
    return Response->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : Response->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
}

struct FFixture
{
    UPackage* Package = nullptr;
    TArray<UObject*> Assets;
    FFixture()
    {
        Package = CreatePackage(*(TEXT("/Game/JevProjectFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        Package->AddToRoot();
    }
    UJevProjectFixtureAsset* Asset(const TCHAR* Name)
    {
        auto* Result = NewObject<UJevProjectFixtureAsset>(Package, Name, RF_Public | RF_Standalone);
        Assets.Add(Result); FAssetRegistryModule::AssetCreated(Result); return Result;
    }
    UBlueprint* Blueprint()
    {
        auto* Result = NewObject<UBlueprint>(Package, TEXT("BP_InspectionFixture"), RF_Public | RF_Standalone);
        Result->ParentClass = UObject::StaticClass();
        Result->Status = BS_Error;
        auto* Graph = NewObject<UEdGraph>(Result, TEXT("FixtureGraph"));
        Graph->GraphGuid = FGuid::NewGuid();
        for (int32 I = 0; I < 3; ++I)
        {
            auto* Node = NewObject<UEdGraphNode>(Graph);
            Node->NodeGuid = FGuid::NewGuid();
            Node->bHasCompilerMessage = I == 0;
            Node->ErrorType = EMessageSeverity::Error;
            Node->ErrorMsg = TEXT("Stored compiler error fixture: missing function.");
            Node->CreatePin(EGPD_Input, FName(TEXT("int")), FName(TEXT("Value")));
            Graph->Nodes.Add(Node);
        }
        Result->UbergraphPages.Add(Graph);
        FBPVariableDescription Variable;
        Variable.VarName = TEXT("FixtureCount"); Variable.VarGuid = FGuid::NewGuid(); Variable.VarType.PinCategory = TEXT("int");
        Result->NewVariables.Add(Variable);
        Assets.Add(Result); FAssetRegistryModule::AssetCreated(Result);
        Package->SetDirtyFlag(false);
        return Result;
    }
    ~FFixture()
    {
        for (UObject* Asset : Assets) { FAssetRegistryModule::AssetDeleted(Asset); Asset->ClearFlags(RF_Public | RF_Standalone); }
        Package->SetDirtyFlag(false); Package->RemoveFromRoot();
    }
};

struct FConfig
{
    bool Enabled = false, HadEnabled = false;
    double Timeout = 0; bool HadTimeout = false;
    TArray<FString> Entries;
    FConfig()
    {
        HadEnabled = GConfig->GetBool(TEXT("JevEditor.Validation"), TEXT("bEnabled"), Enabled, GGameIni);
        HadTimeout = GConfig->GetDouble(TEXT("JevEditor.Validation"), TEXT("MaxJobSeconds"), Timeout, GGameIni);
        GConfig->GetArray(TEXT("JevEditor.Validation"), TEXT("Rules"), Entries, GGameIni);
        GConfig->SetBool(TEXT("JevEditor.Validation"), TEXT("bEnabled"), true, GGameIni);
        GConfig->SetDouble(TEXT("JevEditor.Validation"), TEXT("MaxJobSeconds"), 30, GGameIni);
        GConfig->SetArray(TEXT("JevEditor.Validation"), TEXT("Rules"), {TEXT("fixture|") + UJevProjectFixtureValidator::StaticClass()->GetPathName()}, GGameIni);
        UJevProjectFixtureValidator::bAutomationEnabled = true; UJevProjectFixtureValidator::Executions = 0;
    }
    ~FConfig()
    {
        if (HadEnabled) GConfig->SetBool(TEXT("JevEditor.Validation"), TEXT("bEnabled"), Enabled, GGameIni);
        else GConfig->RemoveKey(TEXT("JevEditor.Validation"), TEXT("bEnabled"), GGameIni);
        if (HadTimeout) GConfig->SetDouble(TEXT("JevEditor.Validation"), TEXT("MaxJobSeconds"), Timeout, GGameIni);
        else GConfig->RemoveKey(TEXT("JevEditor.Validation"), TEXT("MaxJobSeconds"), GGameIni);
        GConfig->SetArray(TEXT("JevEditor.Validation"), TEXT("Rules"), Entries, GGameIni);
        UJevProjectFixtureValidator::bAutomationEnabled = false;
        UJevProjectFixtureValidator::OnValidate = {};
    }
};

TSharedRef<FJsonObject> StartParams(const TArray<UObject*>& Assets)
{
    auto Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Paths;
    for (const auto* Asset : Assets) Paths.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
    Params->SetArrayField(TEXT("asset_paths"), Paths);
    Params->SetArrayField(TEXT("rule_ids"), {MakeShared<FJsonValueString>(TEXT("fixture"))});
    const auto Current = Identity();
    Params->SetStringField(TEXT("expected_project"), Current->GetStringField(TEXT("project_file")));
    auto State = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) State->SetStringField(Key, Current->GetStringField(Key));
    Params->SetObjectField(TEXT("expected_state"), State);
    return Params;
}

TSharedRef<FJsonObject> JobParams(const TSharedRef<FJsonObject>& Started)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("job_id"), Started->GetObjectField(TEXT("result"))->GetStringField(TEXT("job_id")));
    return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevBlueprintInspection, "Jev.Editor.BlueprintInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevBlueprintInspection::RunTest(const FString& Parameters)
{
    using namespace JevProjectTests;
    FFixture Fixture; UBlueprint* BP = Fixture.Blueprint(); FJevProjectTools Tools;
    auto Params = PathParams(BP->GetPathName());
    auto Response = Tools.Execute(TEXT("blueprint_inspect"), Params, Identity());
    if (!TestTrue(TEXT("loaded native Blueprint inspection succeeds"), Response->GetBoolField(TEXT("ok")))) return false;
    auto Result = Response->GetObjectField(TEXT("result"));
    TestEqual(TEXT("existing error status is preserved"), Result->GetStringField(TEXT("compile_status")), FString(TEXT("error")));
    TestFalse(TEXT("no implicit compilation"), Result->GetBoolField(TEXT("compiled")));
    TestFalse(TEXT("inspection leaves clean package clean"), Fixture.Package->IsDirty());
    const auto Graph = Result->GetArrayField(TEXT("graphs"))[0]->AsObject();
    TestEqual(TEXT("stable graph identity"), Graph->GetStringField(TEXT("guid")), BP->UbergraphPages[0]->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
    TestTrue(TEXT("stored compiler diagnostic survives"), Graph->GetArrayField(TEXT("nodes"))[0]->AsObject()->GetStringField(TEXT("compiler_message")).Contains(TEXT("missing function")));
    TestEqual(TEXT("variable types exposed"), Result->GetArrayField(TEXT("variables"))[0]->AsObject()->GetObjectField(TEXT("type"))->GetStringField(TEXT("category")), FString(TEXT("int")));
    Params->SetNumberField(TEXT("node_limit"), 1);
    auto Limited = Tools.Execute(TEXT("blueprint_inspect"), Params, Identity())->GetObjectField(TEXT("result"));
    TestTrue(TEXT("bounded node results marked truncated"), Limited->GetBoolField(TEXT("truncated")));
    TestEqual(TEXT("exact node budget"), Limited->GetNumberField(TEXT("nodes_returned")), 1.0);
    BP->UbergraphPages[0]->SubGraphs.Add(BP->UbergraphPages[0]);
    BP->AddExtension(NewObject<UJevProjectFixtureExtension>(BP));
    UJevProjectFixtureExtension::GraphCallbacks = 0;
    auto Bounded = Tools.Execute(TEXT("blueprint_inspect"), Params, Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("cyclic stored subgraph traversal terminates"), Bounded->GetNumberField(TEXT("graph_count")), 1.0);
    TestEqual(TEXT("extension graph callbacks never execute"), UJevProjectFixtureExtension::GraphCallbacks, 0);
    TestTrue(TEXT("omitted extension graphs are explicit"), Bounded->GetBoolField(TEXT("extension_graphs_omitted")));
    BP->UbergraphPages[0]->SubGraphs.Reset();
    Params->SetBoolField(TEXT("compile"), true);
    TestEqual(TEXT("compile selector forbidden"), ErrorCode(Tools.Execute(TEXT("blueprint_inspect"), Params, Identity())), FString(TEXT("bad_request")));
    TestEqual(TEXT("native mesh is not Blueprint"), ErrorCode(Tools.Execute(TEXT("blueprint_inspect"), PathParams(TEXT("/Engine/BasicShapes/Cube.Cube")), Identity())), FString(TEXT("unsupported_asset")));
    TestEqual(TEXT("traversal forbidden"), ErrorCode(Tools.Execute(TEXT("blueprint_inspect"), PathParams(TEXT("/Game/../BP.BP")), Identity())), FString(TEXT("bad_request")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevAssetProjectInspection, "Jev.Editor.AssetProjectInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevAssetProjectInspection::RunTest(const FString& Parameters)
{
    using namespace JevProjectTests;
    FJevProjectTools Tools;
    auto Params = PathParams(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Params->SetNumberField(TEXT("limit"), 1);
    const auto Response = Tools.Execute(TEXT("asset_dependencies"), Params, Identity());
    if (!TestTrue(TEXT("registry dependency query succeeds"), Response->GetBoolField(TEXT("ok")))) return false;
    const auto Result = Response->GetObjectField(TEXT("result"));
    TestTrue(TEXT("dependency page bounded"), Result->GetArrayField(TEXT("edges")).Num() <= 1);
    TestEqual(TEXT("query scope explicit"), Result->GetStringField(TEXT("category")), FString(TEXT("package")));
    Params->SetStringField(TEXT("direction"), TEXT("referencers"));
    TestTrue(TEXT("referencer query supported"), Tools.Execute(TEXT("asset_dependencies"), Params, Identity())->GetBoolField(TEXT("ok")));
    Params->SetStringField(TEXT("category"), TEXT("recursive"));
    TestEqual(TEXT("arbitrary traversal disallowed"), ErrorCode(Tools.Execute(TEXT("asset_dependencies"), Params, Identity())), FString(TEXT("bad_request")));
    FFixture Fixture; auto* Asset = Fixture.Asset(TEXT("ImportedFixture"));
    FAssetRegistryModule::AssetDeleted(Asset);
    FAssetImportInfo Info;
    Info.SourceFiles.Emplace(TEXT("C:\\PrivateUser\\ClientSecret\\mesh.fbx"), FDateTime(2026, 1, 1));
    // UE5.8 ToJson interpolates filenames verbatim, so escape this intentionally
    // noncanonical Windows-path fixture before storing valid registry JSON.
    Asset->ImportMetadata = Info.ToJson().Replace(TEXT("\\"), TEXT("\\\\")); FAssetRegistryModule::AssetCreated(Asset);
    const auto ImportResponse = Tools.Execute(TEXT("asset_import_info"), PathParams(Asset->GetPathName()), Identity());
    if (!TestTrue(TEXT("import metadata inspection succeeds"), ImportResponse->GetBoolField(TEXT("ok")))) return false;
    const auto Import = ImportResponse->GetObjectField(TEXT("result"));
    TestTrue(TEXT("recorded import metadata parsed"), Import->GetBoolField(TEXT("metadata_parsed")));
    if (TestEqual(TEXT("one provenance record"), Import->GetArrayField(TEXT("sources")).Num(), 1))
        TestEqual(TEXT("private directories removed"), Import->GetArrayField(TEXT("sources"))[0]->AsObject()->GetStringField(TEXT("filename")), FString(TEXT("mesh.fbx")));
    TestFalse(TEXT("metadata read does not modify asset"), Import->GetBoolField(TEXT("modified")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevValidationJobs, "Jev.Editor.ValidationJobs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevValidationJobs::RunTest(const FString& Parameters)
{
    using namespace JevProjectTests;
    FConfig Config; FFixture Fixture; FJevProjectTools Tools;
    auto* Good = Fixture.Asset(TEXT("Good")); auto* Bad = Fixture.Asset(TEXT("Invalid")); auto* Skip = Fixture.Asset(TEXT("Skip"));
    auto Params = StartParams({Good, Bad, Skip});
    auto Started = Tools.Execute(TEXT("validation_start"), Params, Identity());
    if (!TestTrue(TEXT("explicit allowlisted job queued"), Started->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("queueing executes no validator"), UJevProjectFixtureValidator::Executions, 0);
    TestEqual(TEXT("one active job"), ErrorCode(Tools.Execute(TEXT("validation_start"), Params, Identity())), FString(TEXT("job_busy")));
    auto Id = JobParams(Started);
    Tools.Tick(Identity());
    auto Partial = Tools.Execute(TEXT("validation_job"), Id, Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("one asset/rule per tick"), Partial->GetNumberField(TEXT("completed")), 1.0);
    Tools.Tick(Identity()); Tools.Tick(Identity()); Tools.Tick(Identity());
    auto Done = Tools.Execute(TEXT("validation_job"), Id, Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("native validator job completes"), Done->GetStringField(TEXT("state")), FString(TEXT("completed")));
    TestEqual(TEXT("native failure is authoritative"), Done->GetStringField(TEXT("verdict")), FString(TEXT("invalid")));
    const auto& Rows = Done->GetArrayField(TEXT("results"));
    TestEqual(TEXT("positive fixture succeeds"), Rows[0]->AsObject()->GetStringField(TEXT("result")), FString(TEXT("valid")));
    TestEqual(TEXT("negative fixture fails"), Rows[1]->AsObject()->GetStringField(TEXT("result")), FString(TEXT("invalid")));
    TestEqual(TEXT("unsupported fixture not silently passed"), Rows[2]->AsObject()->GetStringField(TEXT("result")), FString(TEXT("not_validated")));
    TestEqual(TEXT("native error retained"), Rows[1]->AsObject()->GetNumberField(TEXT("error_count")), 1.0);
    TestFalse(TEXT("never requests save"), Done->GetBoolField(TEXT("save_requested")));
    TestTrue(TEXT("project callback save side effects are unknown"), Done->HasTypedField<EJson::Null>(TEXT("saved")));
    Started = Tools.Execute(TEXT("validation_start"), StartParams({Good, Bad}), Identity()); Id = JobParams(Started);
    Tools.Tick(Identity());
    auto Cancelled = Tools.Execute(TEXT("validation_cancel"), Id, Identity())->GetObjectField(TEXT("result"));
    Tools.Tick(Identity());
    TestEqual(TEXT("cooperative cancellation"), Cancelled->GetStringField(TEXT("state")), FString(TEXT("cancelled")));
    TestEqual(TEXT("completed evidence retained"), Cancelled->GetArrayField(TEXT("results")).Num(), 1);
    auto Wrong = Identity(); Wrong->SetStringField(TEXT("session_id"), TEXT("other-session"));
    TestEqual(TEXT("job bound to exact session"), ErrorCode(Tools.Execute(TEXT("validation_job"), Id, Wrong)), FString(TEXT("wrong_project")));
    GConfig->SetBool(TEXT("JevEditor.Validation"), TEXT("bEnabled"), false, GGameIni);
    TestEqual(TEXT("execution disabled by project configuration"), ErrorCode(Tools.Execute(TEXT("validation_start"), Params, Identity())), FString(TEXT("validation_disabled")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevValidationJobSafety, "Jev.Editor.ValidationJobSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevValidationJobSafety::RunTest(const FString& Parameters)
{
    using namespace JevProjectTests;
    FConfig Config; FFixture Fixture; FJevProjectTools Tools;
    auto* Good = Fixture.Asset(TEXT("Good")); auto* Verbose = Fixture.Asset(TEXT("Verbose")); auto* Slow = Fixture.Asset(TEXT("Slow"));
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        auto Stale = StartParams({Good}); Stale->GetObjectField(TEXT("expected_state"))->SetStringField(Key, TEXT("changed"));
        TestEqual(TEXT("validation rejects changed preflight state"), ErrorCode(Tools.Execute(TEXT("validation_start"), Stale, Identity())), FString(TEXT("stale_plan")));
        Stale = StartParams({Good}); Stale->GetObjectField(TEXT("expected_state"))->SetBoolField(Key, true);
        TestEqual(TEXT("validation state rejects non-string identity"), ErrorCode(Tools.Execute(TEXT("validation_start"), Stale, Identity())), FString(TEXT("bad_request")));
    }
    auto InvalidState = StartParams({Good}); InvalidState->SetStringField(TEXT("expected_project"), TEXT("/other/Game.uproject"));
    TestEqual(TEXT("validation rejects project replacement after preflight"), ErrorCode(Tools.Execute(TEXT("validation_start"), InvalidState, Identity())), FString(TEXT("wrong_project")));
    InvalidState = StartParams({Good}); InvalidState->RemoveField(TEXT("expected_state"));
    TestEqual(TEXT("validation state is mandatory at native boundary"), ErrorCode(Tools.Execute(TEXT("validation_start"), InvalidState, Identity())), FString(TEXT("bad_request")));
    InvalidState = StartParams({Good}); InvalidState->GetObjectField(TEXT("expected_state"))->SetBoolField(TEXT("ignore_revision"), true);
    TestEqual(TEXT("validation state rejects extra selectors"), ErrorCode(Tools.Execute(TEXT("validation_start"), InvalidState, Identity())), FString(TEXT("bad_request")));
    TestEqual(TEXT("preflight failures execute no validators"), UJevProjectFixtureValidator::Executions, 0);
    auto Params = StartParams({Good}); Params->SetArrayField(TEXT("rule_ids"), {MakeShared<FJsonValueString>(TEXT("not-approved"))});
    TestEqual(TEXT("rule aliases are allowlisted"), ErrorCode(Tools.Execute(TEXT("validation_start"), Params, Identity())), FString(TEXT("rule_not_allowed")));
    Params = StartParams({Good, Good});
    TestEqual(TEXT("duplicate assets rejected before execution"), ErrorCode(Tools.Execute(TEXT("validation_start"), Params, Identity())), FString(TEXT("bad_request")));
    auto Started = Tools.Execute(TEXT("validation_start"), StartParams({Verbose}), Identity());
    if (!TestTrue(TEXT("verbose fixture job queued"), Started->GetBoolField(TEXT("ok")))) return false;
    Tools.Tick(Identity()); Tools.Tick(Identity());
    auto Result = Tools.Execute(TEXT("validation_job"), JobParams(Started), Identity())->GetObjectField(TEXT("result"));
    TestTrue(TEXT("large diagnostic output marked truncated"), Result->GetBoolField(TEXT("truncated")));
    TestEqual(TEXT("message count bounded per rule"), Result->GetArrayField(TEXT("results"))[0]->AsObject()->GetArrayField(TEXT("messages")).Num(), 8);
    Started = Tools.Execute(TEXT("validation_start"), StartParams({Good}), Identity());
    auto NewWorld = Identity(); NewWorld->SetStringField(TEXT("world_path"), TEXT("/Game/Other")); Tools.Tick(NewWorld);
    Result = Tools.Execute(TEXT("validation_job"), JobParams(Started), Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("world change cancels pending validation"), Result->GetStringField(TEXT("stop_reason")), FString(TEXT("identity_changed")));
    Started = Tools.Execute(TEXT("validation_start"), StartParams({Good}), Identity());
    GConfig->SetBool(TEXT("JevEditor.Validation"), TEXT("bEnabled"), false, GGameIni); Tools.Tick(Identity());
    Result = Tools.Execute(TEXT("validation_job"), JobParams(Started), Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("revoked permission checked between calls"), Result->GetStringField(TEXT("stop_reason")), FString(TEXT("configuration_changed")));
    GConfig->SetBool(TEXT("JevEditor.Validation"), TEXT("bEnabled"), true, GGameIni);
    GConfig->SetDouble(TEXT("JevEditor.Validation"), TEXT("MaxJobSeconds"), 1, GGameIni);
    Started = Tools.Execute(TEXT("validation_start"), StartParams({Slow, Good}), Identity()); Tools.Tick(Identity());
    Result = Tools.Execute(TEXT("validation_job"), JobParams(Started), Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("timeout enforced after in-flight validator returns"), Result->GetStringField(TEXT("state")), FString(TEXT("timed_out")));
    TestEqual(TEXT("no second validation after timeout"), Result->GetNumberField(TEXT("completed")), 1.0);
    TestEqual(TEXT("timeout never claims overall validity"), Result->GetStringField(TEXT("verdict")), FString(TEXT("incomplete")));
    GConfig->SetDouble(TEXT("JevEditor.Validation"), TEXT("MaxJobSeconds"), 30, GGameIni);
    Started = Tools.Execute(TEXT("validation_start"), StartParams({Good}), Identity());
    auto ReentrantId = JobParams(Started);
    UJevProjectFixtureValidator::OnValidate = [&]()
    {
        TestEqual(TEXT("validator cannot reenter cancellation"), ErrorCode(Tools.Execute(TEXT("validation_cancel"), ReentrantId, Identity())), FString(TEXT("editor_busy")));
        TestEqual(TEXT("validator cannot queue a successor inside callback"), ErrorCode(Tools.Execute(TEXT("validation_start"), StartParams({Good}), Identity())), FString(TEXT("editor_busy")));
        Tools.Shutdown();
    };
    Tools.Tick(Identity()); UJevProjectFixtureValidator::OnValidate = {};
    Result = Tools.Execute(TEXT("validation_job"), ReentrantId, Identity())->GetObjectField(TEXT("result"));
    TestEqual(TEXT("callback cancellation is not overwritten by outer tick"), Result->GetStringField(TEXT("state")), FString(TEXT("cancelled")));
    TestEqual(TEXT("cancelled callback does not append stale success"), Result->GetNumberField(TEXT("completed")), 0.0);
    TestTrue(TEXT("subsequent independent job can be queued"), Tools.Execute(TEXT("validation_start"), StartParams({Good}), Identity())->GetBoolField(TEXT("ok")));
    Tools.Shutdown();
    return true;
}

#endif
