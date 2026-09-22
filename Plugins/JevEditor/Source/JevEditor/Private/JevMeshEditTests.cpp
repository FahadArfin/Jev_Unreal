#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "JevEditorReviewPanel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace JevMeshTests
{
TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const TCHAR* Action, const TSharedRef<FJsonObject>& Params = MakeShared<FJsonObject>())
{
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), Action);
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

FString ErrorCode(const TSharedRef<FJsonObject>& Result)
{
    return Result->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : Result->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
}

TSharedRef<FJsonObject> Preview(FJevEditorBridge& Bridge, const TArray<TSharedRef<FJsonObject>>& Operations)
{
    auto Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto& Operation : Operations) Values.Add(MakeShared<FJsonValueObject>(Operation));
    Params->SetArrayField(TEXT("operations"), Values);
    return Call(Bridge, TEXT("preview"), Params);
}

TSharedRef<FJsonObject> ByPlan(FJevEditorBridge& Bridge, const TCHAR* Action, const TSharedRef<FJsonObject>& Plan)
{
    if (!Plan->GetBoolField(TEXT("ok"))) return Plan;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("plan_id"), Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")));
    return Call(Bridge, Action, Params);
}

TSharedRef<FJsonObject> Replace(AActor* Actor, UStaticMesh* Mesh, const TCHAR* Policy = TEXT("preserve_slots"))
{
    auto Operation = MakeShared<FJsonObject>();
    Operation->SetStringField(TEXT("op"), TEXT("replace_mesh"));
    Operation->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    Operation->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
    Operation->SetStringField(TEXT("material_policy"), Policy);
    return Operation;
}

TSharedRef<FJsonObject> Duplicate(AActor* Actor)
{
    auto Operation = MakeShared<FJsonObject>();
    Operation->SetStringField(TEXT("op"), TEXT("duplicate_mesh"));
    Operation->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    Operation->SetStringField(TEXT("label"), TEXT("ControlledCopy"));
    return Operation;
}

TSharedPtr<FJsonObject> Details(FJevEditorBridge& Bridge, AActor* Actor)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("actor_paths"), {MakeShared<FJsonValueString>(Actor->GetPathName())});
    return Call(Bridge, TEXT("actor_details"), Params)->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject();
}

FString Encode(const TSharedPtr<FJsonObject>& Object)
{
    FString Result;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
    FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
    return Result;
}

FString Revision(FJevEditorBridge& Bridge)
{
    return Call(Bridge, TEXT("status"))->GetObjectField(TEXT("result"))->GetStringField(TEXT("revision"));
}

int32 Count(UWorld* World)
{
    int32 Result = 0;
    for (TActorIterator<AActor> It(World); It; ++It) ++Result;
    return Result;
}

AStaticMeshActor* Cube(UWorld* World, const TCHAR* Label)
{
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanPathsSynchronous({TEXT("/Engine/BasicShapes")});
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags |= RF_Transactional;
    Spawn.OverrideLevel = World->GetCurrentLevel();
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
    Actor->GetStaticMeshComponent()->SetFlags(RF_Transactional);
    Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Actor->SetActorLabel(Label);
    return Actor;
}

struct FAssets
{
    UPackage* Package;
    UStaticMesh* Mesh;
    UMaterial* Material;
    TArray<UObject*> Assets;

    FAssets()
    {
        Package = CreatePackage(*(TEXT("/Game/JevMeshFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        Material = NewObject<UMaterial>(Package, TEXT("Material"), RF_Public | RF_Standalone);
        Mesh = NewObject<UStaticMesh>(Package, TEXT("Mesh"), RF_Public | RF_Standalone);
        Mesh->GetStaticMaterials().Add(FStaticMaterial(Material));
        Mesh->CreateBodySetup();
        Mesh->GetBodySetup()->DefaultInstance.SetCollisionProfileName(TEXT("BlockAll"));
        Assets = {Mesh, Material};
        for (UObject* Asset : Assets) FAssetRegistryModule::AssetCreated(Asset);
    }

    void ReplaceMeshObject()
    {
        FAssetRegistryModule::AssetDeleted(Mesh);
        Mesh->Rename(TEXT("RetiredMesh"), Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Mesh = NewObject<UStaticMesh>(Package, TEXT("Mesh"), RF_Public | RF_Standalone);
        Mesh->GetStaticMaterials().Add(FStaticMaterial(Material));
        Mesh->CreateBodySetup();
        Mesh->GetBodySetup()->DefaultInstance.SetCollisionProfileName(TEXT("BlockAll"));
        Assets.Add(Mesh);
        FAssetRegistryModule::AssetCreated(Mesh);
    }

    ~FAssets()
    {
        for (UObject* Asset : Assets)
        {
            FAssetRegistryModule::AssetDeleted(Asset);
            Asset->ClearFlags(RF_Public | RF_Standalone);
            Asset->MarkAsGarbage();
        }
        Package->SetDirtyFlag(false);
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMeshReplacementTest, "Jev.Editor.MeshReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMeshReplacementTest::RunTest(const FString& Parameters)
{
    using namespace JevMeshTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    AStaticMeshActor* Actor = Cube(World, TEXT("ReplacementSource"));
    UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
    UStaticMesh* OriginalMesh = Component->GetStaticMesh();
    UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    FAssets Assets;
    FJevEditorBridge Bridge;
    Component->SetMaterial(0, Assets.Material);
    Actor->SetActorTransform(FTransform(FRotator(10, 20, 30), FVector(100, 200, 300), FVector(2, 3, 4)));
    Actor->SetFolderPath(TEXT("JevMesh/Replacement"));
    Actor->SetPivotOffset(FVector(3, 4, 5));
    Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Actor->SetActorEnableCollision(false);
    const FString Before = Encode(Details(Bridge, Actor));
    const FString Instance = Details(Bridge, Actor)->GetStringField(TEXT("instance_id"));
    const FTransform Transform = Actor->GetActorTransform();
    for (const TCHAR* Policy : {TEXT("preserve_slots"), TEXT("mesh_defaults")})
    {
        const auto Plan = Preview(Bridge, {Replace(Actor, Sphere, Policy)});
        if (!TestTrue(TEXT("Both replacement policies preview"), Plan->GetBoolField(TEXT("ok")))) return false;
        const auto Normal = Plan->GetObjectField(TEXT("result"))->GetArrayField(TEXT("operations"))[0]->AsObject();
        TestEqual(TEXT("Preview binds original actor instance"), Normal->GetStringField(TEXT("source_instance_id")), Instance);
        TestEqual(TEXT("Preview records unmasked collision mode"), Normal->GetObjectField(TEXT("mesh_settings"))->GetIntegerField(TEXT("collision_mode")), static_cast<int32>(ECollisionEnabled::QueryAndPhysics));
        TestEqual(TEXT("Preview retains actual source folder"), Normal->GetStringField(TEXT("folder")), FString(TEXT("JevMesh/Replacement")));
        TestTrue(TEXT("Review exposes old and new bounds/collision semantics"), Normal->GetObjectField(TEXT("mesh_review"))->HasTypedField<EJson::Object>(TEXT("source_mesh")) && Normal->GetObjectField(TEXT("mesh_review"))->HasTypedField<EJson::Object>(TEXT("result_mesh")));
        const auto Record = ByPlan(Bridge, TEXT("plan_status"), Plan)->GetObjectField(TEXT("result"));
        const FString Review = FJevEditorReviewPanel::DescribeReview(Record);
        TestTrue(TEXT("Human review describes replacement and policy"), Review.Contains(TEXT("Replace mesh:")) && Review.Contains(TEXT("Material policy:")) && Review.Contains(Policy));
        TestTrue(TEXT("Human review includes exact asset and settings"), Review.Contains(Sphere->GetPathName()) && Review.Contains(TEXT("collision_mode")));
        TestEqual(TEXT("Preview is read-only"), Encode(Details(Bridge, Actor)), Before);
        const auto Applied = ByPlan(Bridge, TEXT("apply"), Plan);
        if (!TestTrue(TEXT("Replacement applies"), Applied->GetBoolField(TEXT("ok")))) { AddError(Encode(Applied)); return false; }
        TestTrue(TEXT("Exact replacement mesh assigned"), Component->GetStaticMesh() == Sphere);
        TestTrue(TEXT("Actor transform retained"), Actor->GetActorTransform().Equals(Transform));
        TestTrue(TEXT("Actor pivot retained"), Actor->GetPivotOffset().Equals(FVector(3, 4, 5)));
        TestEqual(TEXT("Actor identity retained"), Details(Bridge, Actor)->GetStringField(TEXT("instance_id")), Instance);
        TestFalse(TEXT("Actor collision remains disabled"), Actor->GetActorEnableCollision());
        TestEqual(TEXT("Raw collision mode is retained behind actor flag"), Component->BodyInstance.GetCollisionEnabled(false), ECollisionEnabled::QueryAndPhysics);
        TestTrue(TEXT("Material policy matches actual material"), Component->GetEditorMaterial(0) == (FCString::Strcmp(Policy, TEXT("preserve_slots")) == 0 ? Assets.Material : Sphere->GetMaterial(0)));
        TestEqual(TEXT("Override array follows explicit policy"), Component->GetNumOverrideMaterials(), FCString::Strcmp(Policy, TEXT("preserve_slots")) == 0 ? 1 : 0);
        TestEqual(TEXT("Consumed replacement cannot replay"), ErrorCode(ByPlan(Bridge, TEXT("apply"), Plan)), FString(TEXT("unknown_plan")));
        TestTrue(TEXT("Replacement supports editor Undo"), GEditor->UndoTransaction());
        TestTrue(TEXT("Undo restores old mesh"), Component->GetStaticMesh() == OriginalMesh);
        TestEqual(TEXT("Undo restores complete inspected actor"), Encode(Details(Bridge, Actor)), Before);
    }
    Assets.Mesh->GetStaticMaterials().Add(FStaticMaterial(Assets.Material));
    TestEqual(TEXT("Preserve policy rejects unequal slot counts"), ErrorCode(Preview(Bridge, {Replace(Actor, Assets.Mesh)})), FString(TEXT("material_slot_invalid")));
    TestTrue(TEXT("Defaults policy explicitly accepts changed slot counts"), Preview(Bridge, {Replace(Actor, Assets.Mesh, TEXT("mesh_defaults"))})->GetBoolField(TEXT("ok")));
    auto Extra = Replace(Actor, Sphere);
    Extra->SetArrayField(TEXT("location"), {});
    TestEqual(TEXT("Replacement does not accept transform or hidden fields"), ErrorCode(Preview(Bridge, {Extra})), FString(TEXT("bad_request")));
    Component->EmptyOverrideMaterials();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMeshDuplicateTest, "Jev.Editor.MeshDuplicate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMeshDuplicateTest::RunTest(const FString& Parameters)
{
    using namespace JevMeshTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    AStaticMeshActor* Source = Cube(World, TEXT("DuplicateSource"));
    UStaticMeshComponent* Component = Source->GetStaticMeshComponent();
    FAssets Assets;
    FJevEditorBridge Bridge;
    const auto DefaultCopy = Preview(Bridge, {Duplicate(Source)});
    if (!TestTrue(TEXT("Default controlled copy previews"), DefaultCopy->GetBoolField(TEXT("ok")))) { AddError(Encode(DefaultCopy)); return false; }
    const auto DefaultApplied = ByPlan(Bridge, TEXT("apply"), DefaultCopy);
    if (!TestTrue(TEXT("Default controlled copy applies"), DefaultApplied->GetBoolField(TEXT("ok")))) { AddError(Encode(DefaultApplied)); return false; }
    TestTrue(TEXT("Default copy retains mesh collision inheritance"), DefaultApplied->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject()->GetObjectField(TEXT("mesh_settings"))->GetBoolField(TEXT("use_mesh_default_collision")));
    if (!TestTrue(TEXT("Default copy can be undone"), GEditor->UndoTransaction())) return false;
    Source->SetFolderPath(TEXT("JevMesh/Source"));
    Source->SetActorTransform(FTransform(FRotator(10, 20, 30), FVector(100, 200, 300), FVector(2, 3, 4)));
    Component->SetMobility(EComponentMobility::Movable);
    Component->SetMaterial(0, Assets.Material);
    Component->SetCollisionProfileName(TEXT("Custom"));
    Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Component->SetCollisionObjectType(ECC_WorldDynamic);
    Component->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    Source->SetActorEnableCollision(false);
    Component->SetCastShadow(false);
    Component->SetReceivesDecals(false);
    Component->SetRenderCustomDepth(true);
    Component->SetCustomDepthStencilValue(87);
    Component->SetTranslucentSortPriority(-3);
    Component->SetVisibility(false);
    Component->SetHiddenInGame(true);
    Source->SetActorHiddenInGame(true);
    Source->SetIsTemporarilyHiddenInEditor(true);
    Source->Tags = {TEXT("SourceTag")};
    Component->ComponentTags = {TEXT("MeshTag")};
    const FString SourceBefore = Encode(Details(Bridge, Source));
    const auto SourceSettings = Details(Bridge, Source)->GetObjectField(TEXT("mesh_settings"));
    TestFalse(TEXT("Fixture explicitly selects custom component collision"), SourceSettings->GetBoolField(TEXT("use_mesh_default_collision")));
    const int32 BeforeCount = Count(World);
    for (bool bOffset : {false, true})
    {
        auto Operation = Duplicate(Source);
        if (bOffset) Operation->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(400), MakeShared<FJsonValueNumber>(500), MakeShared<FJsonValueNumber>(600)});
        const auto Plan = Preview(Bridge, {Operation});
        if (!TestTrue(TEXT("Controlled native copy previews with nondefault supported settings"), Plan->GetBoolField(TEXT("ok")))) { AddError(Encode(Plan)); return false; }
        const auto Normal = Plan->GetObjectField(TEXT("result"))->GetArrayField(TEXT("operations"))[0]->AsObject();
        TestEqual(TEXT("Copy preview keeps source selector explicit"), Normal->GetStringField(TEXT("source_actor_path")), Source->GetPathName());
        TestEqual(TEXT("Copy preview exposes copied settings"), Encode(Normal->GetObjectField(TEXT("mesh_settings"))), Encode(SourceSettings));
        const FString Review = FJevEditorReviewPanel::DescribeReview(ByPlan(Bridge, TEXT("plan_status"), Plan)->GetObjectField(TEXT("result")));
        TestTrue(TEXT("Human review distinguishes copy from source mutation"), Review.Contains(TEXT("Create controlled mesh copy:")) && Review.Contains(TEXT("ControlledCopy")) && Review.Contains(TEXT("DuplicateSource")));
        TestTrue(TEXT("Human review includes material/settings scope"), Review.Contains(Assets.Material->GetPathName()) && Review.Contains(TEXT("collision_responses")));
        const auto Applied = ByPlan(Bridge, TEXT("apply"), Plan);
        if (!TestTrue(TEXT("Controlled copy applies"), Applied->GetBoolField(TEXT("ok")))) { AddError(Encode(Applied)); return false; }
        const auto Result = Applied->GetObjectField(TEXT("result"))->GetArrayField(TEXT("actors"))[0]->AsObject();
        AStaticMeshActor* Copy = FindObject<AStaticMeshActor>(nullptr, *Result->GetStringField(TEXT("path")));
        if (!TestNotNull(TEXT("Result resolves exact new actor"), Copy)) return false;
        TestTrue(TEXT("Copy has different path and instance"), Copy != Source && Result->GetStringField(TEXT("instance_id")) != Normal->GetStringField(TEXT("source_instance_id")));
        TestTrue(TEXT("Copy uses exact source mesh and explicit override"), Copy->GetStaticMeshComponent()->GetStaticMesh() == Component->GetStaticMesh() && Copy->GetStaticMeshComponent()->GetEditorMaterial(0) == Assets.Material);
        TestEqual(TEXT("Only one actor is created"), Count(World), BeforeCount + 1);
        TestTrue(TEXT("Default or explicit absolute location matches"), Copy->GetActorLocation().Equals(bOffset ? FVector(400, 500, 600) : Source->GetActorLocation()));
        TestEqual(TEXT("Common settings copied exactly"), Encode(Details(Bridge, Copy)->GetObjectField(TEXT("mesh_settings"))), Encode(SourceSettings));
        TestEqual(TEXT("Raw disabled-actor collision mode survives copy"), Copy->GetStaticMeshComponent()->BodyInstance.GetCollisionEnabled(false), ECollisionEnabled::QueryAndPhysics);
        TestEqual(TEXT("Source remains entirely unchanged"), Encode(Details(Bridge, Source)), SourceBefore);
        TestTrue(TEXT("Copy participates in Undo"), GEditor->UndoTransaction());
        TestEqual(TEXT("Undo removes only the created actor"), Count(World), BeforeCount);
        TestEqual(TEXT("Undo keeps source unchanged"), Encode(Details(Bridge, Source)), SourceBefore);
    }
    Component->EmptyOverrideMaterials();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMeshGuardsTest, "Jev.Editor.MeshGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMeshGuardsTest::RunTest(const FString& Parameters)
{
    using namespace JevMeshTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    AStaticMeshActor* Source = Cube(World, TEXT("GuardedSource"));
    AStaticMeshActor* Other = Cube(World, TEXT("Other"));
    UStaticMeshComponent* Component = Source->GetStaticMeshComponent();
    UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    FAssets Assets;
    FJevEditorBridge Bridge;
    const auto Copy = Duplicate(Source);
    TestEqual(TEXT("A source cannot occur twice in a plan"), ErrorCode(Preview(Bridge, {Copy, Copy})), FString(TEXT("bad_request")));
    Other->AttachToActor(Source, FAttachmentTransformRules::KeepWorldTransform);
    TestEqual(TEXT("Attachment parent cannot be copied"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    TestEqual(TEXT("Attached child cannot be replaced"), ErrorCode(Preview(Bridge, {Replace(Other, Sphere)})), FString(TEXT("actor_unsupported")));
    Other->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    USceneComponent* Extra = NewObject<USceneComponent>(Source, TEXT("CustomExtra"));
    Source->AddInstanceComponent(Extra);
    Extra->RegisterComponent();
    TestEqual(TEXT("Extra components cannot be silently omitted"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    Extra->DestroyComponent();
    Source->RemoveInstanceComponent(Extra);
    Component->BodyInstance.bSimulatePhysics = true;
    TestEqual(TEXT("Simulation blocks mesh changes"), ErrorCode(Preview(Bridge, {Replace(Source, Sphere)})), FString(TEXT("actor_unsupported")));
    Component->BodyInstance.bSimulatePhysics = false;
    Component->SetRenderInMainPass(false);
    TestEqual(TEXT("Unsupported nondefault rendering is not silently lost"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    Component->SetRenderInMainPass(true);
    FBoolProperty* AngularOverride = FindFProperty<FBoolProperty>(FBodyInstance::StaticStruct(), TEXT("bOverrideMaxAngularVelocity"));
    if (!TestNotNull(TEXT("Native angular velocity override flag is available"), AngularOverride)) return false;
    AngularOverride->SetPropertyValue_InContainer(&Component->BodyInstance, true);
    TestEqual(TEXT("Active noncopied physics override is refused"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    AngularOverride->SetPropertyValue_InContainer(&Component->BodyInstance, false);
    const auto StaleInheritance = Preview(Bridge, {Copy});
    Component->SetCollisionProfileName(TEXT("BlockAll"));
    TestEqual(TEXT("Changing only collision inheritance invalidates plan"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleInheritance)), FString(TEXT("stale_plan")));
    Component->bUseDefaultCollision = true;
    Component->UpdateCollisionFromStaticMesh();
    Component->SetCollisionObjectType(ECC_Pawn);
    TestEqual(TEXT("Inconsistent mesh inheritance cannot be copied"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    Component->UpdateCollisionFromStaticMesh();
    Source->Tags.SetNum(33);
    TestEqual(TEXT("Tags are bounded before copy"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    Source->Tags.Reset();
    Source->SetPivotOffset(FVector(1, 0, 0));
    TestEqual(TEXT("Custom editor pivot is not silently reset by copying"), ErrorCode(Preview(Bridge, {Copy})), FString(TEXT("actor_unsupported")));
    Source->SetPivotOffset(FVector::ZeroVector);
    const auto StaleSettings = Preview(Bridge, {Copy});
    Component->SetCastShadow(false);
    TestEqual(TEXT("Changed copied settings invalidate plan"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleSettings)), FString(TEXT("stale_plan")));
    Component->SetCastShadow(true);
    const auto StaleUnsupported = Preview(Bridge, {Copy});
    Component->SetRenderCustomDepth(true);
    TestEqual(TEXT("New unsupported settings invalidate plan"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleUnsupported)), FString(TEXT("stale_plan")));
    Component->SetRenderCustomDepth(false);
    const auto StaleMaterial = Preview(Bridge, {Copy});
    Component->SetMaterial(0, Assets.Material);
    TestEqual(TEXT("Changed source material invalidates plan"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleMaterial)), FString(TEXT("stale_plan")));
    const auto StaleMaterialContent = Preview(Bridge, {Copy});
    Assets.Material->SetLightingGuid();
    TestEqual(TEXT("Changed material lighting identity invalidates plan"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleMaterialContent)), FString(TEXT("stale_plan")));
    const auto ReplaceOp = Replace(Source, Assets.Mesh);
    Assets.Mesh->GetBodySetup()->DefaultInstance.SetCollisionProfileName(TEXT("NoCollision"));
    TestEqual(TEXT("Different inherited asset collision must be reviewed as an explicit component setting first"), ErrorCode(Preview(Bridge, {ReplaceOp})), FString(TEXT("actor_unsupported")));
    Component->SetCollisionProfileName(TEXT("BlockAll"));
    TestTrue(TEXT("Explicit component collision permits different mesh defaults"), Preview(Bridge, {ReplaceOp})->GetBoolField(TEXT("ok")));
    Component->bUseDefaultCollision = true;
    Component->UpdateCollisionFromStaticMesh();
    Assets.Mesh->GetBodySetup()->DefaultInstance.SetCollisionProfileName(TEXT("BlockAll"));
    const auto StaleCollisionDefaults = Preview(Bridge, {ReplaceOp});
    Assets.Mesh->GetBodySetup()->DefaultInstance.SetCollisionProfileName(TEXT("NoCollision"));
    TestEqual(TEXT("Changed destination collision defaults invalidate plan without notifications"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleCollisionDefaults)), FString(TEXT("stale_plan")));
    Assets.Mesh->GetBodySetup()->DefaultInstance.SetCollisionProfileName(TEXT("BlockAll"));
    const auto StaleSlot = Preview(Bridge, {ReplaceOp});
    Assets.Mesh->GetStaticMaterials()[0].MaterialSlotName = TEXT("ChangedSlotName");
    TestEqual(TEXT("Changed destination slot metadata invalidates plan"), ErrorCode(ByPlan(Bridge, TEXT("apply"), StaleSlot)), FString(TEXT("stale_plan")));
    const auto AssetNotification = Preview(Bridge, {ReplaceOp});
    FPropertyChangedEvent PropertyChange(nullptr);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Assets.Mesh, PropertyChange);
    TestEqual(TEXT("Editor asset property events invalidate old plans"), ErrorCode(ByPlan(Bridge, TEXT("apply"), AssetNotification)), FString(TEXT("stale_plan")));
    const auto RemappedAsset = Preview(Bridge, {ReplaceOp});
    Assets.ReplaceMeshObject();
    TestEqual(TEXT("Same-path destination object replacement is rejected"), ErrorCode(ByPlan(Bridge, TEXT("apply"), RemappedAsset)), FString(TEXT("stale_plan")));
    const auto RemappedSource = Preview(Bridge, {Copy});
    Source->Rename(nullptr, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
    TestEqual(TEXT("Renamed source identity invalidates copy"), ErrorCode(ByPlan(Bridge, TEXT("apply"), RemappedSource)), FString(TEXT("stale_plan")));
    Component->EmptyOverrideMaterials();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMeshRollbackTest, "Jev.Editor.MeshRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMeshRollbackTest::RunTest(const FString& Parameters)
{
    using namespace JevMeshTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!World) return false;
    AStaticMeshActor* Replaced = Cube(World, TEXT("RollbackReplacement"));
    AStaticMeshActor* Source = Cube(World, TEXT("RollbackSource"));
    UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    FJevEditorBridge Bridge;
    {
        FScopedTransaction Earlier(NSLOCTEXT("JevMesh", "Earlier", "Earlier independent test edit"));
        Source->Modify();
        Source->GetRootComponent()->Modify();
        Source->SetActorLocation(FVector(30, 40, 50));
    }
    const FGuid EarlierId = GEditor->Trans->GetUndoContext().TransactionId;
    const FString Before = Revision(Bridge);
    const int32 BeforeCount = Count(World);
    const auto Mixed = Preview(Bridge, {Replace(Replaced, Sphere), Duplicate(Source)});
    if (!TestTrue(TEXT("Mixed mesh operations preview"), Mixed->GetBoolField(TEXT("ok")))) return false;
    Bridge.FailApplyAfterOperationsForTesting(2);
    TestEqual(TEXT("Mixed failure rolls back exact transaction"), ErrorCode(ByPlan(Bridge, TEXT("apply"), Mixed)), FString(TEXT("apply_failed")));
    TestEqual(TEXT("Rollback restores exact scene revision"), Revision(Bridge), Before);
    TestEqual(TEXT("Rollback removes controlled copy"), Count(World), BeforeCount);
    TestEqual(TEXT("Rollback keeps unrelated Undo entry"), GEditor->Trans->GetUndoContext().TransactionId, EarlierId);
    TestFalse(TEXT("Failed mesh plan cannot be redone"), GEditor->Trans->CanRedo());
    auto CallbackPlan = Preview(Bridge, {Duplicate(Source)});
    Bridge.OnMeshOperationForTesting([Source]
    {
        Source->GetStaticMeshComponent()->Modify();
        Source->GetStaticMeshComponent()->SetRenderCustomDepth(true);
    });
    TestEqual(TEXT("Callback cannot change unsupported source settings unnoticed"), ErrorCode(ByPlan(Bridge, TEXT("apply"), CallbackPlan)), FString(TEXT("apply_failed")));
    TestFalse(TEXT("Callback's transacted source mutation is restored"), Source->GetStaticMeshComponent()->bRenderCustomDepth);
    TestEqual(TEXT("Callback failure leaves exact original scene"), Revision(Bridge), Before);
    CallbackPlan = Preview(Bridge, {Replace(Replaced, Sphere)});
    Bridge.OnMeshOperationForTesting([Source]
    {
        Source->Modify();
        Source->SetActorLabel(TEXT("UnexpectedCallbackRename"));
    });
    TestEqual(TEXT("Mutation of an unrelated actor is detected and rolled back"), ErrorCode(ByPlan(Bridge, TEXT("apply"), CallbackPlan)), FString(TEXT("apply_failed")));
    TestEqual(TEXT("Source label restored after callback"), Source->GetActorLabel(), FString(TEXT("RollbackSource")));
    TestEqual(TEXT("Replacement also restored after callback"), Revision(Bridge), Before);
    CallbackPlan = Preview(Bridge, {Replace(Replaced, Sphere)});
    Bridge.OnMeshOperationForTesting([World, Replaced] { World->EditorDestroyActor(Replaced, true); });
    TestEqual(TEXT("Destroying the edited actor in a callback safely rolls back"), ErrorCode(ByPlan(Bridge, TEXT("apply"), CallbackPlan)), FString(TEXT("apply_failed")));
    TestTrue(TEXT("Undo revives the original actor identity"), IsValid(Replaced));
    TestEqual(TEXT("Actor destruction restores exact scene revision"), Revision(Bridge), Before);
    TestTrue(TEXT("Earlier independent Undo remains usable"), GEditor->UndoTransaction());
    TestTrue(TEXT("Earlier Undo restores only its own location"), Source->GetActorLocation().IsNearlyZero());

    UWorld* SwitchedWorld = UWorld::CreateWorld(EWorldType::Editor, false, FName(*(TEXT("JevMeshSwitch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if (!TestNotNull(TEXT("Isolated callback world created"), SwitchedWorld)) return false;
    ON_SCOPE_EXIT
    {
        GEditor->GetEditorWorldContext().SetCurrentWorld(World);
        SwitchedWorld->DestroyWorld(false);
        SwitchedWorld->RemoveFromRoot();
    };
    const FString BeforeSwitch = Revision(Bridge);
    const int32 BeforeSwitchCount = Count(World);
    const FGuid PreviousUndo = GEditor->Trans->GetUndoContext().TransactionId;
    const auto SwitchPlan = Preview(Bridge, {Duplicate(Source)});
    if (!TestTrue(TEXT("World-switch fixture previews"), SwitchPlan->GetBoolField(TEXT("ok")))) return false;
    Bridge.OnMeshOperationForTesting([SwitchedWorld] { GEditor->GetEditorWorldContext().SetCurrentWorld(SwitchedWorld); });
    const auto SwitchResult = ByPlan(Bridge, TEXT("apply"), SwitchPlan);
    // Restore immediately on this same game-thread turn, before issuing more calls.
    GEditor->GetEditorWorldContext().SetCurrentWorld(World);
    TestEqual(TEXT("World-switch callback produces explicit unknown rollback outcome"), ErrorCode(SwitchResult), FString(TEXT("rollback_failed")));
    TestEqual(TEXT("Changed-world guard did not undo a different context"), Count(World), BeforeSwitchCount + 1);
    TestEqual(TEXT("Unproven rollback stays unknown in native history"), ByPlan(Bridge, TEXT("plan_status"), SwitchPlan)->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("unknown")));
    const FGuid RetainedTransaction = GEditor->Trans->GetUndoContext().TransactionId;
    TestTrue(TEXT("Fixture retains its own independent Undo identity"), RetainedTransaction.IsValid() && RetainedTransaction != PreviousUndo);
    TestTrue(TEXT("Fixture explicitly cleans its retained transaction after restoring context"), GEditor->UndoTransaction(false));
    TestEqual(TEXT("Explicit fixture cleanup restores original scene"), Revision(Bridge), BeforeSwitch);
    return true;
}

#endif
