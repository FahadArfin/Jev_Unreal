#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Selection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace
{
TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const FString& Action, const FString& Parameters = TEXT("{}"))
{
    TSharedPtr<FJsonObject> Params;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Parameters), Params);
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), Action);
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

TSharedRef<FJsonObject> ApplyPlan(FJevEditorBridge& Bridge, const TSharedRef<FJsonObject>& Preview)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("plan_id"), Preview->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")));
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), TEXT("apply"));
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

int32 CountActors(UWorld* World)
{
    int32 Count = 0;
    for (TActorIterator<AActor> It(World); It; ++It) ++Count;
    return Count;
}

const FString SpawnParameters = TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"JevAutomationCube\",\"location\":[100,200,300]}]}");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevPlanLifecycleTest, "Jev.Editor.PlanLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevPlanLifecycleTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    const int32 OriginalCount = CountActors(World);
    const auto Preview = Call(Bridge, TEXT("preview"), SpawnParameters);
    if (!TestTrue(TEXT("Allowed primitive previews"), Preview->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("Preview does not modify the scene"), CountActors(World), OriginalCount);
    const auto Applied = ApplyPlan(Bridge, Preview);
    if (!TestTrue(TEXT("Previewed plan applies"), Applied->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("One actor was spawned"), CountActors(World), OriginalCount + 1);
    const auto Replay = ApplyPlan(Bridge, Preview);
    TestFalse(TEXT("A plan is single use"), Replay->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Replay returns unknown_plan"), Replay->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("unknown_plan")));
    AActor* Spawned = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It) if (It->GetActorLabel() == TEXT("JevAutomationCube")) Spawned = *It;
    if (!TestNotNull(TEXT("Spawned actor has requested label"), Spawned)) return false;
    TestTrue(TEXT("Spawned actor has requested location"), Spawned->GetActorLocation().Equals(FVector(100, 200, 300)));

    auto Operation = MakeShared<FJsonObject>();
    Operation->SetStringField(TEXT("op"), TEXT("set_transform"));
    Operation->SetStringField(TEXT("actor_path"), Spawned->GetPathName());
    Operation->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(400), MakeShared<FJsonValueNumber>(500), MakeShared<FJsonValueNumber>(600)});
    auto TransformParams = MakeShared<FJsonObject>();
    TransformParams->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Operation)});
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), TEXT("preview"));
    Request->SetObjectField(TEXT("params"), TransformParams);
    const auto TransformPreview = Bridge.Execute(Request);
    if (!TestTrue(TEXT("Actor transform previews"), TransformPreview->GetBoolField(TEXT("ok")))) return false;
    TestTrue(TEXT("Actor transform applies"), ApplyPlan(Bridge, TransformPreview)->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Actor moved to target"), Spawned->GetActorLocation().Equals(FVector(400, 500, 600)));
    TestTrue(TEXT("Transform participates in editor undo"), GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores original transform"), Spawned->GetActorLocation().Equals(FVector(100, 200, 300)));
    TestTrue(TEXT("Spawn participates in editor undo"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo removes spawned actor"), CountActors(World), OriginalCount);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevPlanSafetyTest, "Jev.Editor.PlanSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevPlanSafetyTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    double Now = 1000;
    FJevEditorBridge Bridge([&Now] { return Now; });
    const auto Preview = Call(Bridge, TEXT("preview"), SpawnParameters);
    if (!TestTrue(TEXT("Plan previews"), Preview->GetBoolField(TEXT("ok")))) return false;
    World->SpawnActor<AStaticMeshActor>();
    const int32 ChangedCount = CountActors(World);
    const auto Stale = ApplyPlan(Bridge, Preview);
    TestFalse(TEXT("Scene changes invalidate plans"), Stale->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Stale plan reports stale_plan"), Stale->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("stale_plan")));
    TestEqual(TEXT("Stale plan makes no changes"), CountActors(World), ChangedCount);
    const auto Expiring = Call(Bridge, TEXT("preview"), SpawnParameters);
    Now += 121;
    const auto Expired = ApplyPlan(Bridge, Expiring);
    TestFalse(TEXT("Expired plans cannot apply"), Expired->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Expiry reports expired_plan"), Expired->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("expired_plan")));
    FJevEditorBridge OtherSession;
    TestFalse(TEXT("Plans cannot cross bridge sessions"), ApplyPlan(OtherSession, Call(Bridge, TEXT("preview"), SpawnParameters))->GetBoolField(TEXT("ok")));

    // Object paths can be reused; plans must bind actual live UObject instances.
    AActor* Original = World->SpawnActor<AStaticMeshActor>();
    Original->SetActorLabel(TEXT("ReplacementTarget"));
    const FString OriginalPath = Original->GetPathName();
    const FName OriginalName = Original->GetFName();
    const auto BeforeReplacement = Call(Bridge, TEXT("preview"), SpawnParameters);
    Original->Rename(nullptr, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
    World->EditorDestroyActor(Original, true);
    FActorSpawnParameters ReplacementParams;
    ReplacementParams.Name = OriginalName;
    AActor* Replacement = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, ReplacementParams);
    if (!TestNotNull(TEXT("Replacement actor exists"), Replacement)) return false;
    Replacement->SetActorLabel(TEXT("ReplacementTarget"));
    TestEqual(TEXT("Replacement reproduces the original object path"), Replacement->GetPathName(), OriginalPath);
    TestFalse(TEXT("Replaced object identity invalidates the plan"), ApplyPlan(Bridge, BeforeReplacement)->GetBoolField(TEXT("ok")));

    Replacement->SetActorLabel(TEXT("\u6e2c"));
    const auto BeforeUnicodeChange = Call(Bridge, TEXT("preview"), SpawnParameters);
    Replacement->SetActorLabel(TEXT("\u8a66"));
    TestFalse(TEXT("Different Unicode labels change the revision"), ApplyPlan(Bridge, BeforeUnicodeChange)->GetBoolField(TEXT("ok")));

    const auto BeforeWorldChange = Call(Bridge, TEXT("preview"), SpawnParameters);
    FAutomationEditorCommonUtils::CreateNewMap();
    TestFalse(TEXT("A different editor world invalidates the plan"), ApplyPlan(Bridge, BeforeWorldChange)->GetBoolField(TEXT("ok")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevSchemaSafetyTest, "Jev.Editor.SchemaSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevSchemaSafetyTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    const int32 OriginalCount = CountActors(World);
    TestFalse(TEXT("Arbitrary execution is unavailable"), Call(Bridge, TEXT("execute_python"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Unexpected fields are rejected"), Call(Bridge, TEXT("status"), TEXT("{\"extra\":true}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Unbounded actor requests are rejected"), Call(Bridge, TEXT("actors"), TEXT("{\"limit\":201}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Fractional limits are rejected"), Call(Bridge, TEXT("actors"), TEXT("{\"limit\":1.5}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Boolean limits are rejected"), Call(Bridge, TEXT("actors"), TEXT("{\"limit\":true}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("String limits are rejected"), Call(Bridge, TEXT("actors"), TEXT("{\"limit\":\"2\"}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Boolean queries are rejected"), Call(Bridge, TEXT("actors"), TEXT("{\"query\":true}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Asset path traversal is rejected"), Call(Bridge, TEXT("assets"), TEXT("{\"path\":\"/Game/../Engine\"}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Empty plans are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Null operation elements are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[null]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("String operation elements are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[\"delete\"]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Coerced vector values are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"test\",\"location\":[true,\"2\",3]}]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Boolean labels are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":true}]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Nonallowlisted meshes are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"/Game/Evil\",\"label\":\"test\"}]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Negative scales are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"test\",\"scale\":[1,-1,1]}]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Out of bounds coordinates are rejected"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"test\",\"location\":[1000001,0,0]}]}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Mixed valid and invalid operations reject the entire plan"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"test\"},{\"op\":\"delete_actor\"}]}"))->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Rejected operations never mutate the world"), CountActors(World), OriginalCount);
    auto Oversized = MakeShared<FJsonObject>();
    Oversized->SetStringField(TEXT("label"), FString::ChrN(400000, TCHAR(0x6e2c)));
    const FString Bounded = FJevEditorBridge::BoundedResponseBody(Oversized);
    const FTCHARToUTF8 Encoded(*Bounded);
    TestTrue(TEXT("Response cap counts UTF-8 bytes, not TCHAR characters"), Encoded.Length() <= 1048576);
    TSharedPtr<FJsonObject> Parsed;
    if (TestTrue(TEXT("Oversized response becomes valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Bounded), Parsed)))
        TestEqual(TEXT("Oversized response has an actionable error"), Parsed->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("response_too_large")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevContextInspectionTest, "Jev.Editor.ContextInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevContextInspectionTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    AStaticMeshActor* First = World->SpawnActor<AStaticMeshActor>();
    AStaticMeshActor* Second = World->SpawnActor<AStaticMeshActor>();
    if (!First || !Second) return false;
    First->SetActorLabel(TEXT("JevContextFirst"));
    Second->SetActorLabel(TEXT("JevContextSecond"));
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(Second, true, false);
    const auto Context = Call(Bridge, TEXT("context"), TEXT("{\"query\":\"JevContext\",\"limit\":1}"));
    if (!TestTrue(TEXT("Context is available"), Context->GetBoolField(TEXT("ok")))) return false;
    const auto Result = Context->GetObjectField(TEXT("result"));
    TestEqual(TEXT("Actor output respects requested limit"), Result->GetArrayField(TEXT("actors")).Num(), 1);
    TestTrue(TEXT("Actor output reports truncation"), Result->GetBoolField(TEXT("actors_truncated")));
    TestTrue(TEXT("Context identifies the editor world"), Result->GetBoolField(TEXT("editor_world")));
    TestEqual(TEXT("Context names current level"), Result->GetStringField(TEXT("current_level")), World->GetCurrentLevel()->GetPathName());
    TestTrue(TEXT("Selected actor path is included"), Result->GetArrayField(TEXT("selected_actor_paths")).ContainsByPredicate([Second](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == Second->GetPathName(); }));
    GEditor->SelectActor(Second, false, false);

    // Verify the play-mode policy without starting or stopping someone else's PIE session.
    {
        const bool bPreviouslySimulating = GEditor->bIsSimulatingInEditor;
        GEditor->bIsSimulatingInEditor = true;
        TestTrue(TEXT("Context remains available while simulating"), Call(Bridge, TEXT("context"))->GetBoolField(TEXT("ok")));
        TestTrue(TEXT("Status remains available for the project handshake while simulating"), Call(Bridge, TEXT("status"))->GetBoolField(TEXT("ok")));
        TestFalse(TEXT("Editing remains blocked while simulating"), Call(Bridge, TEXT("preview"), SpawnParameters)->GetBoolField(TEXT("ok")));
        GEditor->bIsSimulatingInEditor = bPreviouslySimulating;
    }
    TestFalse(TEXT("Context rejects coerced limits"), Call(Bridge, TEXT("context"), TEXT("{\"limit\":true}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Context rejects unknown params"), Call(Bridge, TEXT("context"), TEXT("{\"include_secrets\":true}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Context rejects coerced query"), Call(Bridge, TEXT("context"), TEXT("{\"query\":1}"))->GetBoolField(TEXT("ok")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevSceneValidationTest, "Jev.Editor.SceneValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevSceneValidationTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    AStaticMeshActor* Empty = World->SpawnActor<AStaticMeshActor>();
    AStaticMeshActor* NoCollision = World->SpawnActor<AStaticMeshActor>();
    if (!Empty || !NoCollision) return false;
    Empty->SetActorLabel(TEXT("JevValidateEmpty"));
    Empty->SetActorScale3D(FVector(-1, 1, 1));
    NoCollision->SetActorLabel(TEXT("JevValidateNoCollision"));
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Known engine cube is available"), Cube)) return false;
    NoCollision->GetStaticMeshComponent()->SetStaticMesh(Cube);
    NoCollision->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    const auto Validation = Call(Bridge, TEXT("validate"), TEXT("{\"query\":\"JevValidate\",\"limit\":2}"));
    if (!TestTrue(TEXT("Deterministic validation succeeds"), Validation->GetBoolField(TEXT("ok")))) return false;
    const auto Result = Validation->GetObjectField(TEXT("result"));
    const auto& Warnings = Result->GetArrayField(TEXT("warnings"));
    auto HasWarning = [&Warnings](const FString& Code, const AActor* Actor)
    {
        return Warnings.ContainsByPredicate([&Code, Actor](const TSharedPtr<FJsonValue>& Value) { const auto Object = Value->AsObject(); return Object->GetStringField(TEXT("code")) == Code && Object->GetStringField(TEXT("actor_path")) == Actor->GetPathName(); });
    };
    TestTrue(TEXT("Missing mesh has its exact actor path"), HasWarning(TEXT("missing_mesh"), Empty));
    TestTrue(TEXT("Negative scale is a warning"), HasWarning(TEXT("negative_scale"), Empty));
    TestTrue(TEXT("Disabled collision is a warning"), HasWarning(TEXT("collision_disabled"), NoCollision));
    TestEqual(TEXT("Both matching actors were scanned"), Result->GetIntegerField(TEXT("scanned_actors")), 2);
    TestFalse(TEXT("Complete bounded scan is identified"), Result->GetBoolField(TEXT("scan_incomplete")));
    const auto Limited = Call(Bridge, TEXT("validate"), TEXT("{\"query\":\"JevValidate\",\"limit\":1}"));
    TestTrue(TEXT("Validation reports skipped matching actors"), Limited->GetObjectField(TEXT("result"))->GetBoolField(TEXT("scan_incomplete")));
    Empty->SetActorScale3D(FVector(0, 1, 1));
    const auto ZeroScale = Call(Bridge, TEXT("validate"), TEXT("{\"query\":\"JevValidateEmpty\"}"));
    TestTrue(TEXT("Zero scale is detected"), ZeroScale->GetObjectField(TEXT("result"))->GetArrayField(TEXT("warnings")).ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value) { return Value->AsObject()->GetStringField(TEXT("code")) == TEXT("zero_scale"); }));
    TestFalse(TEXT("Validation rejects unbounded requests"), Call(Bridge, TEXT("validate"), TEXT("{\"limit\":201}"))->GetBoolField(TEXT("ok")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevAssetInspectionTest, "Jev.Editor.AssetInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevAssetInspectionTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    // Register only the known fixture directory if the background registry scan is unfinished.
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanPathsSynchronous({TEXT("/Engine/BasicShapes")});
    const auto Asset = Call(Bridge, TEXT("asset_details"), TEXT("{\"path\":\"/Engine/BasicShapes/Cube.Cube\"}"));
    if (!TestTrue(TEXT("Explicit engine static mesh can be inspected"), Asset->GetBoolField(TEXT("ok")))) return false;
    const auto Result = Asset->GetObjectField(TEXT("result"));
    TestTrue(TEXT("Selected mesh is loaded"), Result->GetBoolField(TEXT("loaded")));
    const auto Details = Result->GetObjectField(TEXT("static_mesh"));
    const auto Bounds = Details->GetObjectField(TEXT("bounds_cm"));
    const auto& Size = Bounds->GetArrayField(TEXT("size"));
    TestEqual(TEXT("Bounds are three dimensions"), Size.Num(), 3);
    for (const auto& Axis : Size) TestTrue(TEXT("Engine cube measures 100 cm per axis"), FMath::IsNearlyEqual(Axis->AsNumber(), 100.0, 0.01));
    TestTrue(TEXT("At least one LOD is reported"), Details->GetIntegerField(TEXT("lod_count")) >= 1);
    TestTrue(TEXT("Material slots are bounded"), Details->GetArrayField(TEXT("material_slots")).Num() <= 64);
    TestTrue(TEXT("LODs are bounded"), Details->GetArrayField(TEXT("lods")).Num() <= 16);
    TestTrue(TEXT("Collision metadata is present"), Details->HasTypedField<EJson::Object>(TEXT("collision")));
    for (const FString& Invalid : {TEXT("{\"path\":true}"), TEXT("{\"path\":\"C:/private.txt\"}"), TEXT("{\"path\":\"/Game/PackageOnly\"}"), TEXT("{\"path\":\"/Game/../Engine/Cube.Cube\"}"), TEXT("{\"path\":\"/Engine/BasicShapes/Cube.Cube:Subobject\"}")})
        TestFalse(TEXT("Asset inspection rejects unsafe or malformed paths"), Call(Bridge, TEXT("asset_details"), Invalid)->GetBoolField(TEXT("ok")));
    const auto Missing = Call(Bridge, TEXT("asset_details"), TEXT("{\"path\":\"/Game/JevDefinitelyMissing.JevDefinitelyMissing\"}"));
    TestFalse(TEXT("Missing asset returns a structured failure"), Missing->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Missing asset error is actionable"), Missing->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("asset_not_found")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevCaptureSafetyTest, "Jev.Editor.CaptureSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevCaptureSafetyTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    TestFalse(TEXT("Capture cannot write arbitrary paths"), Call(Bridge, TEXT("capture"), TEXT("{\"path\":\"C:/private.png\"}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Capture dimension is bounded"), Call(Bridge, TEXT("capture"), TEXT("{\"max_dimension\":1025}"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Capture dimension rejects coercion"), Call(Bridge, TEXT("capture"), TEXT("{\"max_dimension\":\"64\"}"))->GetBoolField(TEXT("ok")));
    {
        TGuardValue<FLevelEditorViewportClient*> ViewportGuard(GCurrentLevelEditingViewportClient, nullptr);
        const auto Missing = Call(Bridge, TEXT("capture"), TEXT("{\"max_dimension\":64}"));
        TestFalse(TEXT("Missing viewport is a structured error"), Missing->GetBoolField(TEXT("ok")));
        TestEqual(TEXT("No desktop fallback occurs"), Missing->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("viewport_unavailable")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevFrameSafetyTest, "Jev.Editor.FrameSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevFrameSafetyTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Actor || !Cube) return false;
    Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
    Actor->SetActorLocation(FVector(300, 400, 500));
    const FTransform OriginalTransform = Actor->GetActorTransform();
    const int32 OriginalActorCount = CountActors(World);
    const bool bOriginallySelected = GEditor->GetSelectedActors()->IsSelected(Actor);
    const FString ExactPath = Actor->GetPathName();
    FLevelEditorViewportClient* OriginalClient = GCurrentLevelEditingViewportClient;
    const FVector OriginalCameraLocation = OriginalClient ? OriginalClient->GetViewLocation() : FVector::ZeroVector;
    const FRotator OriginalCameraRotation = OriginalClient ? OriginalClient->GetViewRotation() : FRotator::ZeroRotator;
    auto FrameCall = [&Bridge](const TArray<TSharedPtr<FJsonValue>>& Paths, TSharedPtr<FJsonValue> Padding = nullptr, TSharedPtr<FJsonValue> View = nullptr)
    {
        auto Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("actor_paths"), Paths);
        if (Padding) Params->SetField(TEXT("padding"), Padding);
        if (View) Params->SetField(TEXT("view"), View);
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("frame"));
        Request->SetObjectField(TEXT("params"), Params);
        return Bridge.Execute(Request);
    };
    const auto PathValue = MakeShared<FJsonValueString>(ExactPath);
    TestFalse(TEXT("Frame rejects no targets"), FrameCall({})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects duplicate exact paths"), FrameCall({PathValue, PathValue})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects non-string targets"), FrameCall({MakeShared<FJsonValueBoolean>(true)})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects null targets"), FrameCall({MakeShared<FJsonValueNull>()})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects coerced padding"), FrameCall({PathValue}, MakeShared<FJsonValueBoolean>(true))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects excessive padding"), FrameCall({PathValue}, MakeShared<FJsonValueNumber>(4.01))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects out-of-range padding"), FrameCall({PathValue}, MakeShared<FJsonValueNumber>(0.99))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects unknown camera preset"), FrameCall({PathValue}, nullptr, MakeShared<FJsonValueString>(TEXT("arbitrary")))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Frame rejects coerced camera preset"), FrameCall({PathValue}, nullptr, MakeShared<FJsonValueBoolean>(true))->GetBoolField(TEXT("ok")));
    TArray<TSharedPtr<FJsonValue>> ExcessPaths;
    for (int32 I = 0; I < 21; ++I) ExcessPaths.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("unused%d"), I)));
    TestFalse(TEXT("Frame is bounded to 20 targets"), FrameCall(ExcessPaths)->GetBoolField(TEXT("ok")));
    const auto Missing = FrameCall({PathValue, MakeShared<FJsonValueString>(TEXT("/Game/MissingWorld.MissingWorld:PersistentLevel.MissingActor"))});
    TestFalse(TEXT("A missing target rejects the whole frame request"), Missing->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Missing actor is actionable"), Missing->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("actor_not_found")));
    {
        TGuardValue<FLevelEditorViewportClient*> ViewportGuard(GCurrentLevelEditingViewportClient, nullptr);
        const auto NoViewport = FrameCall({PathValue});
        TestFalse(TEXT("Frame requires an existing viewport"), NoViewport->GetBoolField(TEXT("ok")));
        TestEqual(TEXT("Frame cannot fall back to other editor state"), NoViewport->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("viewport_unavailable")));
        for (const TCHAR* Preset : {TEXT("isometric"), TEXT("top"), TEXT("front"), TEXT("right")})
        {
            const auto PresetWithoutViewport = FrameCall({PathValue}, nullptr, MakeShared<FJsonValueString>(Preset));
            TestEqual(TEXT("Valid preset still requires a real viewport"), PresetWithoutViewport->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("viewport_unavailable")));
        }
    }
    TestTrue(TEXT("Failed frame requests preserve actor transforms"), Actor->GetActorTransform().Equals(OriginalTransform));
    TestEqual(TEXT("Failed frame requests preserve actor count"), CountActors(World), OriginalActorCount);
    TestEqual(TEXT("Failed frame requests preserve selection"), GEditor->GetSelectedActors()->IsSelected(Actor), bOriginallySelected);
    if (OriginalClient)
    {
        TestTrue(TEXT("Rejected frames preserve camera location"), OriginalClient->GetViewLocation().Equals(OriginalCameraLocation));
        TestTrue(TEXT("Rejected frames preserve camera rotation"), OriginalClient->GetViewRotation().Equals(OriginalCameraRotation));
    }
    TestFalse(TEXT("Frame rejects arbitrary camera or filesystem parameters"), Call(Bridge, TEXT("frame"), TEXT("{\"actor_paths\":[],\"camera_position\":[1,2,3]}"))->GetBoolField(TEXT("ok")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevStaticMeshPlacementTest, "Jev.Editor.StaticMeshPlacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevStaticMeshPlacementTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated editor world exists"), World)) return false;
    FJevEditorBridge Bridge;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanPathsSynchronous({TEXT("/Engine/BasicShapes")});
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Cube fixture is available"), Cube)) return false;
    const int32 OriginalCount = CountActors(World);
    auto PreviewAsset = [&Bridge](const FString& Path)
    {
        auto Operation = MakeShared<FJsonObject>();
        Operation->SetStringField(TEXT("op"), TEXT("spawn_static_mesh"));
        Operation->SetStringField(TEXT("asset_path"), Path);
        Operation->SetStringField(TEXT("label"), TEXT("JevPlacedMesh"));
        Operation->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(100), MakeShared<FJsonValueNumber>(200), MakeShared<FJsonValueNumber>(300)});
        auto Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Operation)});
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("preview"));
        Request->SetObjectField(TEXT("params"), Params);
        return Bridge.Execute(Request);
    };
    const auto Preview = PreviewAsset(Cube->GetPathName());
    if (!TestTrue(TEXT("Exact registry static mesh previews"), Preview->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("Mesh preview never spawns an actor"), CountActors(World), OriginalCount);
    const auto Applied = ApplyPlan(Bridge, Preview);
    if (!TestTrue(TEXT("Previewed static mesh applies"), Applied->GetBoolField(TEXT("ok")))) return false;
    AStaticMeshActor* Placed = nullptr;
    for (TActorIterator<AStaticMeshActor> It(World); It; ++It) if (It->GetActorLabel() == TEXT("JevPlacedMesh")) Placed = *It;
    if (!TestNotNull(TEXT("Placement creates a native StaticMeshActor"), Placed)) return false;
    TestTrue(TEXT("The exact selected asset was assigned"), Placed->GetStaticMeshComponent()->GetStaticMesh() == Cube);
    TestTrue(TEXT("Placement uses the requested transform"), Placed->GetActorLocation().Equals(FVector(100, 200, 300)));
    TestTrue(TEXT("Placement participates in editor Undo"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo removes the placed mesh actor"), CountActors(World), OriginalCount);

    TestFalse(TEXT("Missing asset rejects preview"), PreviewAsset(TEXT("/Game/JevMissingAsset.JevMissingAsset"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Package-only asset path rejects preview"), PreviewAsset(TEXT("/Game/PackageOnly"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Filesystem asset path rejects preview"), PreviewAsset(TEXT("C:/private.obj"))->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Subobject asset path rejects preview"), PreviewAsset(TEXT("/Engine/BasicShapes/Cube.Cube:Component"))->GetBoolField(TEXT("ok")));
    if (!TestNotNull(TEXT("Cube has a material fixture"), Cube->GetMaterial(0))) return false;
    const auto WrongClass = PreviewAsset(Cube->GetMaterial(0)->GetPathName());
    TestFalse(TEXT("Nonmesh assets reject preview"), WrongClass->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Nonmesh asset error is explicit"), WrongClass->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("asset_unsupported")));
    TestFalse(TEXT("Mesh placement rejects shape aliases"), Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_static_mesh\",\"asset_path\":\"/Engine/BasicShapes/Cube.Cube\",\"shape\":\"Cube\",\"label\":\"invalid\"}]}"))->GetBoolField(TEXT("ok")));

    // Unsaved fixtures exercise redirects and identical-path UObject replacement without
    // touching the engine asset or writing any asset package to disk.
    const FString PackageName = TEXT("/Game/JevAutomationFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Package = CreatePackage(*PackageName);
    UStaticMesh* OriginalMesh = NewObject<UStaticMesh>(Package, TEXT("Mesh"), RF_Public | RF_Standalone);
    UObjectRedirector* Redirector = NewObject<UObjectRedirector>(Package, TEXT("Redirector"), RF_Public | RF_Standalone);
    Redirector->DestinationObject = Cube;
    UStaticMesh* Replacement = nullptr;
    FAssetRegistryModule::AssetCreated(OriginalMesh);
    FAssetRegistryModule::AssetCreated(Redirector);
    ON_SCOPE_EXIT
    {
        FAssetRegistryModule::AssetDeleted(Redirector);
        if (Replacement) FAssetRegistryModule::AssetDeleted(Replacement);
        else FAssetRegistryModule::AssetDeleted(OriginalMesh);
        for (UObject* Object : TArray<UObject*>{OriginalMesh, Redirector, Replacement})
            if (Object) { Object->ClearFlags(RF_Public | RF_Standalone); Object->MarkAsGarbage(); }
        Package->SetDirtyFlag(false);
    };
    const auto Redirect = PreviewAsset(Redirector->GetPathName());
    TestFalse(TEXT("Asset redirectors cannot be placed"), Redirect->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Redirect error is explicit"), Redirect->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("asset_unsupported")));
    const FString ReusedPath = OriginalMesh->GetPathName();
    const auto BeforeReplacement = PreviewAsset(ReusedPath);
    if (!TestTrue(TEXT("Unsaved static mesh fixture previews"), BeforeReplacement->GetBoolField(TEXT("ok")))) return false;
    FAssetRegistryModule::AssetDeleted(OriginalMesh);
    OriginalMesh->Rename(TEXT("RetiredMesh"), Package, REN_DontCreateRedirectors | REN_NonTransactional);
    Replacement = NewObject<UStaticMesh>(Package, TEXT("Mesh"), RF_Public | RF_Standalone);
    FAssetRegistryModule::AssetCreated(Replacement);
    TestEqual(TEXT("Replacement reuses the exact reviewed asset path"), Replacement->GetPathName(), ReusedPath);
    const auto Stale = ApplyPlan(Bridge, BeforeReplacement);
    TestFalse(TEXT("Replacement asset invalidates the preview"), Stale->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Replacement reports stale_plan"), Stale->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("stale_plan")));
    TestEqual(TEXT("Rejected mesh placement never changes the actor count"), CountActors(World), OriginalCount);
    return true;
}

#endif
