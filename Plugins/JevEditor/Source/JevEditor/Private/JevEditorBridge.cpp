#include "JevEditorBridge.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "LevelUtils.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectGlobals.h"

namespace Jev
{
constexpr int32 MaxResults = 200;

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

bool OnlyFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Fields)
{
    for (const auto& Pair : Object->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

FString ProjectPath()
{
    return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
}

FString ObjectIdentity(const UObject* Object)
{
    // FObjectKey includes the weak-object serial, not just a reusable object index.
    // Its supported nonpersistent serialization also handles remote object handles.
    FObjectKey Key(Object);
    TArray<uint8> Bytes;
    FMemoryWriter Writer(Bytes, false);
    Writer << Key;
    return FBase64::Encode(Bytes);
}

FString JsonFingerprint(const TSharedRef<FJsonObject>& State)
{
    FString Encoded;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Encoded);
    FJsonSerializer::Serialize(State, Writer);
    const FTCHARToUTF8 Bytes(*Encoded);
    return FMD5::HashBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
}

}

FJevEditorBridge::FJevEditorBridge(TFunction<double()> InClock)
    : SessionId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens)), Clock(MoveTemp(InClock))
{
    if (!Clock) Clock = [] { return FPlatformTime::Seconds(); };
    // Conservative, bounded invalidation: editor changes to any mesh/material/body
    // invalidate reviewed scene state, including changes to material ancestors.
    AssetChangeHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* Object, FPropertyChangedEvent&)
    {
        for (UObject* Current = Object; Current; Current = Current->GetOuter())
            if (Current->IsA<UStaticMesh>() || Current->IsA<UMaterialInterface>() || Current->IsA<UBodySetup>())
            { ++AssetChangeEpoch; break; }
    });
}

FJevEditorBridge::~FJevEditorBridge()
{
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(AssetChangeHandle);
}

FJevEditorBridge::FMeshSettings FJevEditorBridge::CaptureMeshSettings(AStaticMeshActor* Actor) const
{
    FMeshSettings Settings;
    const UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
    Settings.Mobility = static_cast<uint8>(Component->Mobility);
    Settings.CollisionMode = static_cast<uint8>(Component->BodyInstance.GetCollisionEnabled(false));
    Settings.CollisionObjectType = static_cast<uint8>(Component->GetCollisionObjectType());
    Settings.CollisionProfile = Component->GetCollisionProfileName();
    Settings.bUseMeshDefaultCollision = Component->bUseDefaultCollision;
    for (int32 Channel = 0; Channel < 32; ++Channel) Settings.CollisionResponses.Add(static_cast<uint8>(Component->GetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel))));
    Settings.bActorCollisionEnabled = Actor->GetActorEnableCollision();
    Settings.bCastShadow = Component->CastShadow;
    Settings.bVisible = Component->IsVisible();
    Settings.bHiddenInGame = Component->bHiddenInGame;
    Settings.bActorHiddenInGame = Actor->IsHidden();
    Settings.bActorHiddenInEditor = Actor->IsTemporarilyHiddenInEditor();
    Settings.ActorTags = Actor->Tags;
    Settings.ComponentTags = Component->ComponentTags;
    return Settings;
}

TSharedRef<FJsonObject> FJevEditorBridge::MeshSettingsSnapshot(const FMeshSettings& Settings) const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("mobility"), Settings.Mobility == EComponentMobility::Static ? TEXT("Static") : Settings.Mobility == EComponentMobility::Stationary ? TEXT("Stationary") : TEXT("Movable"));
    Result->SetNumberField(TEXT("collision_mode"), Settings.CollisionMode);
    Result->SetStringField(TEXT("collision_profile"), Settings.CollisionProfile.ToString());
    Result->SetBoolField(TEXT("use_mesh_default_collision"), Settings.bUseMeshDefaultCollision);
    Result->SetNumberField(TEXT("collision_object_type"), Settings.CollisionObjectType);
    TArray<TSharedPtr<FJsonValue>> Responses;
    for (uint8 Value : Settings.CollisionResponses) Responses.Add(MakeShared<FJsonValueNumber>(Value));
    Result->SetArrayField(TEXT("collision_responses"), Responses);
    Result->SetBoolField(TEXT("actor_collision_enabled"), Settings.bActorCollisionEnabled);
    Result->SetBoolField(TEXT("cast_shadow"), Settings.bCastShadow);
    Result->SetBoolField(TEXT("visible"), Settings.bVisible);
    Result->SetBoolField(TEXT("hidden_in_game"), Settings.bHiddenInGame);
    Result->SetBoolField(TEXT("actor_hidden_in_game"), Settings.bActorHiddenInGame);
    Result->SetBoolField(TEXT("actor_hidden_in_editor"), Settings.bActorHiddenInEditor);
    const auto Tags = [](const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (int32 I = 0; I < FMath::Min(Names.Num(), 32); ++I) Values.Add(MakeShared<FJsonValueString>(Names[I].ToString().Left(128)));
        return Values;
    };
    Result->SetArrayField(TEXT("actor_tags"), Tags(Settings.ActorTags));
    Result->SetArrayField(TEXT("component_tags"), Tags(Settings.ComponentTags));
    Result->SetBoolField(TEXT("tags_truncated"), Settings.ActorTags.Num() > 32 || Settings.ComponentTags.Num() > 32 || Settings.ActorTags.ContainsByPredicate([](FName Name) { return Name.ToString().Len() > 128; }) || Settings.ComponentTags.ContainsByPredicate([](FName Name) { return Name.ToString().Len() > 128; }));
    return Result;
}

TSharedRef<FJsonObject> FJevEditorBridge::MeshAssetSnapshot(UStaticMesh* Mesh) const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), GetPathNameSafe(Mesh));
    if (!IsValid(Mesh)) return Result;
    Result->SetStringField(TEXT("instance_id"), Jev::ObjectIdentity(Mesh));
    Result->SetStringField(TEXT("lighting_guid"), Mesh->GetLightingGuid().ToString());
    const FBoxSphereBounds Bounds = Mesh->GetBounds();
    Result->SetArrayField(TEXT("local_bounds_center_cm"), Jev::Vector(Bounds.Origin));
    Result->SetArrayField(TEXT("local_bounds_extent_cm"), Jev::Vector(Bounds.BoxExtent));
    Result->SetNumberField(TEXT("material_slot_count"), Mesh->GetStaticMaterials().Num());
    TArray<TSharedPtr<FJsonValue>> Slots;
    for (int32 I = 0; I < FMath::Min(Mesh->GetStaticMaterials().Num(), 64); ++I)
    {
        const auto& Slot = Mesh->GetStaticMaterials()[I];
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("slot_name"), Slot.MaterialSlotName.ToString());
        Item->SetStringField(TEXT("imported_slot_name"), Slot.ImportedMaterialSlotName.ToString());
        Item->SetStringField(TEXT("material_fingerprint"), MaterialAssetFingerprint(Slot.MaterialInterface.Get()));
        Slots.Add(MakeShared<FJsonValueObject>(Item));
    }
    Result->SetArrayField(TEXT("slots"), Slots);
    const UBodySetup* Body = Mesh->GetBodySetup();
    Result->SetStringField(TEXT("body_instance_id"), Jev::ObjectIdentity(Body));
    if (IsValid(Body))
    {
        Result->SetStringField(TEXT("body_guid"), Body->BodySetupGuid.ToString());
        Result->SetNumberField(TEXT("collision_trace_mode"), static_cast<uint8>(Body->CollisionTraceFlag));
        Result->SetNumberField(TEXT("simple_collision_shapes"), Body->AggGeom.GetElementCount());
        auto Defaults = MakeShared<FJsonObject>();
        Defaults->SetStringField(TEXT("profile"), Body->DefaultInstance.GetCollisionProfileName().ToString());
        Defaults->SetNumberField(TEXT("mode"), static_cast<uint8>(Body->DefaultInstance.GetCollisionEnabled(false)));
        Defaults->SetNumberField(TEXT("object_type"), static_cast<uint8>(Body->DefaultInstance.GetObjectType()));
        TArray<TSharedPtr<FJsonValue>> Responses;
        for (int32 Channel = 0; Channel < 32; ++Channel) Responses.Add(MakeShared<FJsonValueNumber>(static_cast<uint8>(Body->DefaultInstance.GetResponseToChannel(static_cast<ECollisionChannel>(Channel)))));
        Defaults->SetArrayField(TEXT("responses"), Responses);
        Result->SetObjectField(TEXT("default_collision"), Defaults);
    }
    Result->SetStringField(TEXT("pivot_semantics"), TEXT("Mesh local origin remains the actor transform origin; bounds center can move when mesh geometry changes."));
    return Result;
}

FString FJevEditorBridge::MeshAssetFingerprint(UStaticMesh* Mesh) const
{
    return Jev::JsonFingerprint(MeshAssetSnapshot(Mesh));
}

FString FJevEditorBridge::MaterialAssetFingerprint(UMaterialInterface* Material) const
{
    return IsValid(Material) ? GetPathNameSafe(Material) + TEXT("|") + Jev::ObjectIdentity(Material) + TEXT("|") + Material->GetLightingGuid().ToString() : TEXT("null");
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

FString FJevEditorBridge::BoundedResponseBody(const TSharedRef<FJsonObject>& Response)
{
    FString Body;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
    const bool bSerialized = FJsonSerializer::Serialize(Response, Writer);
    const FTCHARToUTF8 Bytes(*Body);
    if (!bSerialized || Bytes.Length() > 1048576)
    {
        Body.Reset();
        auto ErrorWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
        FJsonSerializer::Serialize(Error(TEXT("response_too_large"), TEXT("The editor response exceeds 1 MiB. Reduce the requested limit or narrow the query.")), ErrorWriter);
    }
    return Body;
}

TSharedRef<FJsonObject> FJevEditorBridge::ActorSnapshot(AActor* Actor) const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Actor->GetPathName());
    Result->SetStringField(TEXT("instance_id"), SessionId + TEXT(":") + Jev::ObjectIdentity(Actor));
    Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("folder"), Actor->GetFolderPath().IsNone() ? TEXT("") : Actor->GetFolderPath().ToString());
    Result->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    Result->SetArrayField(TEXT("location"), Jev::Vector(Actor->GetActorLocation()));
    const FRotator Rotation = Actor->GetActorRotation();
    Result->SetArrayField(TEXT("rotation"), Jev::Vector(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
    Result->SetArrayField(TEXT("scale"), Jev::Vector(Actor->GetActorScale3D()));
    if (const AStaticMeshActor* StaticActor = Cast<AStaticMeshActor>(Actor))
    {
        const UStaticMeshComponent* Component = StaticActor->GetStaticMeshComponent();
        if (IsValid(Component->GetStaticMesh())) Result->SetStringField(TEXT("static_mesh_path"), Component->GetStaticMesh()->GetPathName());
        else Result->SetField(TEXT("static_mesh_path"), MakeShared<FJsonValueNull>());
        Result->SetBoolField(TEXT("collision_enabled"), Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
        Result->SetStringField(TEXT("collision_profile"), Component->GetCollisionProfileName().ToString());
        Result->SetObjectField(TEXT("mesh_settings"), MeshSettingsSnapshot(CaptureMeshSettings(const_cast<AStaticMeshActor*>(StaticActor))));
        Result->SetNumberField(TEXT("material_slot_count"), Component->GetNumMaterials());
        Result->SetNumberField(TEXT("material_override_count"), Component->GetNumOverrideMaterials());
        TArray<TSharedPtr<FJsonValue>> Materials;
        for (int32 Slot = 0; Slot < FMath::Min(Component->GetNumMaterials(), 64); ++Slot)
        {
            auto Material = MakeShared<FJsonObject>();
            Material->SetNumberField(TEXT("slot"), Slot);
            UMaterialInterface* Assigned = Component->GetEditorMaterial(Slot);
            if (IsValid(Assigned)) Material->SetStringField(TEXT("path"), Assigned->GetPathName());
            else Material->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
            UMaterialInterface* Override = Component->OverrideMaterials.IsValidIndex(Slot) ? Component->OverrideMaterials[Slot].Get() : nullptr;
            if (IsValid(Override)) Material->SetStringField(TEXT("override_path"), Override->GetPathName());
            else Material->SetField(TEXT("override_path"), MakeShared<FJsonValueNull>());
            Materials.Add(MakeShared<FJsonValueObject>(Material));
        }
        Result->SetArrayField(TEXT("materials"), Materials);
        Result->SetBoolField(TEXT("materials_truncated"), Component->GetNumMaterials() > 64);
    }
    else
    {
        Result->SetField(TEXT("static_mesh_path"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("collision_enabled"), MakeShared<FJsonValueNull>());
        Result->SetArrayField(TEXT("materials"), {});
        Result->SetNumberField(TEXT("material_slot_count"), 0);
        Result->SetBoolField(TEXT("materials_truncated"), false);
    }
    return Result;
}

TArray<FString> FJevEditorBridge::ActorEditBlockers(AActor* Actor) const
{
    TArray<FString> Blockers;
    if (!IsValid(Actor)) { Blockers.Add(TEXT("actor_unavailable")); return Blockers; }
    if (Actor->GetClass() != AStaticMeshActor::StaticClass()) Blockers.Add(TEXT("unsupported_class"));
    if (!Actor->GetRootComponent()) Blockers.Add(TEXT("missing_root"));
    if (Actor->GetAttachParentActor()) Blockers.Add(TEXT("attached_parent"));
    if (Actor->GetParentActor()) Blockers.Add(TEXT("child_actor"));
    TArray<AActor*> Attached;
    Actor->GetAttachedActors(Attached);
    if (!Attached.IsEmpty()) Blockers.Add(TEXT("attached_children"));
    if (Actor->IsLockLocation()) Blockers.Add(TEXT("actor_locked"));
    if (!Actor->IsEditable()) Blockers.Add(TEXT("actor_read_only"));
    if (!Actor->GetLevel() || FLevelUtils::IsLevelLocked(Actor->GetLevel())) Blockers.Add(TEXT("level_locked"));
    return Blockers;
}

FString FJevEditorBridge::ActorEditFingerprint(AActor* Actor) const
{
    auto State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("label"), Actor->GetActorLabel());
    State->SetStringField(TEXT("folder"), Actor->GetFolderPath().IsNone() ? TEXT("") : Actor->GetFolderPath().ToString());
    State->SetBoolField(TEXT("editable"), Actor->IsEditable());
    State->SetBoolField(TEXT("label_editable"), Actor->IsActorLabelEditable());
    State->SetStringField(TEXT("actor_id"), Jev::ObjectIdentity(Actor));
    State->SetStringField(TEXT("level_id"), Jev::ObjectIdentity(Actor->GetLevel()));
    State->SetStringField(TEXT("transform"), Actor->GetActorTransform().ToString());
    State->SetArrayField(TEXT("pivot_offset"), Jev::Vector(Actor->GetPivotOffset()));
    if (const AStaticMeshActor* StaticActor = Cast<AStaticMeshActor>(Actor))
    {
        const UStaticMeshComponent* Component = StaticActor->GetStaticMeshComponent();
        const UStaticMesh* Mesh = Component->GetStaticMesh();
        State->SetStringField(TEXT("mesh"), GetPathNameSafe(Mesh));
        State->SetStringField(TEXT("mesh_id"), Jev::ObjectIdentity(Mesh));
        State->SetStringField(TEXT("mesh_fingerprint"), MeshAssetFingerprint(const_cast<UStaticMesh*>(Mesh)));
        State->SetObjectField(TEXT("mesh_settings"), MeshSettingsSnapshot(CaptureMeshSettings(const_cast<AStaticMeshActor*>(StaticActor))));
        State->SetStringField(TEXT("component_id"), Jev::ObjectIdentity(Component));
        TArray<TSharedPtr<FJsonValue>> Components;
        for (const UActorComponent* Item : Actor->GetComponents()) Components.Add(MakeShared<FJsonValueString>(Jev::ObjectIdentity(Item)));
        State->SetArrayField(TEXT("component_ids"), Components);
        State->SetNumberField(TEXT("material_count"), Component->GetNumMaterials());
        State->SetNumberField(TEXT("override_count"), Component->GetNumOverrideMaterials());
        const FBox Bounds = Actor->GetComponentsBoundingBox(true, false);
        if (Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN())
        {
            State->SetArrayField(TEXT("bounds_min"), Jev::Vector(Bounds.Min));
            State->SetArrayField(TEXT("bounds_max"), Jev::Vector(Bounds.Max));
        }
        TArray<TSharedPtr<FJsonValue>> Materials;
        for (int32 Slot = 0; Slot < FMath::Min(FMath::Max(Component->GetNumMaterials(), Component->GetNumOverrideMaterials()), 64); ++Slot)
        {
            auto Material = MakeShared<FJsonObject>();
            const UMaterialInterface* Effective = Component->GetEditorMaterial(Slot);
            const UMaterialInterface* Override = Component->OverrideMaterials.IsValidIndex(Slot) ? Component->OverrideMaterials[Slot].Get() : nullptr;
            Material->SetStringField(TEXT("path"), GetPathNameSafe(Effective));
            Material->SetStringField(TEXT("id"), Jev::ObjectIdentity(Effective));
            Material->SetStringField(TEXT("content"), MaterialAssetFingerprint(const_cast<UMaterialInterface*>(Effective)));
            Material->SetStringField(TEXT("override_path"), GetPathNameSafe(Override));
            Material->SetStringField(TEXT("override_id"), Jev::ObjectIdentity(Override));
            Materials.Add(MakeShared<FJsonValueObject>(Material));
        }
        State->SetArrayField(TEXT("materials"), Materials);
    }
    return Jev::JsonFingerprint(State);
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
        Entries.Last() += TEXT("|") + Jev::ObjectIdentity(Actor) + TEXT("|") + ActorEditFingerprint(Actor);
    }
    Entries.Sort();
    const FString State = SessionId + TEXT("|") + LexToString(AssetChangeEpoch) + TEXT("|") + Jev::ProjectPath() + TEXT("|") + World->GetPathName() + TEXT("|") + Jev::ObjectIdentity(World) + TEXT("|") + GetPathNameSafe(World->GetCurrentLevel()) + TEXT("|") + Jev::ObjectIdentity(World->GetCurrentLevel()) + TEXT("|") + FString::Join(Entries, TEXT("\n"));
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
    if (Action == TEXT("status"))
    {
        if (!Params->Values.IsEmpty()) return Error(TEXT("bad_request"), TEXT("status has no parameters."));
        return Jev::Success(StatusSnapshot(World));
    }
    if (Action == TEXT("context")) return Context(World, Params);
    if (Action == TEXT("plan_status")) return PlanStatus(Params);
    if (Action == TEXT("pending_plans")) return PendingPlans(Params);
    if (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
        return Error(TEXT("play_mode"), TEXT("Stop Play or Simulate before this operation; status and context remain available."));
    if (Action == TEXT("asset_details")) return AssetDetails(Params);
    if (Action == TEXT("actor_details")) return ActorDetails(World, Params);
    if (Action == TEXT("validate")) return Validate(World, Params);
    if (Action == TEXT("capture")) return Capture(World, Params);
    if (Action == TEXT("frame")) return Frame(World, Params);
    if (Action == TEXT("preview")) return Preview(World, Params);
    if (Action == TEXT("apply")) return ApplyTracked(World, Params);
    if (Action != TEXT("actors") && Action != TEXT("assets"))
        return Error(TEXT("unknown_action"), TEXT("Supported actions: status, context, actors, actor_details, assets, asset_details, validate, capture, frame, preview, apply, plan_status, pending_plans."));

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

void FJevEditorBridge::PrunePlanRecords()
{
    const double Now = Clock();
    for (auto It = PlanRecords.CreateIterator(); It; ++It)
    {
        FPlanRecord& Record = It.Value();
        if (Record.Status == TEXT("pending") && Record.ExpiresAt <= Now)
        {
            Record.Status = TEXT("expired");
            Record.OutcomeCode = TEXT("expired_plan");
            Record.UpdatedAt = Now;
            Plans.Remove(It.Key());
        }
        if (Record.Status != TEXT("applying") && Now - Record.CreatedAt >= 900)
        {
            Plans.Remove(It.Key());
            PlanRecordOrder.Remove(It.Key());
            It.RemoveCurrent();
        }
    }
}

TSharedRef<FJsonObject> FJevEditorBridge::PlanRecordSnapshot(const FString& PlanId, const FPlanRecord& Record) const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("plan_id"), PlanId);
    Result->SetStringField(TEXT("session_id"), SessionId);
    Result->SetStringField(TEXT("status"), Record.Status);
    Result->SetObjectField(TEXT("review"), Record.Review);
    Result->SetBoolField(TEXT("applied"), Record.Status == TEXT("applied"));
    if (Record.Status == TEXT("unknown") || Record.Status == TEXT("applying")) Result->SetField(TEXT("executed"), MakeShared<FJsonValueNull>());
    else Result->SetBoolField(TEXT("executed"), Record.Status == TEXT("applied") || Record.Status == TEXT("rolled_back"));
    Result->SetBoolField(TEXT("saved"), false);
    Result->SetBoolField(TEXT("requires_fresh_verification"), true);
    Result->SetStringField(TEXT("scope"), TEXT("editor_session_memory"));
    Result->SetStringField(TEXT("note"), TEXT("Historical outcome only. Undo, external edits, and map changes are not reflected here. Records survive an MCP reconnect, but not editor restart."));
    Result->SetNumberField(TEXT("age_seconds"), FMath::Max(0.0, Clock() - Record.CreatedAt));
    Result->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, Record.ExpiresAt - Clock()));
    Result->SetNumberField(TEXT("retention_remaining_seconds"), FMath::Max(0.0, 900.0 - (Clock() - Record.CreatedAt)));
    if (!Record.OutcomeCode.IsEmpty()) Result->SetStringField(TEXT("outcome_code"), Record.OutcomeCode);
    if (!Record.RevisionAfter.IsEmpty()) Result->SetStringField(TEXT("revision_after"), Record.RevisionAfter);
    TArray<TSharedPtr<FJsonValue>> Paths;
    for (const FString& Path : Record.ActorPaths) Paths.Add(MakeShared<FJsonValueString>(Path));
    Result->SetArrayField(TEXT("actor_paths"), Paths);
    return Result;
}

TSharedRef<FJsonObject> FJevEditorBridge::PlanStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString PlanId;
    if (!Jev::OnlyFields(Params, {TEXT("plan_id")}) || !Jev::ReadString(Params, TEXT("plan_id"), PlanId) || PlanId.IsEmpty() || PlanId.Len() > 64)
        return Error(TEXT("bad_request"), TEXT("plan_status requires only a nonempty plan_id of at most 64 characters."));
    PrunePlanRecords();
    const FPlanRecord* Record = PlanRecords.Find(PlanId);
    if (!Record) return Error(TEXT("unknown_plan"), TEXT("No retained record exists in this editor session. History is bounded to 64 records and 15 minutes."));
    return Jev::Success(PlanRecordSnapshot(PlanId, *Record));
}

TSharedRef<FJsonObject> FJevEditorBridge::PendingPlans(const TSharedPtr<FJsonObject>& Params)
{
    double LimitNumber = 20;
    if (!Jev::OnlyFields(Params, {TEXT("limit")}) || (Params->HasField(TEXT("limit")) &&
        (!Params->HasTypedField<EJson::Number>(TEXT("limit")) || !Params->TryGetNumberField(TEXT("limit"), LimitNumber) ||
        !FMath::IsFinite(LimitNumber) || LimitNumber < 1 || LimitNumber > 64 || FMath::FloorToDouble(LimitNumber) != LimitNumber)))
        return Error(TEXT("bad_request"), TEXT("pending_plans accepts only limit, an integer from 1 to 64."));
    PrunePlanRecords();
    TArray<TSharedPtr<FJsonValue>> Items;
    int32 PendingCount = 0;
    for (int32 Index = PlanRecordOrder.Num() - 1; Index >= 0; --Index)
    {
        const FPlanRecord* Record = PlanRecords.Find(PlanRecordOrder[Index]);
        if (!Record || Record->Status != TEXT("pending")) continue;
        ++PendingCount;
        if (Items.Num() >= static_cast<int32>(LimitNumber)) continue;
        // The list contains summaries, so 64 plans cannot exceed the response cap.
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("plan_id"), PlanRecordOrder[Index]);
        Item->SetStringField(TEXT("status"), Record->Status);
        Item->SetStringField(TEXT("project_file"), Record->Review->GetStringField(TEXT("project_file")));
        Item->SetStringField(TEXT("world_path"), Record->Review->GetStringField(TEXT("world_path")));
        Item->SetNumberField(TEXT("operation_count"), Record->Review->GetArrayField(TEXT("operations")).Num());
        Item->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, Record->ExpiresAt - Clock()));
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("session_id"), SessionId);
    Result->SetStringField(TEXT("scope"), TEXT("editor_session_memory"));
    Result->SetArrayField(TEXT("plans"), Items);
    Result->SetBoolField(TEXT("truncated"), PendingCount > Items.Num());
    Result->SetNumberField(TEXT("pending_count"), PendingCount);
    return Jev::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::ApplyTracked(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    if (bApplyingPlan) return Error(TEXT("editor_busy"), TEXT("A Jev plan is already applying. Inspect its outcome before another edit."));
    TGuardValue<bool> ApplyGuard(bApplyingPlan, true);
    FString PlanId;
    const bool bTracked = Jev::OnlyFields(Params, {TEXT("plan_id")}) && Jev::ReadString(Params, TEXT("plan_id"), PlanId) &&
        PlanId.Len() <= 64 && Plans.Contains(PlanId) && PlanRecords.Contains(PlanId);
    if (bTracked)
    {
        PlanRecords[PlanId].Status = TEXT("applying");
        PlanRecords[PlanId].UpdatedAt = Clock();
    }
    const auto Response = Apply(World, Params);
    if (FPlanRecord* RecordPtr = bTracked ? PlanRecords.Find(PlanId) : nullptr)
    {
        FPlanRecord& Record = *RecordPtr;
        Record.UpdatedAt = Clock();
        if (Response->GetBoolField(TEXT("ok")))
        {
            const auto Result = Response->GetObjectField(TEXT("result"));
            Record.Status = TEXT("applied");
            Record.RevisionAfter = Result->GetStringField(TEXT("revision"));
            for (const auto& Actor : Result->GetArrayField(TEXT("actors"))) Record.ActorPaths.Add(Actor->AsObject()->GetStringField(TEXT("path")));
        }
        else
        {
            Record.OutcomeCode = Response->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
            Record.Status = Record.OutcomeCode == TEXT("rollback_failed") ? TEXT("unknown") :
                Record.OutcomeCode == TEXT("apply_failed") ? TEXT("rolled_back") :
                Record.OutcomeCode == TEXT("expired_plan") ? TEXT("expired") : TEXT("rejected");
        }
    }
    return Response;
}
