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
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/ObjectKey.h"

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
        Result->SetNumberField(TEXT("material_slot_count"), Component->GetNumMaterials());
        TArray<TSharedPtr<FJsonValue>> Materials;
        for (int32 Slot = 0; Slot < FMath::Min(Component->GetNumMaterials(), 64); ++Slot)
        {
            auto Material = MakeShared<FJsonObject>();
            Material->SetNumberField(TEXT("slot"), Slot);
            UMaterialInterface* Assigned = Component->GetEditorMaterial(Slot);
            if (IsValid(Assigned)) Material->SetStringField(TEXT("path"), Assigned->GetPathName());
            else Material->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
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
    if (const AStaticMeshActor* StaticActor = Cast<AStaticMeshActor>(Actor))
    {
        const UStaticMeshComponent* Component = StaticActor->GetStaticMeshComponent();
        const UStaticMesh* Mesh = Component->GetStaticMesh();
        State->SetStringField(TEXT("mesh"), GetPathNameSafe(Mesh));
        State->SetStringField(TEXT("mesh_id"), Jev::ObjectIdentity(Mesh));
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
            Material->SetStringField(TEXT("override_path"), GetPathNameSafe(Override));
            Material->SetStringField(TEXT("override_id"), Jev::ObjectIdentity(Override));
            Materials.Add(MakeShared<FJsonValueObject>(Material));
        }
        State->SetArrayField(TEXT("materials"), Materials);
    }
    FString Encoded;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Encoded);
    FJsonSerializer::Serialize(State, Writer);
    const FTCHARToUTF8 Bytes(*Encoded);
    return FMD5::HashBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
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
    const FString State = SessionId + TEXT("|") + Jev::ProjectPath() + TEXT("|") + World->GetPathName() + TEXT("|") + Jev::ObjectIdentity(World) + TEXT("|") + GetPathNameSafe(World->GetCurrentLevel()) + TEXT("|") + Jev::ObjectIdentity(World->GetCurrentLevel()) + TEXT("|") + FString::Join(Entries, TEXT("\n"));
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
    if (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
        return Error(TEXT("play_mode"), TEXT("Stop Play or Simulate before this operation; status and context remain available."));
    if (Action == TEXT("asset_details")) return AssetDetails(Params);
    if (Action == TEXT("actor_details")) return ActorDetails(World, Params);
    if (Action == TEXT("validate")) return Validate(World, Params);
    if (Action == TEXT("capture")) return Capture(World, Params);
    if (Action == TEXT("frame")) return Frame(World, Params);
    if (Action == TEXT("preview")) return Preview(World, Params);
    if (Action == TEXT("apply")) return Apply(World, Params);
    if (Action != TEXT("actors") && Action != TEXT("assets"))
        return Error(TEXT("unknown_action"), TEXT("Supported actions: status, context, actors, actor_details, assets, asset_details, validate, capture, frame, preview, apply."));

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
