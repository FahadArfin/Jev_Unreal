#include "JevEditorBridge.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Misc/Paths.h"
#include "Selection.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace JevCompact
{
constexpr int32 MaxRows = 200;
constexpr int32 MaxCandidates = 5000;

bool Only(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Fields)
{
    for (const auto& Pair : Object->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool Text(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& Out, int32 Max)
{
    if (!Object->HasField(Name)) return true;
    if (!Object->HasTypedField<EJson::String>(Name) || !Object->TryGetStringField(Name, Out) || Out.Len() > Max) return false;
    for (TCHAR C : Out) if (C < 32) return false;
    return true;
}

TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

bool Mandatory(const FString& Name)
{
    return Name == TEXT("path") || Name == TEXT("instance_id") || Name == TEXT("editable") || Name == TEXT("edit_blockers") || Name == TEXT("registry_loading") || Name == TEXT("warnings") || Name == TEXT("truncated") || Name.EndsWith(TEXT("_truncated")) || Name.EndsWith(TEXT("_incomplete")) || Name.EndsWith(TEXT("_available"));
}

TSharedRef<FJsonObject> Project(const TSharedRef<FJsonObject>& Row, const TSet<FString>& Fields, const FString& Prefix = TEXT(""))
{
    auto Result = MakeShared<FJsonObject>();
    for (const auto& Pair : Row->Values)
    {
        const FString Name(*Pair.Key);
        if (Mandatory(Name) || Fields.Contains(Prefix + Name)) Result->SetField(Name, Pair.Value);
        else if (Pair.Value->Type == EJson::Object)
        {
            auto Nested = Project(Pair.Value->AsObject().ToSharedRef(), Fields, Prefix + Name + TEXT("."));
            if (!Nested->Values.IsEmpty()) Result->SetObjectField(Name, Nested);
        }
    }
    return Result;
}

void Missing(const TSharedRef<FJsonObject>& Row, const TSet<FString>& Fields)
{
    TArray<FString> Names = Fields.Array(); Names.Sort();
    TArray<TSharedPtr<FJsonValue>> Missing;
    for (const FString& Name : Names)
    {
        FString Parent, Child;
        if (!Name.Split(TEXT("."), &Parent, &Child)) { if (!Row->HasField(Name)) Missing.Add(MakeShared<FJsonValueString>(Name)); }
        else
        {
            const TSharedPtr<FJsonObject>* Nested = nullptr;
            if (!Row->TryGetObjectField(Parent, Nested) || !(*Nested)->HasField(Child)) Missing.Add(MakeShared<FJsonValueString>(Name));
        }
    }
    if (!Missing.IsEmpty()) Row->SetArrayField(TEXT("unavailable_fields"), Missing);
}

FString Encode(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    return Text;
}
}

TSharedRef<FJsonObject> FJevEditorBridge::CompactRead(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    using namespace JevCompact;
    if (!Only(Params, {TEXT("source"), TEXT("query"), TEXT("path"), TEXT("actor_paths"), TEXT("fields"), TEXT("page_size"), TEXT("cursor")}))
        return Error(TEXT("bad_request"), TEXT("Unexpected compact_read parameter."));
    FString Source, Query, Path, Cursor;
    if (!Text(Params, TEXT("source"), Source, 32) || !Text(Params, TEXT("query"), Query, 200) || !Text(Params, TEXT("path"), Path, 512) || !Text(Params, TEXT("cursor"), Cursor, 80))
        return Error(TEXT("bad_request"), TEXT("Compact read strings are bounded and cannot contain control characters."));
    const TSet<FString> Sources = {TEXT("context"), TEXT("actors"), TEXT("actor_details"), TEXT("assets"), TEXT("asset_details")};
    if (!Sources.Contains(Source)) return Error(TEXT("bad_request"), TEXT("Unknown compact read source."));
    if ((GEditor->PlayWorld || GEditor->bIsSimulatingInEditor) && Source != TEXT("context")) return Error(TEXT("play_mode"), TEXT("Only compact context is available during play."));
    double Size = 20;
    if (Params->HasField(TEXT("page_size")) && (!Params->HasTypedField<EJson::Number>(TEXT("page_size")) || !Params->TryGetNumberField(TEXT("page_size"), Size) || !FMath::IsFinite(Size) || Size < 1 || Size > 100 || FMath::FloorToDouble(Size) != Size))
        return Error(TEXT("bad_request"), TEXT("page_size must be an integer from 1 to 100."));
    const int32 PageSize = static_cast<int32>(Size);
    const bool bAsset = Source.StartsWith(TEXT("asset"));
    const TSet<FString> ActorFields = {TEXT("path"), TEXT("instance_id"), TEXT("editable"), TEXT("edit_blockers"), TEXT("bounds_available"), TEXT("label"), TEXT("class"), TEXT("folder"), TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("static_mesh_path"), TEXT("bounds_cm"), TEXT("materials"), TEXT("material_slot_count"), TEXT("material_override_count"), TEXT("mesh_settings"), TEXT("collision_enabled"), TEXT("attachment_parent_path"), TEXT("attachment_parent_instance_id"), TEXT("attachment_relative_transform"), TEXT("attachment_socket")};
    const TSet<FString> AssetFields = {TEXT("path"), TEXT("instance_id"), TEXT("editable"), TEXT("edit_blockers"), TEXT("bounds_available"), TEXT("name"), TEXT("class"), TEXT("loaded"), TEXT("static_mesh.bounds_cm"), TEXT("static_mesh.lods"), TEXT("static_mesh.lod_count"), TEXT("static_mesh.material_slots"), TEXT("static_mesh.material_slot_count"), TEXT("static_mesh.collision")};
    const TArray<TSharedPtr<FJsonValue>>* FieldValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("fields"), FieldValues) || FieldValues->IsEmpty() || FieldValues->Num() > 24) return Error(TEXT("bad_request"), TEXT("fields must contain 1 to 24 documented names."));
    TSet<FString> Fields;
    for (const auto& Value : *FieldValues)
    {
        FString Name;
        if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Name) || Fields.Contains(Name) || !(bAsset ? AssetFields : ActorFields).Contains(Name)) return Error(TEXT("bad_request"), TEXT("Unknown or duplicate field."));
        Fields.Add(Name);
    }
    TArray<FString> Paths;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (Params->HasField(TEXT("actor_paths")))
    {
        if (!Params->TryGetArrayField(TEXT("actor_paths"), Values) || Values->Num() > 20) return Error(TEXT("bad_request"), TEXT("actor_paths exceeds its bound."));
        for (const auto& Value : *Values)
        {
            FString ActorPath;
            if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(ActorPath) || ActorPath.IsEmpty() || ActorPath.Len() > 1024 || Paths.Contains(ActorPath)) return Error(TEXT("bad_request"), TEXT("Invalid or duplicate actor path."));
            for (TCHAR C : ActorPath) if (C < 32) return Error(TEXT("bad_request"), TEXT("Invalid actor path."));
            Paths.Add(ActorPath);
        }
    }
    if ((Source == TEXT("actor_details")) != !Paths.IsEmpty() || ((!Query.IsEmpty()) && Source.EndsWith(TEXT("details"))) || (!Path.IsEmpty() && !bAsset) || (Source == TEXT("asset_details") && Path.IsEmpty()))
        return Error(TEXT("bad_request"), TEXT("Read scope does not match source."));
    if (Source == TEXT("assets"))
    {
        if (Path.IsEmpty()) Path = TEXT("/Game");
        if ((Path != TEXT("/Game") && !Path.StartsWith(TEXT("/Game/"))) || Path.Contains(TEXT("..")) || Path.Len() > 200) return Error(TEXT("bad_request"), TEXT("Asset search requires a /Game content path without traversal."));
    }

    // Build a canonical scope; neither ordering of JSON keys nor fields affects it.
    auto Scope = MakeShared<FJsonObject>();
    Scope->SetStringField(TEXT("source"), Source); Scope->SetStringField(TEXT("query"), Query); Scope->SetStringField(TEXT("path"), Path); Scope->SetNumberField(TEXT("page_size"), PageSize);
    TArray<FString> Names = Fields.Array(); Names.Sort();
    TArray<TSharedPtr<FJsonValue>> SortedFields, ActorPaths;
    for (const FString& Name : Names) SortedFields.Add(MakeShared<FJsonValueString>(Name));
    for (const FString& ActorPath : Paths) ActorPaths.Add(MakeShared<FJsonValueString>(ActorPath));
    Scope->SetArrayField(TEXT("fields"), SortedFields); Scope->SetArrayField(TEXT("actor_paths"), ActorPaths);
    const FString Signature = Encode(Scope);
    const double Now = Clock();
    for (int32 I = CompactReadOrder.Num() - 1; I >= 0; --I)
    {
        const auto* Existing = CompactReads.Find(CompactReadOrder[I]);
        if (!Existing || Existing->ExpiresAt <= Now) { CompactReads.Remove(CompactReadOrder[I]); CompactReadOrder.RemoveAt(I); }
    }
    FString Key; int32 Offset = 0;
    if (!Cursor.IsEmpty())
    {
        FString Number;
        if (!Cursor.Split(TEXT(":"), &Key, &Number) || Number.IsEmpty() || Number.Len() > 3) return Error(TEXT("invalid_cursor"), TEXT("Use the returned native cursor."));
        for (TCHAR C : Number) if (C < '0' || C > '9') return Error(TEXT("invalid_cursor"), TEXT("Invalid native cursor offset."));
        Offset = FCString::Atoi(*Number);
        const auto* Existing = CompactReads.Find(Key);
        if (!Existing) return Error(TEXT("read_expired"), TEXT("Native read expired, was evicted, or belongs to another bridge."));
        if (Existing->Signature != Signature || Offset <= 0 || Offset >= Existing->Rows.Num() || Offset % PageSize) return Error(TEXT("invalid_cursor"), TEXT("Cursor does not match this scope or page."));
        if (Existing->World != World->GetPathName() || Existing->Revision != Revision(World)) return Error(TEXT("stale_cursor"), TEXT("Editor identity/revision changed; start a new read."));
    }
    else
    {
        FCompactRead Read;
        Read.Signature = Signature; Read.World = World->GetPathName(); Read.ExpiresAt = Now + 120;
        auto Status = StatusSnapshot(World); Read.Revision = Status->GetStringField(TEXT("revision"));
        Read.Metadata = MakeShared<FJsonObject>();
        bool bTruncated = false, bIncomplete = false;
        if (Source == TEXT("asset_details"))
        {
            auto Exact = MakeShared<FJsonObject>(); Exact->SetStringField(TEXT("path"), Path);
            auto Response = AssetDetails(Exact, &Fields);
            if (!Response->GetBoolField(TEXT("ok"))) return Response;
            auto Row = Project(Response->GetObjectField(TEXT("result")).ToSharedRef(), Fields); Missing(Row, Fields);
            Read.Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        else if (Source == TEXT("assets"))
        {
            auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            FARFilter Filter; Filter.PackagePaths.Add(FName(*Path)); Filter.bRecursivePaths = true;
            TArray<FAssetData> Assets;
            Registry.EnumerateAssets(Filter, [&](const FAssetData& Asset) { if (Assets.Num() == MaxCandidates) { bIncomplete = true; return false; } Assets.Add(Asset); return true; });
            Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
            for (const FAssetData& Asset : Assets)
            {
                const FString AssetPath = Asset.GetSoftObjectPath().ToString();
                if (!Query.IsEmpty() && !AssetPath.Contains(Query)) continue;
                if (Read.Rows.Num() == MaxRows) { bTruncated = true; break; }
                auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("path"), AssetPath); Row->SetStringField(TEXT("name"), Asset.AssetName.ToString()); Row->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
                Row = Project(Row, Fields); Missing(Row, Fields); Read.Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
            Read.Metadata->SetBoolField(TEXT("registry_loading"), Registry.IsLoadingAssets());
            Read.Metadata->SetNumberField(TEXT("examined_assets"), Assets.Num());
        }
        else
        {
            TArray<AActor*> Actors;
            if (Source == TEXT("actor_details"))
            {
                for (const FString& ActorPath : Paths)
                {
                    AActor* Actor = FindActor(World, ActorPath);
                    if (!IsValid(Actor)) return Error(TEXT("actor_not_found"), TEXT("Every requested actor must exist; no partial snapshot was returned."));
                    Actors.Add(Actor);
                }
            }
            else
            {
                for (TActorIterator<AActor> It(World); It; ++It) { if (Actors.Num() == MaxCandidates) { bIncomplete = true; break; } Actors.Add(*It); }
                Actors.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });
            }
            TArray<TSharedPtr<FJsonValue>> Selected;
            bool bSelectionTruncated = false;
            for (AActor* Actor : Actors)
            {
                if (Source == TEXT("context") && GEditor->GetSelectedActors()->IsSelected(Actor))
                {
                    if (Selected.Num() < MaxRows) Selected.Add(MakeShared<FJsonValueString>(Actor->GetPathName())); else bSelectionTruncated = true;
                }
                if (!Query.IsEmpty() && !Actor->GetActorLabel().Contains(Query) && !Actor->GetPathName().Contains(Query)) continue;
                if (Read.Rows.Num() == MaxRows) { bTruncated = true; continue; }
                if (Actor->GetActorTransform().ContainsNaN()) return Error(TEXT("actor_bounds_unavailable"), TEXT("Non-finite actor transform cannot be inspected safely."));
                auto Row = ActorSnapshot(Actor, &Fields);
                const FBox Bounds = Actor->GetComponentsBoundingBox(true, false);
                const bool bBounds = Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN() && !Bounds.GetCenter().ContainsNaN() && !Bounds.GetSize().ContainsNaN() && !Bounds.GetExtent().IsNearlyZero();
                Row->SetBoolField(TEXT("bounds_available"), bBounds);
                if (Fields.Contains(TEXT("bounds_cm")))
                {
                    if (bBounds) { auto Box = MakeShared<FJsonObject>(); Box->SetArrayField(TEXT("min"), Vector(Bounds.Min)); Box->SetArrayField(TEXT("max"), Vector(Bounds.Max)); Box->SetArrayField(TEXT("center"), Vector(Bounds.GetCenter())); Box->SetArrayField(TEXT("size"), Vector(Bounds.GetSize())); Row->SetObjectField(TEXT("bounds_cm"), Box); }
                    else Row->SetField(TEXT("bounds_cm"), MakeShared<FJsonValueNull>());
                }
                const auto Blockers = ActorEditBlockers(Actor); TArray<TSharedPtr<FJsonValue>> Reasons;
                for (const FString& Blocker : Blockers) Reasons.Add(MakeShared<FJsonValueString>(Blocker));
                Row->SetArrayField(TEXT("edit_blockers"), Reasons); Row->SetBoolField(TEXT("editable"), Blockers.IsEmpty());
                Row = Project(Row, Fields); Missing(Row, Fields); Read.Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
            Read.Metadata->SetNumberField(TEXT("examined_actors"), Actors.Num());
            if (Source == TEXT("context"))
            {
                Read.Metadata->SetArrayField(TEXT("selected_actor_paths"), Selected); Read.Metadata->SetBoolField(TEXT("selection_truncated"), bSelectionTruncated || bIncomplete);
                TArray<UPackage*> Worlds, Content; FEditorFileUtils::GetDirtyWorldPackages(Worlds); FEditorFileUtils::GetDirtyContentPackages(Content);
                TSet<FString> Packages; for (UPackage* Package : Worlds) if (IsValid(Package)) Packages.Add(Package->GetName()); for (UPackage* Package : Content) if (IsValid(Package)) Packages.Add(Package->GetName());
                auto Dirty = Packages.Array(); Dirty.Sort(); TArray<TSharedPtr<FJsonValue>> DirtyValues;
                for (int32 I = 0; I < FMath::Min(Dirty.Num(), MaxRows); ++I) DirtyValues.Add(MakeShared<FJsonValueString>(Dirty[I]));
                Read.Metadata->SetArrayField(TEXT("dirty_packages"), DirtyValues); Read.Metadata->SetNumberField(TEXT("dirty_package_count"), Dirty.Num()); Read.Metadata->SetBoolField(TEXT("dirty_packages_truncated"), Dirty.Num() > MaxRows);
                Read.Metadata->SetBoolField(TEXT("play_in_editor"), GEditor->PlayWorld != nullptr); Read.Metadata->SetBoolField(TEXT("simulating"), GEditor->bIsSimulatingInEditor);
            }
        }
        if (Read.Revision != Revision(World)) return Error(TEXT("read_state_changed"), TEXT("Scene changed during capture; no native read retained."));
        Read.Metadata->SetBoolField(TEXT("truncated"), bTruncated || bIncomplete); Read.Metadata->SetBoolField(TEXT("scan_incomplete"), bIncomplete);
        auto Encoded = MakeShared<FJsonObject>(); Encoded->SetArrayField(TEXT("items"), Read.Rows); Encoded->SetObjectField(TEXT("metadata"), Read.Metadata);
        const FTCHARToUTF8 Bytes(*Encode(Encoded)); Read.Bytes = Bytes.Length();
        if (Read.Bytes > 900000) return Error(TEXT("read_too_large"), TEXT("Native read exceeds its 900000-byte budget; select fewer fields."));
        int64 Retained = 0; for (const auto& Pair : CompactReads) Retained += Pair.Value.Bytes;
        while (!CompactReadOrder.IsEmpty() && (CompactReads.Num() >= 32 || Retained + Read.Bytes > 8388608))
        {
            Retained -= CompactReads[CompactReadOrder[0]].Bytes; CompactReads.Remove(CompactReadOrder[0]); CompactReadOrder.RemoveAt(0);
        }
        Key = FGuid::NewGuid().ToString(EGuidFormats::Digits); CompactReads.Add(Key, MoveTemp(Read)); CompactReadOrder.Add(Key);
    }
    const FCompactRead& Read = CompactReads[Key];
    auto Result = MakeShared<FJsonObject>(); auto Identity = MakeShared<FJsonObject>();
    Identity->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())); Identity->SetStringField(TEXT("session_id"), SessionId); Identity->SetStringField(TEXT("world_path"), Read.World); Identity->SetStringField(TEXT("revision"), Read.Revision);
    Result->SetObjectField(TEXT("identity"), Identity); Result->SetObjectField(TEXT("metadata"), Read.Metadata); Result->SetStringField(TEXT("native_read_id"), Key);
    TArray<TSharedPtr<FJsonValue>> Page; const int32 End = FMath::Min(Offset + PageSize, Read.Rows.Num());
    for (int32 I = Offset; I < End; ++I) Page.Add(Read.Rows[I]);
    Result->SetArrayField(TEXT("items"), Page); Result->SetNumberField(TEXT("captured_count"), Read.Rows.Num()); Result->SetNumberField(TEXT("returned_count"), Page.Num()); Result->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, Read.ExpiresAt - Clock()));
    if (End < Read.Rows.Num()) Result->SetStringField(TEXT("next_cursor"), Key + TEXT(":") + LexToString(End)); else Result->SetField(TEXT("next_cursor"), MakeShared<FJsonValueNull>());
    auto Response = MakeShared<FJsonObject>(); Response->SetBoolField(TEXT("ok"), true); Response->SetObjectField(TEXT("result"), Result); return Response;
}
