#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace JevEditTests
{
TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const TCHAR* Action, const TSharedRef<FJsonObject>& Params = MakeShared<FJsonObject>())
{
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), Action);
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

FString ErrorCode(const TSharedRef<FJsonObject>& Response)
{
    return Response->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : Response->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
}

TSharedRef<FJsonObject> Operation(const TCHAR* Type, AActor* Actor)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), Type);
    Result->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    return Result;
}

TSharedRef<FJsonObject> Preview(FJevEditorBridge& Bridge, const TArray<TSharedRef<FJsonObject>>& Operations, const TSharedPtr<FJsonObject>& State = nullptr)
{
    auto Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto& Op : Operations) Values.Add(MakeShared<FJsonValueObject>(Op));
    Params->SetArrayField(TEXT("operations"), Values);
    if (State) Params->SetObjectField(TEXT("expected_state"), State);
    return Call(Bridge, TEXT("preview"), Params);
}

TSharedRef<FJsonObject> Apply(FJevEditorBridge& Bridge, const TSharedRef<FJsonObject>& Plan)
{
    if (!Plan->GetBoolField(TEXT("ok"))) return Plan;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("plan_id"), Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")));
    return Call(Bridge, TEXT("apply"), Params);
}

TSharedRef<FJsonObject> State(FJevEditorBridge& Bridge)
{
    auto Status = Call(Bridge, TEXT("status"))->GetObjectField(TEXT("result"));
    auto Result = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) Result->SetStringField(Key, Status->GetStringField(Key));
    return Result;
}

TSharedRef<FJsonObject> Details(FJevEditorBridge& Bridge, const TArray<TSharedPtr<FJsonValue>>& Paths)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("actor_paths"), Paths);
    return Call(Bridge, TEXT("actor_details"), Params);
}

AStaticMeshActor* Cube(UWorld* World, const TCHAR* Label)
{
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags |= RF_Transactional;
    auto* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
    if (Actor)
    {
        Actor->GetStaticMeshComponent()->SetFlags(RF_Transactional);
        Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        Actor->SetActorLabel(Label);
    }
    return Actor;
}

int32 Count(UWorld* World)
{
    int32 Result = 0;
    for (TActorIterator<AActor> It(World); It; ++It) ++Result;
    return Result;
}

struct FMaterialFixtures
{
    UPackage* Package;
    UMaterial* Material;
    UMaterialInstanceConstant* Instance;
    UObjectRedirector* Redirector;
    UMaterial* Replacement = nullptr;

    FMaterialFixtures()
    {
        Package = CreatePackage(*(TEXT("/Game/JevMaterialFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        Material = NewObject<UMaterial>(Package, TEXT("Material"), RF_Public | RF_Standalone);
        Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Public | RF_Standalone);
        Instance->SetParentEditorOnly(UMaterial::GetDefaultMaterial(MD_Surface), false);
        Redirector = NewObject<UObjectRedirector>(Package, TEXT("Redirector"), RF_Public | RF_Standalone);
        Redirector->DestinationObject = Material;
        for (UObject* Asset : TArray<UObject*>{Material, Instance, Redirector}) FAssetRegistryModule::AssetCreated(Asset);
    }

    void ReplaceMaterial()
    {
        FAssetRegistryModule::AssetDeleted(Material);
        Material->Rename(TEXT("RetiredMaterial"), Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Replacement = NewObject<UMaterial>(Package, TEXT("Material"), RF_Public | RF_Standalone);
        FAssetRegistryModule::AssetCreated(Replacement);
    }

    ~FMaterialFixtures()
    {
        for (UObject* Asset : TArray<UObject*>{Replacement ? Replacement : Material, Instance, Redirector}) FAssetRegistryModule::AssetDeleted(Asset);
        for (UObject* Asset : TArray<UObject*>{Material, Instance, Redirector, Replacement})
            if (Asset) { Asset->ClearFlags(RF_Public | RF_Standalone); Asset->MarkAsGarbage(); }
        Package->SetDirtyFlag(false);
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevActorDetailsTest, "Jev.Editor.ActorDetails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevActorDetailsTest::RunTest(const FString& Parameters)
{
    using namespace JevEditTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    FJevEditorBridge Bridge;
    auto* First = Cube(World, TEXT("DetailFirst"));
    auto* Second = Cube(World, TEXT("DetailSecond"));
    auto* Plain = World->SpawnActor<AActor>();
    if (!First || !Second || !Plain) return false;
    First->SetActorLocation(FVector(100, 200, 300));
    First->SetActorScale3D(FVector(2, 1, 3));
    const auto FirstPath = MakeShared<FJsonValueString>(First->GetPathName());
    const auto SecondPath = MakeShared<FJsonValueString>(Second->GetPathName());
    const auto Inspection = Details(Bridge, {SecondPath, FirstPath});
    if (!TestTrue(TEXT("Exact actors can be inspected"), Inspection->GetBoolField(TEXT("ok")))) return false;
    const auto Result = Inspection->GetObjectField(TEXT("result"));
    const auto Actors = Result->GetArrayField(TEXT("actors"));
    TestEqual(TEXT("Requested order is preserved"), Actors[0]->AsObject()->GetStringField(TEXT("path")), Second->GetPathName());
    const auto Detail = Actors[1]->AsObject();
    TestTrue(TEXT("Native cube is editable"), Detail->GetBoolField(TEXT("editable")));
    TestEqual(TEXT("Editable actor has no blockers"), Detail->GetArrayField(TEXT("edit_blockers")).Num(), 0);
    TestTrue(TEXT("Mesh world bounds are available"), Detail->GetBoolField(TEXT("bounds_available")));
    const auto Bounds = Detail->GetObjectField(TEXT("bounds_cm"));
    TestEqual(TEXT("Bounds center follows world transform"), Bounds->GetArrayField(TEXT("center"))[0]->AsNumber(), 100.0);
    TestEqual(TEXT("Bounds size includes actor scale"), Bounds->GetArrayField(TEXT("size"))[2]->AsNumber(), 300.0);
    TestFalse(TEXT("Exact selections are never partial"), Result->GetBoolField(TEXT("truncated")));
    TestEqual(TEXT("Identity and actors share one revision"), Result->GetStringField(TEXT("revision")), State(Bridge)->GetStringField(TEXT("revision")));
    TestFalse(TEXT("Duplicate targets rejected"), Details(Bridge, {FirstPath, FirstPath})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Null targets rejected"), Details(Bridge, {MakeShared<FJsonValueNull>()})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Boolean targets rejected"), Details(Bridge, {MakeShared<FJsonValueBoolean>(true)})->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Empty target list rejected"), Details(Bridge, {})->GetBoolField(TEXT("ok")));
    TArray<TSharedPtr<FJsonValue>> Excess;
    for (int32 I = 0; I < 21; ++I) Excess.Add(MakeShared<FJsonValueString>(FString::FromInt(I)));
    TestFalse(TEXT("Selection count bounded"), Details(Bridge, Excess)->GetBoolField(TEXT("ok")));
    const auto Missing = Details(Bridge, {FirstPath, MakeShared<FJsonValueString>(TEXT("/Game/Missing.Missing:PersistentLevel.Missing"))});
    TestEqual(TEXT("Missing actor fails the entire inspection"), ErrorCode(Missing), FString(TEXT("actor_not_found")));
    TestFalse(TEXT("Failed inspection contains no partial result"), Missing->HasField(TEXT("result")));
    const auto PlainDetail = Details(Bridge, {MakeShared<FJsonValueString>(Plain->GetPathName())})->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject();
    TestFalse(TEXT("Rootless actor has no geometric bounds"), PlainDetail->GetBoolField(TEXT("bounds_available")));
    TestTrue(TEXT("Missing bounds are explicit null"), PlainDetail->Values[TEXT("bounds_cm")]->Type == EJson::Null);
    TestTrue(TEXT("Nonmesh identity is explicit null"), PlainDetail->Values[TEXT("static_mesh_path")]->Type == EJson::Null);
    TestTrue(TEXT("Nonmesh collision is explicit null"), PlainDetail->Values[TEXT("collision_enabled")]->Type == EJson::Null);
    TestFalse(TEXT("Unsupported actor cannot be edited"), PlainDetail->GetBoolField(TEXT("editable")));
    Second->AttachToActor(First, FAttachmentTransformRules::KeepWorldTransform);
    const auto Attached = Details(Bridge, {FirstPath, SecondPath})->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"));
    TestFalse(TEXT("Attachment parent is blocked"), Attached[0]->AsObject()->GetBoolField(TEXT("editable")));
    TestFalse(TEXT("Attachment child is blocked"), Attached[1]->AsObject()->GetBoolField(TEXT("editable")));
    Second->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    UStaticMesh* SlotFixture = NewObject<UStaticMesh>(GetTransientPackage());
    SlotFixture->GetStaticMaterials().SetNum(65);
    Second->GetStaticMeshComponent()->SetStaticMesh(SlotFixture);
    const auto Slots = Details(Bridge, {SecondPath})->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject();
    TestEqual(TEXT("Material listing bounded to 64 slots"), Slots->GetArrayField(TEXT("materials")).Num(), 64);
    TestTrue(TEXT("Material truncation is explicit"), Slots->GetBoolField(TEXT("materials_truncated")));
    TestTrue(TEXT("Unassigned material is explicit null"), Slots->GetArrayField(TEXT("materials"))[0]->AsObject()->Values[TEXT("path")]->Type == EJson::Null);
    const FString OriginalToken = Detail->GetStringField(TEXT("instance_id"));
    const auto Repeated = Details(Bridge, {FirstPath})->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject();
    TestEqual(TEXT("Instance identity is stable for the same live actor"), Repeated->GetStringField(TEXT("instance_id")), OriginalToken);
    const FString OriginalPath = First->GetPathName();
    const FName OriginalName = First->GetFName();
    const FTransform OriginalTransform = First->GetActorTransform();
    const auto BeforeReplacement = State(Bridge);
    First->Rename(nullptr, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
    World->EditorDestroyActor(First, true);
    FActorSpawnParameters ReplacementParams;
    ReplacementParams.Name = OriginalName;
    AStaticMeshActor* Replacement = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), OriginalTransform, ReplacementParams);
    if (!Replacement) return false;
    Replacement->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Replacement->SetActorLabel(TEXT("DetailFirst"));
    TestEqual(TEXT("Replacement reuses the original exact path"), Replacement->GetPathName(), OriginalPath);
    const auto Replaced = Details(Bridge, {FirstPath})->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject();
    TestNotEqual(TEXT("Replacement receives a different live instance identity"), Replaced->GetStringField(TEXT("instance_id")), OriginalToken);
    auto ReplacementEdit = Operation(TEXT("set_metadata"), Replacement);
    ReplacementEdit->SetStringField(TEXT("label"), TEXT("NoStaleMeasurement"));
    TestEqual(TEXT("Same-path replacement invalidates measured state before preview"), ErrorCode(Preview(Bridge, {ReplacementEdit}, BeforeReplacement)), FString(TEXT("stale_plan")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevExpectedStateTest, "Jev.Editor.ExpectedState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevExpectedStateTest::RunTest(const FString& Parameters)
{
    using namespace JevEditTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    auto* Actor = Cube(World, TEXT("ExpectedStateCube"));
    if (!Actor) return false;
    FJevEditorBridge Bridge;
    auto Op = Operation(TEXT("set_metadata"), Actor);
    Op->SetStringField(TEXT("label"), TEXT("ReviewedName"));
    TestTrue(TEXT("Current inspection state previews"), Preview(Bridge, {Op}, State(Bridge))->GetBoolField(TEXT("ok")));
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        auto Wrong = State(Bridge);
        Wrong->SetStringField(Key, TEXT("wrong"));
        TestEqual(TEXT("Every identity field guards preview"), ErrorCode(Preview(Bridge, {Op}, Wrong)), FString(TEXT("stale_plan")));
        auto Missing = State(Bridge);
        Missing->RemoveField(Key);
        TestEqual(TEXT("Partial expected_state rejected"), ErrorCode(Preview(Bridge, {Op}, Missing)), FString(TEXT("bad_request")));
        auto Coerced = State(Bridge);
        Coerced->SetBoolField(Key, true);
        TestEqual(TEXT("Identity values are never coerced"), ErrorCode(Preview(Bridge, {Op}, Coerced)), FString(TEXT("bad_request")));
    }
    auto Extra = State(Bridge);
    Extra->SetStringField(TEXT("ignored"), TEXT("not accepted"));
    TestEqual(TEXT("Unknown state field rejected"), ErrorCode(Preview(Bridge, {Op}, Extra)), FString(TEXT("bad_request")));
    const auto BeforeFolder = State(Bridge);
    Actor->SetFolderPath(FName(TEXT("ManualFolder")));
    TestEqual(TEXT("Folder edit invalidates inspection"), ErrorCode(Preview(Bridge, {Op}, BeforeFolder)), FString(TEXT("stale_plan")));
    const auto BeforeMaterial = State(Bridge);
    Actor->GetStaticMeshComponent()->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
    TestEqual(TEXT("Assignment edit invalidates inspection"), ErrorCode(Preview(Bridge, {Op}, BeforeMaterial)), FString(TEXT("stale_plan")));
    const auto Reviewed = Preview(Bridge, {Op}, State(Bridge));
    Actor->SetActorLabel(TEXT("ManualRename"));
    TestEqual(TEXT("Metadata change also invalidates an existing plan"), ErrorCode(Apply(Bridge, Reviewed)), FString(TEXT("stale_plan")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMetadataEditsTest, "Jev.Editor.MetadataEdits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMetadataEditsTest::RunTest(const FString& Parameters)
{
    using namespace JevEditTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    auto* Actor = Cube(World, TEXT("OriginalMetadata"));
    if (!Actor) return false;
    FJevEditorBridge Bridge;
    auto Op = Operation(TEXT("set_metadata"), Actor);
    Op->SetStringField(TEXT("label"), TEXT("  ReviewedMetadata  "));
    Op->SetStringField(TEXT("folder"), TEXT("Jev/Architecture"));
    const auto Plan = Preview(Bridge, {Op});
    if (!TestTrue(TEXT("Metadata previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    const auto Normalized = Plan->GetObjectField(TEXT("result"))->GetArrayField(TEXT("operations"))[0]->AsObject();
    TestEqual(TEXT("Preview shows actual normalized label"), Normalized->GetStringField(TEXT("label")), FString(TEXT("ReviewedMetadata")));
    TestTrue(TEXT("Metadata preview includes baseline transform"), Normalized->HasTypedField<EJson::Array>(TEXT("location")) && Normalized->HasTypedField<EJson::Array>(TEXT("rotation")) && Normalized->HasTypedField<EJson::Array>(TEXT("scale")));
    if (!TestTrue(TEXT("Metadata applies"), Apply(Bridge, Plan)->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("Label assigned"), Actor->GetActorLabel(), FString(TEXT("ReviewedMetadata")));
    TestEqual(TEXT("Folder assigned"), Actor->GetFolderPath().ToString(), FString(TEXT("Jev/Architecture")));
    auto Root = Operation(TEXT("set_metadata"), Actor);
    Root->SetStringField(TEXT("folder"), TEXT(""));
    TestTrue(TEXT("Empty folder moves actor to root"), Apply(Bridge, Preview(Bridge, {Root}))->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Root represented by NAME_None"), Actor->GetFolderPath().IsNone());
    TestTrue(TEXT("Root folder change supports Undo"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores folder"), Actor->GetFolderPath().ToString(), FString(TEXT("Jev/Architecture")));
    TestTrue(TEXT("Metadata change supports Undo"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores original label"), Actor->GetActorLabel(), FString(TEXT("OriginalMetadata")));
    TestTrue(TEXT("Undo restores original root folder"), Actor->GetFolderPath().IsNone());
    for (const TCHAR* Invalid : {TEXT("../Secret"), TEXT("A//B"), TEXT("/Absolute"), TEXT("A/./B"), TEXT("A\\B"), TEXT("A:B"), TEXT("Trailing/"), TEXT("None"), TEXT("A/ padded")})
    {
        auto Bad = Operation(TEXT("set_metadata"), Actor);
        Bad->SetStringField(TEXT("folder"), Invalid);
        TestEqual(TEXT("Unsafe folder rejected"), ErrorCode(Preview(Bridge, {Bad})), FString(TEXT("bad_request")));
    }
    auto Empty = Operation(TEXT("set_metadata"), Actor);
    TestFalse(TEXT("Metadata requires a requested change"), Preview(Bridge, {Empty})->GetBoolField(TEXT("ok")));
    Empty->SetStringField(TEXT("label"), TEXT("control\nlabel"));
    TestFalse(TEXT("Control characters rejected"), Preview(Bridge, {Empty})->GetBoolField(TEXT("ok")));
    auto Transform = Operation(TEXT("set_transform"), Actor);
    Transform->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(3)});
    TestEqual(TEXT("One actor cannot appear in mixed edit types"), ErrorCode(Preview(Bridge, {Op, Transform})), FString(TEXT("bad_request")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMaterialEditsTest, "Jev.Editor.MaterialEdits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMaterialEditsTest::RunTest(const FString& Parameters)
{
    using namespace JevEditTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    auto* Actor = Cube(World, TEXT("MaterialTarget"));
    if (!Actor) return false;
    FJevEditorBridge Bridge;
    FMaterialFixtures Fixtures;
    auto Op = Operation(TEXT("set_material"), Actor);
    Op->SetNumberField(TEXT("slot"), 0);
    const auto* Component = Actor->GetStaticMeshComponent();
    UMaterialInterface* Original = Component->GetEditorMaterial(0);
    const int32 OriginalOverrides = Component->GetNumOverrideMaterials();
    for (UMaterialInterface* Desired : TArray<UMaterialInterface*>{Fixtures.Material, Fixtures.Instance})
    {
        Op->SetStringField(TEXT("material_path"), Desired->GetPathName());
        const auto Plan = Preview(Bridge, {Op});
        if (!TestTrue(TEXT("Native material class previews"), Plan->GetBoolField(TEXT("ok")))) return false;
        if (!TestTrue(TEXT("Native material class applies"), Apply(Bridge, Plan)->GetBoolField(TEXT("ok")))) return false;
        TestTrue(TEXT("Exact selected material assigned"), Component->GetEditorMaterial(0) == Desired);
        TestTrue(TEXT("Material assignment participates in Undo"), GEditor->UndoTransaction());
        TestTrue(TEXT("Undo restores effective material"), Component->GetEditorMaterial(0) == Original);
        TestEqual(TEXT("Undo restores original override array size"), Component->GetNumOverrideMaterials(), OriginalOverrides);
    }
    Op->SetStringField(TEXT("material_path"), Fixtures.Material->GetPathName());
    for (const auto& InvalidSlot : TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueNumber>(0.5), MakeShared<FJsonValueNumber>(-1), MakeShared<FJsonValueNumber>(64), MakeShared<FJsonValueNull>()})
    {
        Op->SetField(TEXT("slot"), InvalidSlot);
        TestEqual(TEXT("Material slot type/range strict"), ErrorCode(Preview(Bridge, {Op})), FString(TEXT("bad_request")));
    }
    Op->SetNumberField(TEXT("slot"), 1);
    TestEqual(TEXT("Nonexistent material slot rejected"), ErrorCode(Preview(Bridge, {Op})), FString(TEXT("material_slot_invalid")));
    Op->SetNumberField(TEXT("slot"), 0);
    Op->SetStringField(TEXT("material_path"), Fixtures.Redirector->GetPathName());
    TestEqual(TEXT("Material redirector rejected"), ErrorCode(Preview(Bridge, {Op})), FString(TEXT("asset_unsupported")));
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanPathsSynchronous({TEXT("/Engine/BasicShapes")});
    Op->SetStringField(TEXT("material_path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    TestEqual(TEXT("Mesh cannot be assigned as material"), ErrorCode(Preview(Bridge, {Op})), FString(TEXT("asset_unsupported")));
    Op->SetStringField(TEXT("material_path"), TEXT("/Game/JevMissingMaterial.JevMissingMaterial"));
    TestEqual(TEXT("Missing material rejected"), ErrorCode(Preview(Bridge, {Op})), FString(TEXT("asset_not_found")));
    Op->SetStringField(TEXT("material_path"), TEXT("C:/external.mat"));
    TestEqual(TEXT("Filesystem path rejected"), ErrorCode(Preview(Bridge, {Op})), FString(TEXT("bad_request")));
    Op->SetStringField(TEXT("material_path"), Fixtures.Material->GetPathName());
    const auto StaleAssignment = Preview(Bridge, {Op});
    Actor->GetStaticMeshComponent()->SetMaterial(0, Fixtures.Instance);
    TestEqual(TEXT("Changed current assignment invalidates preview"), ErrorCode(Apply(Bridge, StaleAssignment)), FString(TEXT("stale_plan")));
    Actor->GetStaticMeshComponent()->SetMaterial(0, Original);
    const auto StaleAsset = Preview(Bridge, {Op});
    Fixtures.ReplaceMaterial();
    TestEqual(TEXT("Same-path replacement of desired material invalidates preview"), ErrorCode(Apply(Bridge, StaleAsset)), FString(TEXT("stale_plan")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevEditRollbackTest, "Jev.Editor.EditRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevEditRollbackTest::RunTest(const FString& Parameters)
{
    using namespace JevEditTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    auto* MetadataActor = Cube(World, TEXT("RollbackMetadata"));
    auto* MaterialActor = Cube(World, TEXT("RollbackMaterial"));
    auto* MovedActor = Cube(World, TEXT("RollbackTransform"));
    if (!MetadataActor || !MaterialActor || !MovedActor) return false;
    FJevEditorBridge Bridge;
    FMaterialFixtures Fixtures;
    const int32 OriginalCount = Count(World);
    const int32 OriginalOverrides = MaterialActor->GetStaticMeshComponent()->GetNumOverrideMaterials();
    UMaterialInterface* OriginalMaterial = MaterialActor->GetStaticMeshComponent()->GetEditorMaterial(0);
    auto Previous = Operation(TEXT("set_transform"), MovedActor);
    Previous->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(100), MakeShared<FJsonValueNumber>(200), MakeShared<FJsonValueNumber>(300)});
    if (!TestTrue(TEXT("Earlier independent operation applies"), Apply(Bridge, Preview(Bridge, {Previous}))->GetBoolField(TEXT("ok")))) return false;
    const FGuid EarlierTransactionId = GEditor->Trans->GetUndoContext().TransactionId;
    auto NoOp = Operation(TEXT("set_metadata"), MetadataActor);
    NoOp->SetStringField(TEXT("folder"), TEXT(""));
    const FString BeforeNoOp = State(Bridge)->GetStringField(TEXT("revision"));
    Bridge.FailApplyAfterOperationsForTesting(1);
    const auto NoOpFailure = Apply(Bridge, Preview(Bridge, {NoOp}));
    TestEqual(TEXT("Failure after no-op reports safely restored state"), ErrorCode(NoOpFailure), FString(TEXT("apply_failed")));
    TestEqual(TEXT("No-op rollback preserves full scene"), State(Bridge)->GetStringField(TEXT("revision")), BeforeNoOp);
    TestTrue(TEXT("No-op rollback never undoes an earlier edit"), MovedActor->GetActorLocation().Equals(FVector(100, 200, 300)));
    TestEqual(TEXT("No-op rollback preserves exact earlier Undo identity"), GEditor->Trans->GetUndoContext().TransactionId, EarlierTransactionId);
    auto Metadata = Operation(TEXT("set_metadata"), MetadataActor);
    Metadata->SetStringField(TEXT("label"), TEXT("MustBeRolledBack"));
    Metadata->SetStringField(TEXT("folder"), TEXT("JevRollback/NewFolder"));
    auto Material = Operation(TEXT("set_material"), MaterialActor);
    Material->SetStringField(TEXT("material_path"), Fixtures.Material->GetPathName());
    Material->SetNumberField(TEXT("slot"), 0);
    auto Move = Operation(TEXT("set_transform"), MovedActor);
    Move->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(900), MakeShared<FJsonValueNumber>(800), MakeShared<FJsonValueNumber>(700)});
    auto Spawn = MakeShared<FJsonObject>();
    Spawn->SetStringField(TEXT("op"), TEXT("spawn_primitive"));
    Spawn->SetStringField(TEXT("shape"), TEXT("Cube"));
    Spawn->SetStringField(TEXT("label"), TEXT("RollbackSpawn"));
    const FString Before = State(Bridge)->GetStringField(TEXT("revision"));
    const auto Plan = Preview(Bridge, {Metadata, Material, Move, Spawn});
    if (!TestTrue(TEXT("Mixed transaction previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    Bridge.FailApplyAfterOperationsForTesting(4);
    const auto Failure = Apply(Bridge, Plan);
    TestEqual(TEXT("Injected failure verifies complete rollback"), ErrorCode(Failure), FString(TEXT("apply_failed")));
    TestEqual(TEXT("Rollback restores exact scene revision"), State(Bridge)->GetStringField(TEXT("revision")), Before);
    TestEqual(TEXT("Rollback removes created actor"), Count(World), OriginalCount);
    TestEqual(TEXT("Rollback restores label"), MetadataActor->GetActorLabel(), FString(TEXT("RollbackMetadata")));
    TestTrue(TEXT("Rollback restores root folder"), MetadataActor->GetFolderPath().IsNone());
    TestTrue(TEXT("Rollback restores effective material"), MaterialActor->GetStaticMeshComponent()->GetEditorMaterial(0) == OriginalMaterial);
    TestEqual(TEXT("Rollback restores raw override array"), MaterialActor->GetStaticMeshComponent()->GetNumOverrideMaterials(), OriginalOverrides);
    TestTrue(TEXT("Rollback restores transform"), MovedActor->GetActorLocation().Equals(FVector(100, 200, 300)));
    TestFalse(TEXT("Failed transaction cannot be redone"), GEditor->Trans->CanRedo());
    TestTrue(TEXT("Earlier Undo entry remains available"), GEditor->UndoTransaction());
    TestTrue(TEXT("Earlier Undo still restores its own change"), MovedActor->GetActorLocation().Equals(FVector::ZeroVector));
    const FGuid EarlierRedoId = GEditor->Trans->GetRedoContext().TransactionId;
    Bridge.FailApplyAfterOperationsForTesting(1);
    TestEqual(TEXT("No-op failure is safe with an existing redo stack"), ErrorCode(Apply(Bridge, Preview(Bridge, {NoOp}))), FString(TEXT("apply_failed")));
    TestEqual(TEXT("No-op rollback preserves exact earlier Redo identity"), GEditor->Trans->GetRedoContext().TransactionId, EarlierRedoId);
    TestTrue(TEXT("No-op failure preserves previously undone scene"), MovedActor->GetActorLocation().Equals(FVector::ZeroVector));
    const auto BusyPlan = Preview(Bridge, {Metadata});
    {
        FScopedTransaction Existing(NSLOCTEXT("JevTests", "Existing", "Existing editor operation"));
        TestEqual(TEXT("Active editor transaction prevents bridge edits"), ErrorCode(Apply(Bridge, BusyPlan)), FString(TEXT("editor_busy")));
        TestEqual(TEXT("Busy rejection leaves actor untouched"), MetadataActor->GetActorLabel(), FString(TEXT("RollbackMetadata")));
        Existing.Cancel();
    }
    return true;
}

#endif
