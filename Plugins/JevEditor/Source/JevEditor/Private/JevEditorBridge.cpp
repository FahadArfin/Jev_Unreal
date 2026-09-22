#include "JevEditorBridge.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "LevelUtils.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"

namespace Jev
{
constexpr int32 MaxOperations = 20;
constexpr int32 MaxResults = 200;
constexpr double PlanLifetime = 120.0;

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), true);
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
    return { MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z) };
}

bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out)
{
    return Object->HasTypedField<EJson::String>(Field) && Object->TryGetStringField(Field, Out);
}

bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out, double Bound, bool bScale = false)
{
    if (!Object->HasField(Field)) return true;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3) return false;
    for (int32 I = 0; I < 3; ++I)
    {
        double Value = 0;
        if (!(*Values)[I].IsValid() || (*Values)[I]->Type != EJson::Number || !(*Values)[I]->TryGetNumber(Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > Bound || (bScale && Value < 0.001)) return false;
        Out[I] = Value;
    }
    return true;
}

bool OnlyFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Fields)
{
    for (const auto& Pair : Object->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

FString MeshPath(const FString& Shape)
{
    if (Shape != TEXT("Cube") && Shape != TEXT("Sphere") && Shape != TEXT("Cylinder") && Shape != TEXT("Plane")) return FString();
    return FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), *Shape, *Shape);
}

FString ProjectPath()
{
    return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
}

bool IsSafeTransformTarget(AActor* Actor)
{
    if (!IsValid(Actor) || Actor->GetClass() != AStaticMeshActor::StaticClass() || Actor->GetAttachParentActor() || Actor->GetParentActor()) return false;
    TArray<AActor*> AttachedActors;
    Actor->GetAttachedActors(AttachedActors);
    return AttachedActors.IsEmpty();
}
}

FJevEditorBridge::FJevEditorBridge(TFunction<double()> InClock)
    : SessionId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens)), Clock(MoveTemp(InClock))
{
    if (!Clock) Clock = [] { return FPlatformTime::Seconds(); };
}

TSharedRef<FJsonObject> FJevEditorBridge::Error(const FString& Code, const FString& Message)
{
    auto Detail = MakeShared<FJsonObject>();
    Detail->SetStringField(TEXT("code"), Code);
    Detail->SetStringField(TEXT("message"), Message);
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), false);
    Response->SetObjectField(TEXT("error"), Detail);
    return Response;
}

TSharedRef<FJsonObject> FJevEditorBridge::ActorSnapshot(AActor* Actor) const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Actor->GetPathName());
    Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    Result->SetArrayField(TEXT("location"), Jev::Vector(Actor->GetActorLocation()));
    const FRotator Rotation = Actor->GetActorRotation();
    Result->SetArrayField(TEXT("rotation"), Jev::Vector(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
    Result->SetArrayField(TEXT("scale"), Jev::Vector(Actor->GetActorScale3D()));
    return Result;
}

FString FJevEditorBridge::Revision(UWorld* World) const
{
    TArray<FString> Entries;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        const FTransform Transform = Actor->GetActorTransform();
        const FVector Location = Transform.GetLocation(), Scale = Transform.GetScale3D();
        const FQuat Rotation = Transform.GetRotation();
        Entries.Add(FString::Printf(TEXT("%s|%s|%s|%.17g,%.17g,%.17g|%.17g,%.17g,%.17g,%.17g|%.17g,%.17g,%.17g|%s|%s|%d|%d|%d|%d"),
            *Actor->GetPathName(), *Actor->GetActorLabel(), *Actor->GetClass()->GetPathName(),
            Location.X, Location.Y, Location.Z, Rotation.X, Rotation.Y, Rotation.Z, Rotation.W, Scale.X, Scale.Y, Scale.Z,
            *GetPathNameSafe(Actor->GetAttachParentActor()), *GetPathNameSafe(Actor->GetRootComponent()),
            Actor->IsHidden(), Actor->IsTemporarilyHiddenInEditor(), Actor->IsLockLocation(), Actor->GetUniqueID()));
    }
    Entries.Sort();
    const FString State = SessionId + TEXT("|") + Jev::ProjectPath() + TEXT("|") + World->GetPathName() + TEXT("|") + GetPathNameSafe(World->GetCurrentLevel()) + TEXT("|") + FString::Join(Entries, TEXT("\n"));
    const FTCHARToUTF8 Bytes(*State);
    return FMD5::HashBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
}

AActor* FJevEditorBridge::FindActor(UWorld* World, const FString& Path) const
{
    for (TActorIterator<AActor> It(World); It; ++It)
        if (It->GetPathName() == Path) return *It;
    return nullptr;
}

TSharedRef<FJsonObject> FJevEditorBridge::Execute(const TSharedPtr<FJsonObject>& Request)
{
    check(IsInGameThread());
    if (!Request.IsValid() || !Jev::OnlyFields(Request, { TEXT("action"), TEXT("params") }))
        return Error(TEXT("bad_request"), TEXT("Expected only action and params fields."));
    FString Action;
    const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
    if (!Jev::ReadString(Request, TEXT("action"), Action) || !Request->TryGetObjectField(TEXT("params"), ParamsPtr))
        return Error(TEXT("bad_request"), TEXT("action must be a string and params must be an object."));
    const auto Params = *ParamsPtr;
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
        return Error(TEXT("editor_unavailable"), TEXT("An editor world is required."));
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
        return Error(TEXT("play_mode"), TEXT("Stop Play or Simulate before using the bridge."));

    if (Action == TEXT("status"))
    {
        if (!Params->Values.IsEmpty()) return Error(TEXT("bad_request"), TEXT("status has no parameters."));
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Result->SetStringField(TEXT("project_file"), Jev::ProjectPath());
        Result->SetStringField(TEXT("session_id"), SessionId);
        Result->SetStringField(TEXT("world_path"), World->GetPathName());
        Result->SetStringField(TEXT("revision"), Revision(World));
        Result->SetStringField(TEXT("bridge_version"), TEXT("0.1.0"));
        TArray<TSharedPtr<FJsonValue>> Capabilities;
        for (const TCHAR* Capability : { TEXT("status"), TEXT("actors"), TEXT("assets"), TEXT("preview"), TEXT("apply") })
            Capabilities.Add(MakeShared<FJsonValueString>(Capability));
        Result->SetArrayField(TEXT("capabilities"), Capabilities);
        return Jev::Success(Result);
    }
    if (Action == TEXT("preview")) return Preview(World, Params);
    if (Action == TEXT("apply")) return Apply(World, Params);
    if (Action != TEXT("actors") && Action != TEXT("assets"))
        return Error(TEXT("unknown_action"), TEXT("Supported actions: status, actors, assets, preview, apply."));

    if (!Jev::OnlyFields(Params, Action == TEXT("assets") ? TArray<FString>{TEXT("limit"), TEXT("query"), TEXT("path")} : TArray<FString>{TEXT("limit"), TEXT("query")}))
        return Error(TEXT("bad_request"), TEXT("Unexpected inspection parameter."));
    int32 Limit = 100;
    double LimitNumber = 100;
    if (Params->HasField(TEXT("limit")) && (!Params->HasTypedField<EJson::Number>(TEXT("limit")) || !Params->TryGetNumberField(TEXT("limit"), LimitNumber) || !FMath::IsFinite(LimitNumber) || LimitNumber < 1 || LimitNumber > Jev::MaxResults || FMath::FloorToDouble(LimitNumber) != LimitNumber))
        return Error(TEXT("bad_request"), TEXT("limit must be an integer from 1 to 200."));
    Limit = static_cast<int32>(LimitNumber);
    FString Query;
    if (Params->HasField(TEXT("query")) && (!Jev::ReadString(Params, TEXT("query"), Query) || Query.Len() > 200))
        return Error(TEXT("bad_request"), TEXT("query must be a string of at most 200 characters."));
    auto Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Items;
    bool bTruncated = false;
    if (Action == TEXT("actors"))
    {
        TArray<AActor*> Actors;
        for (TActorIterator<AActor> It(World); It; ++It)
            if (Query.IsEmpty() || It->GetActorLabel().Contains(Query) || It->GetPathName().Contains(Query)) Actors.Add(*It);
        Actors.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });
        bTruncated = Actors.Num() > Limit;
        for (int32 I = 0; I < FMath::Min(Limit, Actors.Num()); ++I) Items.Add(MakeShared<FJsonValueObject>(ActorSnapshot(Actors[I])));
        Result->SetArrayField(TEXT("actors"), Items);
        Result->SetStringField(TEXT("revision"), Revision(World));
    }
    else
    {
        FString Path = TEXT("/Game");
        if (Params->HasField(TEXT("path")) && (!Jev::ReadString(Params, TEXT("path"), Path) || (Path != TEXT("/Game") && !Path.StartsWith(TEXT("/Game/"))) || Path.Contains(TEXT("..")) || Path.Len() > 200))
            return Error(TEXT("bad_request"), TEXT("path must be a /Game content path without '..'."));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Path));
        Filter.bRecursivePaths = true;
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Assets;
        Registry.GetAssets(Filter, Assets);
        Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
        for (const auto& Asset : Assets)
        {
            const FString AssetPath = Asset.GetSoftObjectPath().ToString();
            if (!Query.IsEmpty() && !AssetPath.Contains(Query)) continue;
            if (Items.Num() == Limit) { bTruncated = true; break; }
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("path"), AssetPath);
            Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
            Item->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
            Items.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("assets"), Items);
        Result->SetBoolField(TEXT("registry_loading"), Registry.IsLoadingAssets());
    }
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    return Jev::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Preview(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    if (!World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel()))
        return Error(TEXT("level_locked"), TEXT("The current editor level must be editable."));
    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    if (!Jev::OnlyFields(Params, {TEXT("operations")}) || !Params->TryGetArrayField(TEXT("operations"), Operations) || Operations->IsEmpty() || Operations->Num() > Jev::MaxOperations)
        return Error(TEXT("bad_request"), TEXT("operations must contain 1 to 20 operations."));
    const double Now = Clock();
    for (auto It = Plans.CreateIterator(); It; ++It) if (It.Value().ExpiresAt <= Now) It.RemoveCurrent();
    if (Plans.Num() >= 64) return Error(TEXT("too_many_plans"), TEXT("At most 64 unexpired plans can be held. Apply a plan or wait 120 seconds."));
    FPlan Plan;
    Plan.Project = Jev::ProjectPath();
    Plan.World = World->GetPathName();
    Plan.WorldInstance = World;
    for (TActorIterator<AActor> It(World); It; ++It) Plan.SceneActors.Add(*It);
    Plan.Revision = Revision(World);
    Plan.ExpiresAt = Now + Jev::PlanLifetime;
    TArray<TSharedPtr<FJsonValue>> Normalized;
    TSet<FString> TransformTargets;
    for (const auto& Value : *Operations)
    {
        const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
        if (!Value->TryGetObject(ObjectPtr)) return Error(TEXT("bad_request"), TEXT("Each operation must be an object."));
        const auto Object = *ObjectPtr;
        FOperation Operation;
        if (!Jev::ReadString(Object, TEXT("op"), Operation.Op)) return Error(TEXT("bad_request"), TEXT("Each operation requires op."));
        FVector Location = FVector::ZeroVector, Rotation = FVector::ZeroVector, Scale = FVector::OneVector;
        if (Operation.Op == TEXT("spawn_primitive"))
        {
            if (!Jev::OnlyFields(Object, {TEXT("op"), TEXT("shape"), TEXT("label"), TEXT("location"), TEXT("rotation"), TEXT("scale")}) ||
                !Jev::ReadString(Object, TEXT("shape"), Operation.Shape) || Jev::MeshPath(Operation.Shape).IsEmpty() ||
                !Jev::ReadString(Object, TEXT("label"), Operation.Label) || Operation.Label.TrimStartAndEnd().IsEmpty() || Operation.Label.Len() > 80)
                return Error(TEXT("bad_request"), TEXT("spawn_primitive requires allowed shape and label (1 to 80 characters)."));
            for (TCHAR Character : Operation.Label) if (Character < 32 || Character == 127) return Error(TEXT("bad_request"), TEXT("Actor labels must not contain control characters."));
        }
        else if (Operation.Op == TEXT("set_transform"))
        {
            if (!Jev::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("location"), TEXT("rotation"), TEXT("scale")}) ||
                !Jev::ReadString(Object, TEXT("actor_path"), Operation.ActorPath) || Operation.ActorPath.Len() > 1024 ||
                (!Object->HasField(TEXT("location")) && !Object->HasField(TEXT("rotation")) && !Object->HasField(TEXT("scale"))))
                return Error(TEXT("bad_request"), TEXT("set_transform requires actor_path and at least one transform field."));
            AActor* Actor = FindActor(World, Operation.ActorPath);
            if (!Actor || !Actor->GetRootComponent()) return Error(TEXT("actor_not_found"), TEXT("Transform target must be an existing actor with a root component in the editor world."));
            if (!Jev::IsSafeTransformTarget(Actor)) return Error(TEXT("actor_unsupported"), TEXT("Version 0.1 transforms only exact native StaticMeshActor objects without attached parents, children, or child-actor ownership."));
            Operation.Target = Actor;
            if (Actor->IsLockLocation() || FLevelUtils::IsLevelLocked(Actor->GetLevel())) return Error(TEXT("actor_locked"), TEXT("Transform target or its level is locked in the editor."));
            if (TransformTargets.Contains(Operation.ActorPath)) return Error(TEXT("bad_request"), TEXT("A plan may transform each actor only once."));
            TransformTargets.Add(Operation.ActorPath);
            Location = Actor->GetActorLocation();
            const FRotator ActorRotation = Actor->GetActorRotation();
            Rotation = FVector(ActorRotation.Pitch, ActorRotation.Yaw, ActorRotation.Roll);
            Scale = Actor->GetActorScale3D();
        }
        else return Error(TEXT("bad_request"), TEXT("Only spawn_primitive and set_transform are allowed."));
        if (!Jev::ReadVector(Object, TEXT("location"), Location, 1000000.0) || !Jev::ReadVector(Object, TEXT("rotation"), Rotation, 36000.0) || !Jev::ReadVector(Object, TEXT("scale"), Scale, 1000.0, true))
            return Error(TEXT("bad_request"), TEXT("Transforms must be three finite numbers: location +/-1000000 cm, rotation +/-36000 degrees, scale 0.001 to 1000."));
        Operation.Transform = FTransform(FRotator(Rotation.X, Rotation.Y, Rotation.Z), Location, Scale);
        auto Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("op"), Operation.Op);
        if (Operation.Op == TEXT("spawn_primitive")) { Summary->SetStringField(TEXT("shape"), Operation.Shape); Summary->SetStringField(TEXT("label"), Operation.Label); }
        else Summary->SetStringField(TEXT("actor_path"), Operation.ActorPath);
        Summary->SetArrayField(TEXT("location"), Jev::Vector(Location));
        Summary->SetArrayField(TEXT("rotation"), Jev::Vector(Rotation));
        Summary->SetArrayField(TEXT("scale"), Jev::Vector(Scale));
        Normalized.Add(MakeShared<FJsonValueObject>(Summary));
        Plan.Operations.Add(MoveTemp(Operation));
    }
    const FString PlanId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Plans.Add(PlanId, MoveTemp(Plan));
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("plan_id"), PlanId);
    Result->SetNumberField(TEXT("expires_in_seconds"), Jev::PlanLifetime);
    Result->SetStringField(TEXT("revision"), Plans[PlanId].Revision);
    Result->SetArrayField(TEXT("operations"), Normalized);
    return Jev::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Apply(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    FString PlanId;
    if (!Jev::OnlyFields(Params, {TEXT("plan_id")}) || !Jev::ReadString(Params, TEXT("plan_id"), PlanId) || PlanId.Len() > 64)
        return Error(TEXT("bad_request"), TEXT("apply requires only plan_id."));
    FPlan Plan;
    if (!Plans.RemoveAndCopyValue(PlanId, Plan)) return Error(TEXT("unknown_plan"), TEXT("Plan does not exist in this session or has already been consumed."));
    if (Plan.ExpiresAt <= Clock()) return Error(TEXT("expired_plan"), TEXT("Plan expired. Preview the operations again."));
    if (Plan.Project != Jev::ProjectPath() || Plan.WorldInstance.Get() != World || Plan.World != World->GetPathName() || Plan.Revision != Revision(World))
        return Error(TEXT("stale_plan"), TEXT("Editor scene changed since preview. Inspect and preview again."));
    for (auto Actor : Plan.SceneActors)
        if (!Actor.IsValid() || Actor->GetWorld() != World) return Error(TEXT("stale_plan"), TEXT("An actor instance was replaced after preview."));
    if (!World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel()))
        return Error(TEXT("level_locked"), TEXT("The current editor level must be editable."));

    // Resolve every asset and actor before opening the undo transaction.
    TMap<FString, UStaticMesh*> Meshes;
    TMap<FString, TWeakObjectPtr<AActor>> Targets;
    for (const FOperation& Operation : Plan.Operations)
    {
        if (Operation.Op == TEXT("spawn_primitive"))
        {
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Jev::MeshPath(Operation.Shape));
            if (!Mesh) return Error(TEXT("asset_unavailable"), TEXT("An engine primitive mesh could not be loaded."));
            Meshes.Add(Operation.Shape, Mesh);
        }
        else
        {
            AActor* Actor = FindActor(World, Operation.ActorPath);
            if (!Actor || Operation.Target.Get() != Actor || !Jev::IsSafeTransformTarget(Actor) || !Actor->GetRootComponent() || Actor->IsLockLocation() || FLevelUtils::IsLevelLocked(Actor->GetLevel())) return Error(TEXT("stale_plan"), TEXT("A target actor is no longer editable."));
            Targets.Add(Operation.ActorPath, Actor);
        }
    }
    if (Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor scene changed while resolving the plan."));
    TArray<TWeakObjectPtr<AActor>> Spawned;
    TMap<TWeakObjectPtr<AActor>, FTransform> PreviousTransforms;
    TArray<TWeakObjectPtr<AActor>> ChangedActors;
    bool bFailed = false;
    {
        FScopedTransaction Transaction(NSLOCTEXT("JevEditor", "ApplyPlan", "Apply Jev scene plan"));
        World->GetCurrentLevel()->Modify();
        for (const FOperation& Operation : Plan.Operations)
        {
            AActor* Actor = nullptr;
            if (Operation.Op == TEXT("spawn_primitive"))
            {
                FActorSpawnParameters SpawnParameters;
                SpawnParameters.OverrideLevel = World->GetCurrentLevel();
                SpawnParameters.ObjectFlags |= RF_Transactional;
                SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                auto* StaticActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Operation.Transform, SpawnParameters);
                if (!StaticActor) { bFailed = true; break; }
                Actor = StaticActor;
                Spawned.Add(Actor);
                Actor->Modify();
                StaticActor->GetStaticMeshComponent()->Modify();
                StaticActor->GetStaticMeshComponent()->SetStaticMesh(Meshes[Operation.Shape]);
                Actor->SetActorLabel(Operation.Label);
            }
            else
            {
                Actor = Targets[Operation.ActorPath].Get();
                if (!IsValid(Actor) || !Jev::IsSafeTransformTarget(Actor)) { bFailed = true; break; }
                PreviousTransforms.Add(Actor, Actor->GetActorTransform());
                Actor->Modify();
                Actor->GetRootComponent()->Modify();
                if (!Actor->SetActorTransform(Operation.Transform, false, nullptr, ETeleportType::TeleportPhysics)) { bFailed = true; break; }
            }
            Actor->PostEditMove(true);
            Actor->MarkPackageDirty();
            ChangedActors.Add(Actor);
        }
        if (bFailed)
        {
            for (auto& Previous : PreviousTransforms) if (Previous.Key.IsValid()) { Previous.Key->SetActorTransform(Previous.Value, false, nullptr, ETeleportType::TeleportPhysics); Previous.Key->PostEditMove(true); }
            for (auto Actor : Spawned) if (Actor.IsValid()) World->EditorDestroyActor(Actor.Get(), true);
            Transaction.Cancel();
        }
    }
    GEditor->RedrawLevelEditingViewports(true);
    if (bFailed) return Error(TEXT("apply_failed"), TEXT("An editor operation failed; spawned actors and changed transforms were rolled back."));
    TArray<TSharedPtr<FJsonValue>> Changed;
    for (auto Actor : ChangedActors) if (Actor.IsValid()) Changed.Add(MakeShared<FJsonValueObject>(ActorSnapshot(Actor.Get())));
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("applied"), true);
    Result->SetArrayField(TEXT("actors"), Changed);
    Result->SetStringField(TEXT("revision"), Revision(World));
    return Jev::Success(Result);
}
