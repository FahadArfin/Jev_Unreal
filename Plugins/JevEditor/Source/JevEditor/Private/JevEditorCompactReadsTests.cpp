#if WITH_DEV_AUTOMATION_TESTS
#include "JevEditorBridge.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"

namespace JevCompactTests
{
TSharedRef<FJsonObject> Params(const FString& Text)
{
    TSharedPtr<FJsonObject> Result; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result); return Result.ToSharedRef();
}
TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const TSharedRef<FJsonObject>& Parameters)
{
    auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("action"), TEXT("compact_read")); Request->SetObjectField(TEXT("params"), Parameters); return Bridge.Execute(Request);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevCompactPages, "Jev.Editor.CompactReadPages", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevCompactPages::RunTest(const FString&)
{
    using namespace JevCompactTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("isolated world"), World)) return false;
    double Time = 10; FJevEditorBridge Bridge([&Time] { return Time; });
    TArray<AStaticMeshActor*> Actors;
    for (int32 I = 0; I < 3; ++I)
    {
        auto* Actor = World->SpawnActor<AStaticMeshActor>(); Actor->SetActorLabel(FString::Printf(TEXT("JevCompact_%d"), I)); Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"))); Actors.Add(Actor);
    }
    ON_SCOPE_EXIT { for (AActor* Actor : Actors) if (IsValid(Actor)) World->EditorDestroyActor(Actor, true); };
    Actors[0]->SetLockLocation(true);
    auto Query = Params(TEXT("{\"source\":\"actors\",\"query\":\"JevCompact_\",\"fields\":[\"label\",\"bounds_cm\"],\"page_size\":1}"));
    auto Response = Call(Bridge, Query);
    if (!TestTrue(TEXT("native projection succeeds"), Response->GetBoolField(TEXT("ok")))) return false;
    auto Result = Response->GetObjectField(TEXT("result"));
    TestEqual(TEXT("captured count"), Result->GetNumberField(TEXT("captured_count")), 3.0);
    TestEqual(TEXT("one native row on wire"), Result->GetArrayField(TEXT("items")).Num(), 1);
    auto Row = Result->GetArrayField(TEXT("items"))[0]->AsObject();
    TestTrue(TEXT("identity retained"), Row->HasField(TEXT("instance_id")) && Row->HasField(TEXT("path")));
    TestTrue(TEXT("bounds and blockers retained"), Row->HasField(TEXT("bounds_cm")) && Row->HasField(TEXT("bounds_available")) && Row->HasField(TEXT("editable")) && Row->HasField(TEXT("edit_blockers")));
    TestTrue(TEXT("truncation cannot be hidden"), Row->HasField(TEXT("materials_truncated")));
    TestFalse(TEXT("material array omitted"), Row->HasField(TEXT("materials")));
    TestFalse(TEXT("mesh settings omitted"), Row->HasField(TEXT("mesh_settings")));
    const FString Cursor = Result->GetStringField(TEXT("next_cursor")); Query->SetStringField(TEXT("cursor"), Cursor);
    auto Page = Call(Bridge, Query); TestTrue(TEXT("next page succeeds"), Page->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("pages share capture identity"), Page->GetObjectField(TEXT("result"))->GetStringField(TEXT("native_read_id")), Result->GetStringField(TEXT("native_read_id")));
    Query->SetStringField(TEXT("query"), TEXT("other"));
    TestEqual(TEXT("changed scope rejected"), Call(Bridge, Query)->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("invalid_cursor"))); Query->SetStringField(TEXT("query"), TEXT("JevCompact_"));
    FJevEditorBridge Other; TestEqual(TEXT("other bridge cannot reuse cursor"), Call(Other, Query)->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("read_expired")));
    Actors[0]->SetActorLocation(FVector(100, 0, 0));
    TestEqual(TEXT("changed scene rejects continuation"), Call(Bridge, Query)->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("stale_cursor")));
    Query->RemoveField(TEXT("cursor")); Response = Call(Bridge, Query); Query->SetStringField(TEXT("cursor"), Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("next_cursor"))); Time = 131;
    TestEqual(TEXT("expiry enforced"), Call(Bridge, Query)->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("read_expired")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevCompactAssetProjection, "Jev.Editor.CompactAssetProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevCompactAssetProjection::RunTest(const FString&)
{
    using namespace JevCompactTests;
    FAutomationEditorCommonUtils::CreateNewMap(); FJevEditorBridge Bridge;
    auto Query = Params(TEXT("{\"source\":\"asset_details\",\"path\":\"/Engine/BasicShapes/Cube.Cube\",\"fields\":[\"name\",\"static_mesh.bounds_cm\",\"static_mesh.lod_count\"]}"));
    auto Response = Call(Bridge, Query);
    if (!TestTrue(TEXT("exact mesh inspected natively"), Response->GetBoolField(TEXT("ok")))) return false;
    auto Row = Response->GetObjectField(TEXT("result"))->GetArrayField(TEXT("items"))[0]->AsObject(); auto Mesh = Row->GetObjectField(TEXT("static_mesh"));
    TestTrue(TEXT("mesh bounds measured"), Mesh->HasField(TEXT("bounds_cm")));
    TestFalse(TEXT("LOD array absent"), Mesh->HasField(TEXT("lods")));
    TestFalse(TEXT("material slots absent"), Mesh->HasField(TEXT("material_slots")));
    TestFalse(TEXT("collision details absent"), Mesh->HasField(TEXT("collision")));
    TestTrue(TEXT("truncation retained"), Mesh->HasField(TEXT("lods_truncated")) && Mesh->HasField(TEXT("materials_truncated")));
    Query->SetArrayField(TEXT("fields"), {MakeShared<FJsonValueString>(TEXT("name"))}); Row = Call(Bridge, Query)->GetObjectField(TEXT("result"))->GetArrayField(TEXT("items"))[0]->AsObject();
    TestFalse(TEXT("metadata-only read skips mesh details"), Row->HasField(TEXT("static_mesh")));
    Query->SetArrayField(TEXT("fields"), {MakeShared<FJsonValueString>(TEXT("arbitrary_property"))}); TestFalse(TEXT("unreviewed field refused"), Call(Bridge, Query)->GetBoolField(TEXT("ok")));
    Query->SetArrayField(TEXT("fields"), {MakeShared<FJsonValueString>(TEXT("name"))}); Query->SetNumberField(TEXT("page_size"), 101); TestFalse(TEXT("oversized page refused"), Call(Bridge, Query)->GetBoolField(TEXT("ok")));
    return true;
}
#endif
