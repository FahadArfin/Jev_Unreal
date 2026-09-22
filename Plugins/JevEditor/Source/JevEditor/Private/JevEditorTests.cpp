#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"

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
    return true;
}

#endif
