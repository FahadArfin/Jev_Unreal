#include "JevEditorBridge.h"

#include "ActorEditorUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "LevelUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/StrongObjectPtr.h"

namespace JevEdits
{
constexpr int32 MaxOperations = 20;
constexpr double PlanLifetime = 120.0;

bool OnlyFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Fields)
{
    for (const auto& Pair : Object->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& Out)
{
    return Object->HasTypedField<EJson::String>(Key) && Object->TryGetStringField(Key, Out);
}

bool Vector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FVector& Out, double Bound, bool bScale = false)
{
    if (!Object->HasField(Key)) return true;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object->TryGetArrayField(Key, Values) || Values->Num() != 3) return false;
    for (int32 I = 0; I < 3; ++I)
    {
        double Value = 0;
        if (!(*Values)[I].IsValid() || (*Values)[I]->Type != EJson::Number || !(*Values)[I]->TryGetNumber(Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > Bound || (bScale && Value < 0.001)) return false;
        Out[I] = Value;
    }
    return true;
}

TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

bool Label(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
    if (!String(Object, TEXT("label"), Out)) return false;
    Out.TrimStartAndEndInline();
    if (Out.IsEmpty() || Out.Len() > 80) return false;
    for (TCHAR Character : Out) if (Character < 32 || Character == 127) return false;
    FText Reason;
    return FActorEditorUtils::ValidateActorName(FText::FromString(Out), Reason);
}

bool Folder(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
    if (!String(Object, TEXT("folder"), Out) || Out.Len() > 256 || Out != Out.TrimStartAndEnd() || Out.Contains(TEXT("\\")) || Out.Contains(TEXT(":"))) return false;
    if (Out.IsEmpty()) return true;
    if (FName(*Out).IsNone() || Out.StartsWith(TEXT("/")) || Out.EndsWith(TEXT("/"))) return false;
    for (TCHAR Character : Out) if (Character < 32 || Character == 127) return false;
    TArray<FString> Parts;
    Out.ParseIntoArray(Parts, TEXT("/"), false);
    for (const FString& Part : Parts) if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part != Part.TrimStartAndEnd()) return false;
    return true;
}

FString MeshPath(const FString& Shape)
{
    if (Shape != TEXT("Cube") && Shape != TEXT("Sphere") && Shape != TEXT("Cylinder") && Shape != TEXT("Plane")) return {};
    return FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), *Shape, *Shape);
}

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), true);
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}
}

TSharedRef<FJsonObject> FJevEditorBridge::Preview(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    if (!World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel()))
        return Error(TEXT("level_locked"), TEXT("The current editor level must be editable."));
    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    if (!JevEdits::OnlyFields(Params, {TEXT("operations"), TEXT("expected_state")}) || !Params->TryGetArrayField(TEXT("operations"), Operations) || Operations->IsEmpty() || Operations->Num() > JevEdits::MaxOperations)
        return Error(TEXT("bad_request"), TEXT("preview accepts operations (1 to 20) and optional expected_state."));
    const FString InitialRevision = Revision(World);
    if (Params->HasField(TEXT("expected_state")))
    {
        const TSharedPtr<FJsonObject>* State = nullptr;
        FString ExpectedSession, ExpectedWorld, ExpectedRevision;
        if (!Params->TryGetObjectField(TEXT("expected_state"), State) || !JevEdits::OnlyFields(*State, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) ||
            !JevEdits::String(*State, TEXT("session_id"), ExpectedSession) || ExpectedSession.IsEmpty() || ExpectedSession.Len() > 128 ||
            !JevEdits::String(*State, TEXT("world_path"), ExpectedWorld) || ExpectedWorld.IsEmpty() || ExpectedWorld.Len() > 1024 ||
            !JevEdits::String(*State, TEXT("revision"), ExpectedRevision) || ExpectedRevision.IsEmpty() || ExpectedRevision.Len() > 128)
            return Error(TEXT("bad_request"), TEXT("expected_state requires exactly nonempty session_id, world_path, and revision strings."));
        if (ExpectedSession != SessionId || ExpectedWorld != World->GetPathName() || ExpectedRevision != InitialRevision)
            return Error(TEXT("stale_plan"), TEXT("Editor state changed since inspection. Inspect again before requesting a preview."));
    }
    const double Now = Clock();
    for (auto It = Plans.CreateIterator(); It; ++It) if (It.Value().ExpiresAt <= Now) It.RemoveCurrent();
    if (Plans.Num() >= 64) return Error(TEXT("too_many_plans"), TEXT("At most 64 unexpired plans can be held."));
    FPlan Plan;
    Plan.Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    Plan.World = World->GetPathName();
    Plan.WorldInstance = World;
    for (TActorIterator<AActor> It(World); It; ++It) Plan.SceneActors.Add(*It);
    Plan.Revision = InitialRevision;
    Plan.ExpiresAt = Now + JevEdits::PlanLifetime;
    TArray<TSharedPtr<FJsonValue>> Normalized;
    TSet<FString> ExistingTargets;
    for (const auto& Value : *Operations)
    {
        const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr)) return Error(TEXT("bad_request"), TEXT("Each operation must be an object."));
        const auto Object = *ObjectPtr;
        FOperation Operation;
        if (!JevEdits::String(Object, TEXT("op"), Operation.Op)) return Error(TEXT("bad_request"), TEXT("Each operation requires op."));
        FVector Location = FVector::ZeroVector, Rotation = FVector::ZeroVector, Scale = FVector::OneVector;
        if (Operation.Op == TEXT("spawn_primitive") || Operation.Op == TEXT("spawn_static_mesh"))
        {
            const bool bPrimitive = Operation.Op == TEXT("spawn_primitive");
            const TArray<FString> Fields = bPrimitive ? TArray<FString>{TEXT("op"), TEXT("shape"), TEXT("label"), TEXT("location"), TEXT("rotation"), TEXT("scale")} : TArray<FString>{TEXT("op"), TEXT("asset_path"), TEXT("label"), TEXT("location"), TEXT("rotation"), TEXT("scale")};
            if (!JevEdits::OnlyFields(Object, Fields) || !JevEdits::Label(Object, Operation.Label)) return Error(TEXT("bad_request"), TEXT("Spawns require an allowed asset selector and a valid actor label (1 to 80 characters)."));
            if (bPrimitive)
            {
                if (!JevEdits::String(Object, TEXT("shape"), Operation.Shape) || JevEdits::MeshPath(Operation.Shape).IsEmpty()) return Error(TEXT("bad_request"), TEXT("Unknown primitive shape."));
            }
            else
            {
                if (!JevEdits::String(Object, TEXT("asset_path"), Operation.AssetPath)) return Error(TEXT("bad_request"), TEXT("asset_path must be an exact static mesh object path."));
                UStaticMesh* Mesh = nullptr;
                if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Mesh)) return Failure.ToSharedRef();
                Operation.MeshAsset = Mesh;
            }
        }
        else if (Operation.Op == TEXT("set_transform") || Operation.Op == TEXT("set_material") || Operation.Op == TEXT("set_metadata"))
        {
            if (!JevEdits::String(Object, TEXT("actor_path"), Operation.ActorPath) || Operation.ActorPath.IsEmpty() || Operation.ActorPath.Len() > 1024) return Error(TEXT("bad_request"), TEXT("Existing-actor edits require an exact actor_path of 1 to 1024 characters."));
            if (ExistingTargets.Contains(Operation.ActorPath)) return Error(TEXT("bad_request"), TEXT("A plan may contain only one operation per existing actor, across all edit types."));
            AActor* Actor = FindActor(World, Operation.ActorPath);
            if (!IsValid(Actor)) return Error(TEXT("actor_not_found"), TEXT("The edit target must exist in the editor world."));
            const TArray<FString> Blockers = ActorEditBlockers(Actor);
            if (!Blockers.IsEmpty()) return Error(Blockers.Contains(TEXT("actor_locked")) || Blockers.Contains(TEXT("level_locked")) ? TEXT("actor_locked") : TEXT("actor_unsupported"), TEXT("Edits require an editable, unlocked, exact native StaticMeshActor without parent, child, or child-actor attachments."));
            if (Actor->GetActorTransform().ContainsNaN()) return Error(TEXT("actor_unsupported"), TEXT("The target has a non-finite baseline transform."));
            ExistingTargets.Add(Operation.ActorPath);
            Operation.Target = Actor;
            Operation.TargetBaseline = ActorEditFingerprint(Actor);
            Location = Actor->GetActorLocation();
            const FRotator BaselineRotation = Actor->GetActorRotation();
            Rotation = FVector(BaselineRotation.Pitch, BaselineRotation.Yaw, BaselineRotation.Roll);
            Scale = Actor->GetActorScale3D();
            if (Operation.Op == TEXT("set_transform"))
            {
                if (!JevEdits::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("location"), TEXT("rotation"), TEXT("scale")}) || (!Object->HasField(TEXT("location")) && !Object->HasField(TEXT("rotation")) && !Object->HasField(TEXT("scale")))) return Error(TEXT("bad_request"), TEXT("set_transform requires at least one transform field."));
            }
            else if (Operation.Op == TEXT("set_material"))
            {
                double Slot = 0;
                if (!JevEdits::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("material_path"), TEXT("slot")}) || !JevEdits::String(Object, TEXT("material_path"), Operation.MaterialPath) || !Object->HasTypedField<EJson::Number>(TEXT("slot")) || !Object->TryGetNumberField(TEXT("slot"), Slot) || !FMath::IsFinite(Slot) || Slot < 0 || Slot > 63 || FMath::FloorToDouble(Slot) != Slot) return Error(TEXT("bad_request"), TEXT("set_material requires exact material_path and integer slot from 0 to 63."));
                Operation.Slot = static_cast<int32>(Slot);
                const auto* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                if (Operation.Slot >= Component->GetNumMaterials()) return Error(TEXT("material_slot_invalid"), TEXT("The selected slot does not exist on this actor's static mesh."));
                UMaterialInterface* Material = nullptr;
                if (const auto Failure = ResolveMaterialAsset(Operation.MaterialPath, Material)) return Failure.ToSharedRef();
                Operation.MaterialAsset = Material;
            }
            else
            {
                if (!JevEdits::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("label"), TEXT("folder")}) || (!Object->HasField(TEXT("label")) && !Object->HasField(TEXT("folder")))) return Error(TEXT("bad_request"), TEXT("set_metadata requires label and/or folder."));
                Operation.bSetLabel = Object->HasField(TEXT("label"));
                Operation.bSetFolder = Object->HasField(TEXT("folder"));
                if (Operation.bSetLabel && (!Actor->IsActorLabelEditable() || !JevEdits::Label(Object, Operation.Label))) return Error(TEXT("bad_request"), TEXT("The label must be editable and contain 1 to 80 valid characters."));
                if (Operation.bSetFolder && !JevEdits::Folder(Object, Operation.Folder)) return Error(TEXT("bad_request"), TEXT("folder must be empty for root, or a relative slash-separated path of at most 256 characters without empty, dot, parent, or control segments."));
            }
        }
        else return Error(TEXT("bad_request"), TEXT("Unsupported editor operation."));
        if (!JevEdits::Vector(Object, TEXT("location"), Location, 1000000.0) || !JevEdits::Vector(Object, TEXT("rotation"), Rotation, 36000.0) || !JevEdits::Vector(Object, TEXT("scale"), Scale, 1000.0, true)) return Error(TEXT("bad_request"), TEXT("Transform fields require three finite numbers within the documented editor bounds."));
        Operation.Transform = FTransform(FRotator(Rotation.X, Rotation.Y, Rotation.Z), Location, Scale);
        auto Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("op"), Operation.Op);
        if (Operation.Op == TEXT("spawn_primitive")) { Summary->SetStringField(TEXT("shape"), Operation.Shape); Summary->SetStringField(TEXT("label"), Operation.Label); }
        else if (Operation.Op == TEXT("spawn_static_mesh")) { Summary->SetStringField(TEXT("asset_path"), Operation.AssetPath); Summary->SetStringField(TEXT("label"), Operation.Label); }
        else
        {
            Summary->SetStringField(TEXT("actor_path"), Operation.ActorPath);
            if (Operation.Op == TEXT("set_material")) { Summary->SetStringField(TEXT("material_path"), Operation.MaterialPath); Summary->SetNumberField(TEXT("slot"), Operation.Slot); }
            if (Operation.bSetLabel) Summary->SetStringField(TEXT("label"), Operation.Label);
            if (Operation.bSetFolder) Summary->SetStringField(TEXT("folder"), Operation.Folder);
        }
        Summary->SetArrayField(TEXT("location"), JevEdits::Vector(Location));
        Summary->SetArrayField(TEXT("rotation"), JevEdits::Vector(Rotation));
        Summary->SetArrayField(TEXT("scale"), JevEdits::Vector(Scale));
        Normalized.Add(MakeShared<FJsonValueObject>(Summary));
        Plan.Operations.Add(MoveTemp(Operation));
    }
    if (Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor state changed while resolving the preview assets. Inspect again."));
    const FString PlanId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Plans.Add(PlanId, MoveTemp(Plan));
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("plan_id"), PlanId);
    Result->SetNumberField(TEXT("expires_in_seconds"), JevEdits::PlanLifetime);
    Result->SetStringField(TEXT("revision"), InitialRevision);
    Result->SetArrayField(TEXT("operations"), Normalized);
    return JevEdits::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Apply(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    FString PlanId;
    if (!JevEdits::OnlyFields(Params, {TEXT("plan_id")}) || !JevEdits::String(Params, TEXT("plan_id"), PlanId) || PlanId.Len() > 64) return Error(TEXT("bad_request"), TEXT("apply requires only plan_id."));
    FPlan Plan;
    if (!Plans.RemoveAndCopyValue(PlanId, Plan)) return Error(TEXT("unknown_plan"), TEXT("Plan does not exist or has already been consumed."));
    if (Plan.ExpiresAt <= Clock()) return Error(TEXT("expired_plan"), TEXT("Plan expired. Preview again."));
    if (Plan.Project != FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()) || Plan.WorldInstance.Get() != World || Plan.World != World->GetPathName() || Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor state changed after preview."));
    for (auto Actor : Plan.SceneActors) if (!Actor.IsValid() || Actor->GetWorld() != World) return Error(TEXT("stale_plan"), TEXT("An actor instance was replaced after preview."));
    if (!World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel())) return Error(TEXT("level_locked"), TEXT("The current editor level must be editable."));
    if (!GEditor->CanTransact() || GEditor->IsTransactionActive() || GIsTransacting) return Error(TEXT("editor_busy"), TEXT("An independent editor Undo transaction must be available before applying a plan."));

    TMap<FString, TStrongObjectPtr<UStaticMesh>> Meshes;
    TMap<FString, TStrongObjectPtr<UMaterialInterface>> Materials;
    TMap<FString, TWeakObjectPtr<AActor>> Targets;
    for (const FOperation& Operation : Plan.Operations)
    {
        if (Operation.Op == TEXT("spawn_primitive"))
        {
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *JevEdits::MeshPath(Operation.Shape));
            if (!IsValid(Mesh)) return Error(TEXT("asset_unavailable"), TEXT("An engine primitive mesh could not be loaded."));
            Meshes.Add(Operation.Shape, TStrongObjectPtr<UStaticMesh>(Mesh));
        }
        else if (Operation.Op == TEXT("spawn_static_mesh"))
        {
            if (!Operation.MeshAsset.IsValid()) return Error(TEXT("stale_plan"), TEXT("The selected mesh is no longer the reviewed live object."));
            UStaticMesh* Mesh = nullptr;
            if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Mesh)) return Failure.ToSharedRef();
            if (Operation.MeshAsset.Get() != Mesh) return Error(TEXT("stale_plan"), TEXT("The mesh asset was replaced after preview."));
            Meshes.Add(Operation.AssetPath, TStrongObjectPtr<UStaticMesh>(Mesh));
        }
        else
        {
            AActor* Actor = FindActor(World, Operation.ActorPath);
            if (!IsValid(Actor) || Operation.Target.Get() != Actor || !ActorEditBlockers(Actor).IsEmpty() || Operation.TargetBaseline != ActorEditFingerprint(Actor)) return Error(TEXT("stale_plan"), TEXT("A target's editability, material assignments, metadata, or object identity changed after preview."));
            Targets.Add(Operation.ActorPath, Actor);
            if (Operation.Op == TEXT("set_material"))
            {
                if (!Operation.MaterialAsset.IsValid()) return Error(TEXT("stale_plan"), TEXT("The selected material is no longer the reviewed live object."));
                const auto* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                if (Operation.Slot < 0 || Operation.Slot >= Component->GetNumMaterials()) return Error(TEXT("stale_plan"), TEXT("The reviewed material slot no longer exists."));
                UMaterialInterface* Material = nullptr;
                if (const auto Failure = ResolveMaterialAsset(Operation.MaterialPath, Material)) return Failure.ToSharedRef();
                if (Operation.MaterialAsset.Get() != Material) return Error(TEXT("stale_plan"), TEXT("The material asset was replaced after preview."));
                Materials.Add(Operation.MaterialPath, TStrongObjectPtr<UMaterialInterface>(Material));
            }
            if (Operation.bSetLabel && !Actor->IsActorLabelEditable()) return Error(TEXT("stale_plan"), TEXT("The actor label is no longer editable."));
        }
    }
    if (Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor state changed while resolving the plan."));
    if (!GEditor->CanTransact() || GEditor->IsTransactionActive() || GIsTransacting) return Error(TEXT("editor_busy"), TEXT("The editor became busy while resolving the plan."));
    TArray<TWeakObjectPtr<AActor>> ChangedActors;
    bool bFailed = false;
    FGuid ApplyTransactionId;
    {
        FScopedTransaction Transaction(NSLOCTEXT("JevEditor", "ApplyPlan", "Apply Jev scene plan"));
        if (!Transaction.IsOutstanding()) return Error(TEXT("editor_busy"), TEXT("An editor Undo transaction could not be started."));
        ApplyTransactionId = GEditor->Trans->GetUndoContext(false).TransactionId;
        if (!ApplyTransactionId.IsValid())
        {
            Transaction.Cancel();
            return Error(TEXT("editor_busy"), TEXT("The editor transaction identity could not be established before editing."));
        }
        World->GetCurrentLevel()->Modify();
        for (const FOperation& Operation : Plan.Operations)
        {
            AActor* Actor = nullptr;
            if (Operation.Op == TEXT("spawn_primitive") || Operation.Op == TEXT("spawn_static_mesh"))
            {
                const FString& Key = Operation.Op == TEXT("spawn_primitive") ? Operation.Shape : Operation.AssetPath;
                UStaticMesh* Mesh = Meshes[Key].Get();
                if (!IsValid(Mesh)) { bFailed = true; break; }
                FActorSpawnParameters Spawn;
                Spawn.OverrideLevel = World->GetCurrentLevel();
                Spawn.ObjectFlags |= RF_Transactional;
                Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                auto* StaticActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Operation.Transform, Spawn);
                if (!StaticActor) { bFailed = true; break; }
                Actor = StaticActor;
                Actor->Modify();
                StaticActor->GetStaticMeshComponent()->Modify();
                if (!StaticActor->GetStaticMeshComponent()->SetStaticMesh(Mesh)) { bFailed = true; break; }
                Actor->SetActorLabel(Operation.Label);
                if (!IsValid(Actor) || Actor->GetActorLabel() != Operation.Label) { bFailed = true; break; }
                Actor->PostEditMove(true);
            }
            else
            {
                Actor = Targets[Operation.ActorPath].Get();
                if (!IsValid(Actor) || !ActorEditBlockers(Actor).IsEmpty()) { bFailed = true; break; }
                Actor->Modify();
                Actor->GetRootComponent()->Modify();
                if (Operation.Op == TEXT("set_transform"))
                {
                    if (!Actor->SetActorTransform(Operation.Transform, false, nullptr, ETeleportType::TeleportPhysics)) { bFailed = true; break; }
                    Actor->PostEditMove(true);
                    if (!IsValid(Actor) || !Actor->GetActorTransform().Equals(Operation.Transform, 0.001)) { bFailed = true; break; }
                }
                else if (Operation.Op == TEXT("set_material"))
                {
                    UMaterialInterface* Material = Materials[Operation.MaterialPath].Get();
                    if (!IsValid(Material)) { bFailed = true; break; }
                    UStaticMeshComponent* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                    Component->Modify();
                    Component->SetMaterial(Operation.Slot, Material);
                    if (!IsValid(Actor) || Component->GetEditorMaterial(Operation.Slot) != Material) { bFailed = true; break; }
                }
                else
                {
                    if (Operation.bSetLabel) Actor->SetActorLabel(Operation.Label);
                    if (!IsValid(Actor)) { bFailed = true; break; }
                    if (Operation.bSetFolder) Actor->SetFolderPath(Operation.Folder.IsEmpty() ? NAME_None : FName(*Operation.Folder));
                    if (!IsValid(Actor) || (Operation.bSetLabel && Actor->GetActorLabel() != Operation.Label) || (Operation.bSetFolder && (Actor->GetFolderPath().IsNone() ? FString() : Actor->GetFolderPath().ToString()) != Operation.Folder)) { bFailed = true; break; }
                }
            }
            if (!IsValid(Actor)) { bFailed = true; break; }
            Actor->MarkPackageDirty();
            ChangedActors.Add(Actor);
#if WITH_DEV_AUTOMATION_TESTS
            if (FailureAfterOperationsForTesting > 0 && ChangedActors.Num() == FailureAfterOperationsForTesting)
            {
                FailureAfterOperationsForTesting = INDEX_NONE;
                bFailed = true;
                break;
            }
#endif
        }
    }
    if (bFailed)
    {
        // Finalization discards no-op transactions and restores the earlier Undo/Redo
        // stack. Never undo whatever happens to be on top: only undo our exact ID.
        // false discards this failed transaction's redo record after restoration.
        bool bRestored = false;
        if (GEditor->Trans && !GEditor->IsTransactionActive() && !GIsTransacting &&
            GEditor->Trans->GetUndoContext(false).TransactionId == ApplyTransactionId)
            bRestored = GEditor->UndoTransaction(false);
        else
            bRestored = Plan.Revision == Revision(World);
        GEditor->RedrawLevelEditingViewports(true);
        if (!bRestored || Plan.Revision != Revision(World)) return Error(TEXT("rollback_failed"), TEXT("An editor operation failed and complete rollback could not be verified. Inspect the current scene before further edits."));
        return Error(TEXT("apply_failed"), TEXT("An editor operation failed. The complete transaction was rolled back and the restored scene revision was verified."));
    }
    GEditor->RedrawLevelEditingViewports(true);
    TArray<TSharedPtr<FJsonValue>> Changed;
    for (auto Actor : ChangedActors) if (Actor.IsValid()) Changed.Add(MakeShared<FJsonValueObject>(ActorSnapshot(Actor.Get())));
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("applied"), true);
    Result->SetArrayField(TEXT("actors"), Changed);
    Result->SetStringField(TEXT("revision"), Revision(World));
    return JevEdits::Success(Result);
}
