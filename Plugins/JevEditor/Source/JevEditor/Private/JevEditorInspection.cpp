#include "JevEditorBridge.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderingThread.h"
#include "Selection.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

namespace JevInspection
{
constexpr int32 MaxCandidates = 5000;
constexpr int32 MaxWarnings = 400;
constexpr int32 MaxComponents = 64;
constexpr int32 MaxMaterials = 64;
constexpr int32 MaxPngBytes = 720 * 1024;

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), true);
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

bool OnlyFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Fields)
{
    for (const auto& Pair : Object->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool Integer(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Low, int32 High, int32& Out)
{
    if (!Params->HasField(Name)) return true;
    double Value = 0;
    if (!Params->HasTypedField<EJson::Number>(Name) || !Params->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value) || Value < Low || Value > High || FMath::FloorToDouble(Value) != Value) return false;
    Out = static_cast<int32>(Value);
    return true;
}

bool QueryLimit(const TSharedPtr<FJsonObject>& Params, FString& Query, int32& Limit)
{
    if (!OnlyFields(Params, {TEXT("query"), TEXT("limit")}) || !Integer(Params, TEXT("limit"), 1, 200, Limit)) return false;
    return !Params->HasField(TEXT("query")) || (Params->HasTypedField<EJson::String>(TEXT("query")) && Params->TryGetStringField(TEXT("query"), Query) && Query.Len() <= 200);
}

TArray<AActor*> Candidates(UWorld* World, bool& bIncomplete)
{
    TArray<AActor*> Actors;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (Actors.Num() >= MaxCandidates) { bIncomplete = true; break; }
        Actors.Add(*It);
    }
    Actors.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });
    return Actors;
}

bool Matches(const AActor* Actor, const FString& Query)
{
    return Query.IsEmpty() || Actor->GetActorLabel().Contains(Query) || Actor->GetPathName().Contains(Query);
}

FString TraceFlag(ECollisionTraceFlag Flag)
{
    switch (Flag)
    {
    case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
    case CTF_UseSimpleAsComplex: return TEXT("simple_as_complex");
    case CTF_UseComplexAsSimple: return TEXT("complex_as_simple");
    default: return TEXT("project_default");
    }
}
}

TSharedRef<FJsonObject> FJevEditorBridge::StatusSnapshot(UWorld* World) const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Result->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
    Result->SetStringField(TEXT("session_id"), SessionId);
    Result->SetStringField(TEXT("world_path"), World->GetPathName());
    Result->SetStringField(TEXT("current_level"), GetPathNameSafe(World->GetCurrentLevel()));
    Result->SetStringField(TEXT("revision"), Revision(World));
    Result->SetStringField(TEXT("bridge_version"), TEXT("0.6.0"));
    Result->SetBoolField(TEXT("play_in_editor"), GEditor->PlayWorld != nullptr);
    Result->SetBoolField(TEXT("simulating"), GEditor->bIsSimulatingInEditor);
    Result->SetBoolField(TEXT("editor_world"), true);
    TArray<TSharedPtr<FJsonValue>> Capabilities;
    for (const TCHAR* Capability : {TEXT("status"), TEXT("context"), TEXT("actors"), TEXT("actor_details"), TEXT("assets"), TEXT("asset_details"), TEXT("validate"), TEXT("capture"), TEXT("frame"), TEXT("frame_views"), TEXT("preview"), TEXT("preview_expected_state"), TEXT("set_material"), TEXT("set_metadata"), TEXT("apply"), TEXT("pending_plans"), TEXT("plan_status"), TEXT("blueprint_inspect"), TEXT("asset_dependencies"), TEXT("asset_import_info"), TEXT("validation_rules"), TEXT("validation_start"), TEXT("validation_job"), TEXT("validation_cancel"), TEXT("functional_tests"), TEXT("functional_start"), TEXT("functional_job"), TEXT("functional_cancel")})
        Capabilities.Add(MakeShared<FJsonValueString>(Capability));
    Capabilities.Add(MakeShared<FJsonValueString>(TEXT("replace_mesh")));
    Capabilities.Add(MakeShared<FJsonValueString>(TEXT("duplicate_mesh")));
    Result->SetArrayField(TEXT("capabilities"), Capabilities);
    return Result;
}

TSharedRef<FJsonObject> FJevEditorBridge::Context(UWorld* World, const TSharedPtr<FJsonObject>& Params) const
{
    FString Query;
    int32 Limit = 50;
    if (!JevInspection::QueryLimit(Params, Query, Limit)) return Error(TEXT("bad_request"), TEXT("context accepts only query (string, max 200) and limit (integer, 1 to 200)."));
    auto Result = StatusSnapshot(World);
    bool bScanIncomplete = false, bActorsTruncated = false, bSelectionTruncated = false;
    const auto Actors = JevInspection::Candidates(World, bScanIncomplete);
    TArray<TSharedPtr<FJsonValue>> Snapshots, Selected;
    const USelection* Selection = GEditor->GetSelectedActors();
    for (AActor* Actor : Actors)
    {
        if (Selection && Selection->IsSelected(Actor))
        {
            if (Selected.Num() < Limit) Selected.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
            else bSelectionTruncated = true;
        }
        if (!JevInspection::Matches(Actor, Query)) continue;
        if (Snapshots.Num() < Limit) Snapshots.Add(MakeShared<FJsonValueObject>(ActorSnapshot(Actor)));
        else bActorsTruncated = true;
    }
    Result->SetArrayField(TEXT("actors"), Snapshots);
    Result->SetArrayField(TEXT("selected_actor_paths"), Selected);
    Result->SetBoolField(TEXT("actors_truncated"), bActorsTruncated || bScanIncomplete);
    Result->SetBoolField(TEXT("selection_truncated"), bSelectionTruncated || bScanIncomplete);
    Result->SetBoolField(TEXT("actor_scan_incomplete"), bScanIncomplete);
    Result->SetNumberField(TEXT("examined_actors"), Actors.Num());

    TArray<UPackage*> DirtyWorlds, DirtyContent;
    FEditorFileUtils::GetDirtyWorldPackages(DirtyWorlds);
    FEditorFileUtils::GetDirtyContentPackages(DirtyContent);
    TSet<FString> UniquePackages;
    for (UPackage* Package : DirtyWorlds) if (IsValid(Package)) UniquePackages.Add(Package->GetName());
    for (UPackage* Package : DirtyContent) if (IsValid(Package)) UniquePackages.Add(Package->GetName());
    TArray<FString> PackageNames = UniquePackages.Array();
    PackageNames.Sort();
    TArray<TSharedPtr<FJsonValue>> DirtyPackages;
    for (int32 I = 0; I < FMath::Min(Limit, PackageNames.Num()); ++I) DirtyPackages.Add(MakeShared<FJsonValueString>(PackageNames[I]));
    Result->SetArrayField(TEXT("dirty_packages"), DirtyPackages);
    Result->SetBoolField(TEXT("dirty_packages_truncated"), PackageNames.Num() > Limit);
    Result->SetNumberField(TEXT("dirty_package_count"), PackageNames.Num());
    return JevInspection::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::AssetDetails(const TSharedPtr<FJsonObject>& Params) const
{
    FString Path;
    if (!JevInspection::OnlyFields(Params, {TEXT("path")}) || !Params->HasTypedField<EJson::String>(TEXT("path")) || !Params->TryGetStringField(TEXT("path"), Path) || Path.Len() > 512 || !Path.Contains(TEXT(".")) || !FPackageName::IsValidObjectPath(Path) || Path.Contains(TEXT(":")) || Path.Contains(TEXT("..")) || (!Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Engine/"))))
        return Error(TEXT("bad_request"), TEXT("asset_details requires an exact /Game or /Engine asset object path, without subobjects or traversal, at most 512 characters."));
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
    if (!Asset.IsValid()) return Error(TEXT("asset_not_found"), TEXT("The exact asset is not present in the current asset registry."));
    if (Asset.IsRedirector()) return Error(TEXT("asset_unsupported"), TEXT("Resolve the asset redirector and request the destination's exact path."));
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Asset.GetSoftObjectPath().ToString());
    Result->SetStringField(TEXT("name"), Asset.AssetName.ToString());
    Result->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
    Result->SetBoolField(TEXT("loaded"), false);
    Result->SetBoolField(TEXT("registry_loading"), Registry.IsLoadingAssets());
    if (Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()) return JevInspection::Success(Result);

    // Only the explicitly selected static mesh is loaded; Unreal may load its dependencies.
    UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
    if (!IsValid(Mesh)) return Error(TEXT("asset_unavailable"), TEXT("The selected static mesh could not be loaded."));
    Result->SetBoolField(TEXT("loaded"), true);
    auto Details = MakeShared<FJsonObject>();
    auto Bounds = MakeShared<FJsonObject>();
    const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
    Bounds->SetArrayField(TEXT("origin"), JevInspection::Vector(MeshBounds.Origin));
    Bounds->SetArrayField(TEXT("box_extent"), JevInspection::Vector(MeshBounds.BoxExtent));
    Bounds->SetArrayField(TEXT("size"), JevInspection::Vector(MeshBounds.BoxExtent * 2));
    Bounds->SetNumberField(TEXT("sphere_radius"), MeshBounds.SphereRadius);
    Details->SetObjectField(TEXT("bounds_cm"), Bounds);
    const auto& Materials = Mesh->GetStaticMaterials();
    TArray<TSharedPtr<FJsonValue>> Slots;
    for (int32 I = 0; I < FMath::Min(Materials.Num(), JevInspection::MaxMaterials); ++I)
    {
        auto Slot = MakeShared<FJsonObject>();
        Slot->SetNumberField(TEXT("index"), I);
        Slot->SetStringField(TEXT("name"), Materials[I].MaterialSlotName.ToString());
        Slot->SetStringField(TEXT("material_path"), IsValid(Materials[I].MaterialInterface.Get()) ? Materials[I].MaterialInterface->GetPathName() : TEXT(""));
        Slot->SetBoolField(TEXT("missing"), !IsValid(Materials[I].MaterialInterface.Get()));
        Slots.Add(MakeShared<FJsonValueObject>(Slot));
    }
    Details->SetArrayField(TEXT("material_slots"), Slots);
    Details->SetNumberField(TEXT("material_slot_count"), Materials.Num());
    Details->SetBoolField(TEXT("materials_truncated"), Materials.Num() > JevInspection::MaxMaterials);
    const int32 LodCount = Mesh->GetNumLODs();
    TArray<TSharedPtr<FJsonValue>> Lods;
    for (int32 I = 0; I < FMath::Min(LodCount, 16); ++I)
    {
        auto Lod = MakeShared<FJsonObject>();
        Lod->SetNumberField(TEXT("index"), I);
        Lod->SetNumberField(TEXT("vertices"), Mesh->GetNumVertices(I));
        Lod->SetNumberField(TEXT("triangles"), Mesh->GetNumTriangles(I));
        Lods.Add(MakeShared<FJsonValueObject>(Lod));
    }
    Details->SetArrayField(TEXT("lods"), Lods);
    Details->SetNumberField(TEXT("lod_count"), LodCount);
    Details->SetBoolField(TEXT("lods_truncated"), LodCount > 16);
    auto Collision = MakeShared<FJsonObject>();
    const UBodySetup* Body = Mesh->GetBodySetup();
    Collision->SetBoolField(TEXT("has_body_setup"), Body != nullptr);
    Collision->SetNumberField(TEXT("simple_shape_count"), Body ? Body->AggGeom.GetElementCount() : 0);
    Collision->SetStringField(TEXT("trace_flag"), Body ? JevInspection::TraceFlag(Body->GetCollisionTraceFlag()) : TEXT("none"));
    Details->SetObjectField(TEXT("collision"), Collision);
    Result->SetObjectField(TEXT("static_mesh"), Details);
    return JevInspection::Success(Result);
}

TSharedPtr<FJsonObject> FJevEditorBridge::ResolveStaticMeshAsset(const FString& Path, UStaticMesh*& OutMesh) const
{
    OutMesh = nullptr;
    if (Path.Len() > 512 || !Path.Contains(TEXT(".")) || !FPackageName::IsValidObjectPath(Path) || Path.Contains(TEXT(":")) || Path.Contains(TEXT("..")) || (!Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Engine/"))))
        return Error(TEXT("bad_request"), TEXT("asset_path must identify an exact /Game or /Engine asset object, without subobjects or traversal, at most 512 characters."));
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
    if (!Asset.IsValid()) return Error(TEXT("asset_not_found"), TEXT("The selected mesh is not present in the current asset registry."));
    if (Asset.IsRedirector() || Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName())
        return Error(TEXT("asset_unsupported"), TEXT("Placement accepts only exact native StaticMesh assets, not redirectors, Blueprints, or other asset classes."));
    OutMesh = Cast<UStaticMesh>(Asset.GetAsset());
    if (!IsValid(OutMesh) || OutMesh->GetClass() != UStaticMesh::StaticClass() || OutMesh->GetPathName() != Path)
        return Error(TEXT("asset_unavailable"), TEXT("The selected static mesh could not be resolved to its exact asset identity."));
    return nullptr;
}

TSharedPtr<FJsonObject> FJevEditorBridge::ResolveMaterialAsset(const FString& Path, UMaterialInterface*& OutMaterial) const
{
    OutMaterial = nullptr;
    if (Path.Len() > 512 || !Path.Contains(TEXT(".")) || !FPackageName::IsValidObjectPath(Path) || Path.Contains(TEXT(":")) || Path.Contains(TEXT("..")) || (!Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Engine/"))))
        return Error(TEXT("bad_request"), TEXT("material_path must identify an exact /Game or /Engine asset object, without subobjects or traversal, at most 512 characters."));
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
    if (!Asset.IsValid()) return Error(TEXT("asset_not_found"), TEXT("The selected material is not present in the current asset registry."));
    if (Asset.IsRedirector() || (Asset.AssetClassPath != UMaterial::StaticClass()->GetClassPathName() && Asset.AssetClassPath != UMaterialInstanceConstant::StaticClass()->GetClassPathName()))
        return Error(TEXT("asset_unsupported"), TEXT("Material edits accept only native Material or MaterialInstanceConstant assets, not redirectors or other classes."));
    OutMaterial = Cast<UMaterialInterface>(Asset.GetAsset());
    if (!IsValid(OutMaterial) || (OutMaterial->GetClass() != UMaterial::StaticClass() && OutMaterial->GetClass() != UMaterialInstanceConstant::StaticClass()) || OutMaterial->GetPathName() != Path)
        return Error(TEXT("asset_unavailable"), TEXT("The selected material could not be resolved to its exact asset identity."));
    return nullptr;
}

TSharedRef<FJsonObject> FJevEditorBridge::ActorDetails(UWorld* World, const TSharedPtr<FJsonObject>& Params) const
{
    const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
    if (!JevInspection::OnlyFields(Params, {TEXT("actor_paths")}) || !Params->TryGetArrayField(TEXT("actor_paths"), Paths) || Paths->IsEmpty() || Paths->Num() > 20)
        return Error(TEXT("bad_request"), TEXT("actor_details requires 1 to 20 unique exact actor paths."));
    TSet<FString> Seen;
    TArray<AActor*> Actors;
    for (const auto& Value : *Paths)
    {
        FString Path;
        if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Path) || Path.IsEmpty() || Path.Len() > 1024 || Seen.Contains(Path))
            return Error(TEXT("bad_request"), TEXT("actor_paths must contain unique, nonempty strings of at most 1024 characters."));
        Seen.Add(Path);
        AActor* Actor = FindActor(World, Path);
        if (!IsValid(Actor)) return Error(TEXT("actor_not_found"), TEXT("Every requested actor must exist in the current editor world; no partial snapshot was returned."));
        if (Actor->GetActorTransform().ContainsNaN()) return Error(TEXT("actor_bounds_unavailable"), TEXT("A requested actor has non-finite transform values and cannot be represented safely."));
        Actors.Add(Actor);
    }
    TArray<TSharedPtr<FJsonValue>> Details;
    for (AActor* Actor : Actors)
    {
        auto Detail = ActorSnapshot(Actor);
        const FBox Bounds = Actor->GetComponentsBoundingBox(true, false);
        const bool bBoundsAvailable = Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN() && !Bounds.GetCenter().ContainsNaN() && !Bounds.GetSize().ContainsNaN() && !Bounds.GetExtent().IsNearlyZero();
        Detail->SetBoolField(TEXT("bounds_available"), bBoundsAvailable);
        if (bBoundsAvailable)
        {
            auto Box = MakeShared<FJsonObject>();
            Box->SetArrayField(TEXT("min"), JevInspection::Vector(Bounds.Min));
            Box->SetArrayField(TEXT("max"), JevInspection::Vector(Bounds.Max));
            Box->SetArrayField(TEXT("center"), JevInspection::Vector(Bounds.GetCenter()));
            Box->SetArrayField(TEXT("size"), JevInspection::Vector(Bounds.GetSize()));
            Detail->SetObjectField(TEXT("bounds_cm"), Box);
        }
        else Detail->SetField(TEXT("bounds_cm"), MakeShared<FJsonValueNull>());
        const TArray<FString> Blockers = ActorEditBlockers(Actor);
        TArray<TSharedPtr<FJsonValue>> Reasons;
        for (const FString& Blocker : Blockers) Reasons.Add(MakeShared<FJsonValueString>(Blocker));
        Detail->SetArrayField(TEXT("edit_blockers"), Reasons);
        Detail->SetBoolField(TEXT("editable"), Blockers.IsEmpty());
        Details.Add(MakeShared<FJsonValueObject>(Detail));
    }
    auto Result = StatusSnapshot(World);
    Result->SetArrayField(TEXT("actors"), Details);
    Result->SetBoolField(TEXT("truncated"), false);
    return JevInspection::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Validate(UWorld* World, const TSharedPtr<FJsonObject>& Params) const
{
    FString Query;
    int32 Limit = 100;
    if (!JevInspection::QueryLimit(Params, Query, Limit)) return Error(TEXT("bad_request"), TEXT("validate accepts only query (string, max 200) and limit (integer, 1 to 200)."));
    bool bIncomplete = false, bWarningsTruncated = false;
    const auto Actors = JevInspection::Candidates(World, bIncomplete);
    TArray<TSharedPtr<FJsonValue>> Warnings;
    auto Warn = [&](const TCHAR* Code, AActor* Actor, const TCHAR* Message, UStaticMeshComponent* Component = nullptr)
    {
        if (Warnings.Num() >= JevInspection::MaxWarnings) { bWarningsTruncated = true; return; }
        auto Warning = MakeShared<FJsonObject>();
        Warning->SetStringField(TEXT("code"), Code);
        Warning->SetStringField(TEXT("severity"), TEXT("warning"));
        Warning->SetStringField(TEXT("actor_path"), Actor->GetPathName());
        Warning->SetStringField(TEXT("message"), Message);
        if (Component) Warning->SetStringField(TEXT("component_path"), Component->GetPathName());
        Warnings.Add(MakeShared<FJsonValueObject>(Warning));
    };
    int32 Scanned = 0;
    for (AActor* Actor : Actors)
    {
        if (!JevInspection::Matches(Actor, Query)) continue;
        if (Scanned >= Limit) { bIncomplete = true; break; }
        ++Scanned;
        const FVector Scale = Actor->GetActorScale3D();
        if (Scale.ContainsNaN()) Warn(TEXT("non_finite_scale"), Actor, TEXT("Actor scale contains a non-finite value."));
        else if (FMath::Abs(Scale.X) <= UE_SMALL_NUMBER || FMath::Abs(Scale.Y) <= UE_SMALL_NUMBER || FMath::Abs(Scale.Z) <= UE_SMALL_NUMBER) Warn(TEXT("zero_scale"), Actor, TEXT("Actor scale contains a zero or near-zero axis."));
        else if (Scale.X < 0 || Scale.Y < 0 || Scale.Z < 0) Warn(TEXT("negative_scale"), Actor, TEXT("Negative actor scale can affect winding and collision; review whether mirroring is intentional."));
        TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
        if (Components.Num() > JevInspection::MaxComponents) bIncomplete = true;
        for (int32 I = 0; I < FMath::Min(Components.Num(), JevInspection::MaxComponents); ++I)
        {
            UStaticMeshComponent* Component = Components[I];
            if (!IsValid(Component)) continue;
            UStaticMesh* Mesh = Component->GetStaticMesh();
            if (!IsValid(Mesh)) { Warn(TEXT("missing_mesh"), Actor, TEXT("Static mesh component has no assigned mesh."), Component); continue; }
            const int32 MaterialCount = Component->GetNumMaterials();
            if (MaterialCount > JevInspection::MaxMaterials) bIncomplete = true;
            bool bMissingMaterial = MaterialCount == 0;
            for (int32 Material = 0; Material < FMath::Min(MaterialCount, JevInspection::MaxMaterials); ++Material)
                bMissingMaterial |= Component->GetEditorMaterial(Material) == nullptr;
            if (bMissingMaterial) Warn(TEXT("missing_material"), Actor, TEXT("One or more material slots have no assigned material."), Component);
            if (Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
                Warn(TEXT("collision_disabled"), Actor, TEXT("Collision is disabled; this may be intentional for decorative geometry."), Component);
            else
            {
                const UBodySetup* Body = Mesh->GetBodySetup();
                if (!Body || (Body->AggGeom.GetElementCount() == 0 && Body->GetCollisionTraceFlag() != CTF_UseComplexAsSimple))
                    Warn(TEXT("missing_simple_collision"), Actor, TEXT("No simple collision shapes are available and complex-as-simple is not enabled."), Component);
            }
        }
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetNumberField(TEXT("scanned_actors"), Scanned);
    Result->SetNumberField(TEXT("examined_actors"), Actors.Num());
    Result->SetBoolField(TEXT("scan_incomplete"), bIncomplete || bWarningsTruncated);
    Result->SetBoolField(TEXT("warnings_truncated"), bWarningsTruncated);
    Result->SetBoolField(TEXT("query_filtered"), !Query.IsEmpty());
    Result->SetStringField(TEXT("scope"), TEXT("Loaded editor-world actors and static mesh components only; warnings are not build or gameplay acceptance."));
    Result->SetStringField(TEXT("revision"), Revision(World));
    return JevInspection::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Capture(UWorld* World, const TSharedPtr<FJsonObject>& Params) const
{
    int32 MaximumDimension = 1024;
    if (!JevInspection::OnlyFields(Params, {TEXT("max_dimension")}) || !JevInspection::Integer(Params, TEXT("max_dimension"), 64, 1024, MaximumDimension))
        return Error(TEXT("bad_request"), TEXT("capture accepts only max_dimension (integer, 64 to 1024)."));
    FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
    if (!Client || !GEditor->GetLevelViewportClients().Contains(Client) || Client->GetWorld() != World || !Client->Viewport)
        return Error(TEXT("viewport_unavailable"), TEXT("No current level-editor viewport is available for this editor world."));
    FViewport* Viewport = Client->Viewport;
    const FIntPoint SourceSize = Viewport->GetSizeXY();
    if (SourceSize.X <= 0 || SourceSize.Y <= 0)
        return Error(TEXT("viewport_unavailable"), TEXT("The current editor viewport has no drawable area."));
    if (static_cast<int64>(SourceSize.X) * SourceSize.Y > 16 * 1024 * 1024)
        return Error(TEXT("capture_too_large"), TEXT("Reduce the editor viewport below 16 million pixels before capture."));
    if (FScreenshotRequest::IsScreenshotRequested() || GIsHighResScreenshot || GIsDumpingMovie)
        return Error(TEXT("capture_failed"), TEXT("Wait for the editor's existing screenshot or movie capture to finish before requesting a bridge capture."));
    // FocusViewportOnBox only invalidates the viewport. A background editor may not
    // redraw before the next HTTP call, and ReadPixels alone returns that stale buffer.
    // Push pending component transforms, draw this exact editor viewport without
    // presenting a desktop frame, then wait for its render commands before readback.
    World->SendAllEndOfFrameUpdates();
    Viewport->Draw(false);
    FlushRenderingCommands();
    TArray<FColor> Pixels;
    if (!GetViewportScreenShot(Viewport, Pixels) || Pixels.Num() != SourceSize.X * SourceSize.Y)
        return Error(TEXT("capture_failed"), TEXT("The renderer could not read the current editor viewport."));
    for (FColor& Pixel : Pixels) Pixel.A = 255;
    const double Ratio = FMath::Min(1.0, static_cast<double>(MaximumDimension) / FMath::Max(SourceSize.X, SourceSize.Y));
    int32 Width = FMath::Max(1, FMath::RoundToInt(SourceSize.X * Ratio));
    int32 Height = FMath::Max(1, FMath::RoundToInt(SourceSize.Y * Ratio));
    TArray64<uint8> Png;
    for (int32 Attempt = 0; Attempt < 6; ++Attempt)
    {
        TArray<FColor> Resized;
        FImageUtils::ImageResize(SourceSize.X, SourceSize.Y, Pixels, Width, Height, Resized, true);
        Png.Reset();
        FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Resized.GetData(), Resized.Num()), Png);
        if (Png.Num() > 0 && Png.Num() <= JevInspection::MaxPngBytes) break;
        Width = FMath::Max(1, Width * 3 / 4);
        Height = FMath::Max(1, Height * 3 / 4);
    }
    if (Png.IsEmpty()) return Error(TEXT("capture_failed"), TEXT("The renderer image could not be encoded as PNG."));
    if (Png.Num() > JevInspection::MaxPngBytes) return Error(TEXT("capture_too_large"), TEXT("The compressed image exceeds the bounded bridge response."));
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("mime_type"), TEXT("image/png"));
    Result->SetStringField(TEXT("data"), FBase64::Encode(Png.GetData(), static_cast<uint32>(Png.Num())));
    Result->SetNumberField(TEXT("width"), Width);
    Result->SetNumberField(TEXT("height"), Height);
    Result->SetStringField(TEXT("source"), TEXT("editor_viewport"));
    Result->SetStringField(TEXT("world_path"), World->GetPathName());
    Result->SetStringField(TEXT("revision"), Revision(World));
    Result->SetArrayField(TEXT("current_camera_location"), JevInspection::Vector(Client->GetViewLocation()));
    const FRotator CaptureRotation = Client->GetViewRotation();
    Result->SetArrayField(TEXT("current_camera_rotation"), JevInspection::Vector(FVector(CaptureRotation.Pitch, CaptureRotation.Yaw, CaptureRotation.Roll)));
    return JevInspection::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Frame(UWorld* World, const TSharedPtr<FJsonObject>& Params) const
{
    const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
    double Padding = 1.2;
    FString View = TEXT("current");
    if (!JevInspection::OnlyFields(Params, {TEXT("actor_paths"), TEXT("padding"), TEXT("view")}) || !Params->TryGetArrayField(TEXT("actor_paths"), Paths) || Paths->Num() < 1 || Paths->Num() > 20)
        return Error(TEXT("bad_request"), TEXT("frame requires 1 to 20 unique exact actor paths and optional padding/view."));
    if (Params->HasField(TEXT("padding")) && (!Params->HasTypedField<EJson::Number>(TEXT("padding")) || !Params->TryGetNumberField(TEXT("padding"), Padding) || !FMath::IsFinite(Padding) || Padding < 1 || Padding > 4))
        return Error(TEXT("bad_request"), TEXT("padding must be a finite number from 1 to 4."));
    if (Params->HasField(TEXT("view")) && (!Params->HasTypedField<EJson::String>(TEXT("view")) || !Params->TryGetStringField(TEXT("view"), View)))
        return Error(TEXT("bad_request"), TEXT("view must be a named camera orientation string."));
    if (View != TEXT("current") && View != TEXT("isometric") && View != TEXT("top") && View != TEXT("front") && View != TEXT("right"))
        return Error(TEXT("bad_request"), TEXT("view must be current, isometric, top, front, or right."));

    // Resolve and validate every input before touching viewport state.
    TSet<FString> UniquePaths;
    TArray<FString> ExactPaths;
    for (const auto& Value : *Paths)
    {
        FString Path;
        if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Path) || Path.IsEmpty() || Path.Len() > 1024 || UniquePaths.Contains(Path))
            return Error(TEXT("bad_request"), TEXT("Actor paths must be unique, nonempty strings of at most 1024 characters."));
        UniquePaths.Add(Path);
        ExactPaths.Add(MoveTemp(Path));
    }
    FBox ActorBounds(ForceInit);
    for (const FString& Path : ExactPaths)
    {
        AActor* Actor = FindActor(World, Path);
        if (!IsValid(Actor)) return Error(TEXT("actor_not_found"), TEXT("Every frame target must be an existing actor in the current editor world."));
        const FBox Bounds = Actor->GetComponentsBoundingBox(true, false);
        if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || Bounds.GetExtent().IsNearlyZero())
            return Error(TEXT("actor_bounds_unavailable"), TEXT("Every frame target requires finite, nonzero component bounds in the editor world."));
        ActorBounds += Bounds;
    }
    const FVector Center = ActorBounds.GetCenter();
    const FVector Extent = ActorBounds.GetExtent();
    const FVector PaddedExtent = Extent * Padding;
    // UE's focus calculation uses a float radius internally; fail before conversion overflows.
    if (Center.ContainsNaN() || PaddedExtent.ContainsNaN() || PaddedExtent.GetAbsMax() > 1.0e12)
        return Error(TEXT("actor_bounds_unavailable"), TEXT("The combined actor bounds exceed the supported viewport framing range."));

    FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
    if (!Client || !GEditor->GetLevelViewportClients().Contains(Client) || Client->GetWorld() != World || !Client->Viewport || Client->Viewport->GetSizeXY().X <= 0 || Client->Viewport->GetSizeXY().Y <= 0)
        return Error(TEXT("viewport_unavailable"), TEXT("No current drawable level-editor viewport is available for this editor world."));
    if (Client->IsAnyActorLocked())
        return Error(TEXT("viewport_locked"), TEXT("Stop piloting or locking the viewport to an actor before framing; actor state will not be changed."));
    if (View != TEXT("current") && !Client->IsPerspective())
        return Error(TEXT("viewport_unavailable"), TEXT("Camera orientation presets require an existing perspective viewport; projection mode is never changed."));
    if (!Client->IsOrtho() && (!FMath::IsFinite(Client->ViewFOV) || Client->ViewFOV <= 1 || Client->ViewFOV >= 179))
        return Error(TEXT("viewport_unavailable"), TEXT("The current viewport requires a valid perspective field of view before framing."));

    if (View != TEXT("current"))
    {
        // Focus also exits orbit mode; exit first so its conversion cannot replace
        // the requested rotation. All possible rejection paths precede this point.
        Client->ToggleOrbitCamera(false);
        if (View == TEXT("isometric")) Client->SetViewRotation(FRotator(-35.2643897, 45, 0));
        else if (View == TEXT("top")) Client->SetViewRotation(FRotator(-90, 0, 0));
        else if (View == TEXT("front")) Client->SetViewRotation(FRotator(0, -90, 0));
        else Client->SetViewRotation(FRotator(0, 0, 0));
    }
    Client->FocusViewportOnBox(FBox(Center - PaddedExtent, Center + PaddedExtent), true);
    auto Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> FramedPaths;
    for (const FString& Path : ExactPaths) FramedPaths.Add(MakeShared<FJsonValueString>(Path));
    Result->SetArrayField(TEXT("framed_actor_paths"), FramedPaths);
    auto Bounds = MakeShared<FJsonObject>();
    Bounds->SetArrayField(TEXT("origin"), JevInspection::Vector(Center));
    Bounds->SetArrayField(TEXT("box_extent"), JevInspection::Vector(Extent));
    Bounds->SetArrayField(TEXT("size"), JevInspection::Vector(Extent * 2));
    Result->SetObjectField(TEXT("bounds_cm"), Bounds);
    Result->SetNumberField(TEXT("padding"), Padding);
    Result->SetStringField(TEXT("view"), View);
    Result->SetArrayField(TEXT("current_camera_location"), JevInspection::Vector(Client->GetViewLocation()));
    const FRotator Rotation = Client->GetViewRotation();
    Result->SetArrayField(TEXT("current_camera_rotation"), JevInspection::Vector(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
    Result->SetStringField(TEXT("world_path"), World->GetPathName());
    Result->SetStringField(TEXT("revision"), Revision(World));
    Result->SetStringField(TEXT("source"), TEXT("editor_viewport"));
    return JevInspection::Success(Result);
}
