#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "WidgetBlueprint.h"
#include "UObject/UnrealType.h"

namespace JevWorkflow
{
TSharedRef<FJsonObject> AssetDiagnosis(const TSharedPtr<FJsonObject>& P)
{
    FString Path; double Depth = 0;
    if (!Only(P, {TEXT("kind"), TEXT("target_path"), TEXT("dependency_depth")}) || !Text(P, TEXT("target_path"), Path) || !Number(P, TEXT("dependency_depth"), Depth, 1, 3) || Depth != FMath::FloorToDouble(Depth)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply exact asset and dependency depth 1..3."));
    UObject* O = Loaded(Path);
    if (!O) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open the exact asset first."));
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("asset_path"), Path); R->SetStringField(TEXT("asset_class"), O->GetClass()->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Issues, Sources, Edges;
    auto Issue = [&Issues](const TCHAR* Code) { Issues.Add(MakeShared<FJsonValueString>(Code)); };
    UAssetImportData* Import = nullptr;
    if (auto* Mesh = Cast<UStaticMesh>(O))
    {
        Import = Mesh->GetAssetImportData();
        R->SetArrayField(TEXT("bounds_size_cm"), Vector(Mesh->GetBounds().BoxExtent * 2)); R->SetArrayField(TEXT("bounds_center_cm"), Vector(Mesh->GetBounds().Origin));
        const auto* Render = Mesh->GetRenderData();
        R->SetNumberField(TEXT("lod_count"), Render ? Render->LODResources.Num() : 0);
        TArray<TSharedPtr<FJsonValue>> Lods;
        if (Render) for (int32 I = 0; I < FMath::Min(8, Render->LODResources.Num()); ++I)
        {
            auto Row = MakeShared<FJsonObject>(); Row->SetNumberField(TEXT("lod"), I); Row->SetNumberField(TEXT("triangles"), Render->LODResources[I].GetNumTriangles()); Row->SetNumberField(TEXT("vertices"), Render->LODResources[I].GetNumVertices()); Lods.Add(MakeShared<FJsonValueObject>(Row));
        }
        R->SetArrayField(TEXT("lods"), Lods); R->SetBoolField(TEXT("lods_truncated"), Render && Render->LODResources.Num() > 8);
        const auto* Body = Mesh->GetBodySetup(); const int32 Shapes = Body ? Body->AggGeom.GetElementCount() : 0;
        R->SetNumberField(TEXT("simple_collision_shapes"), Shapes); R->SetNumberField(TEXT("collision_trace_flag"), Body ? static_cast<int32>(Body->CollisionTraceFlag) : -1);
        if (Shapes == 0) Issue(TEXT("no_simple_collision_shapes"));
        if (!Render || Render->LODResources.IsEmpty()) Issue(TEXT("render_lods_unavailable"));
        int32 Missing = 0; for (const auto& Slot : Mesh->GetStaticMaterials()) if (!Slot.MaterialInterface) ++Missing;
        R->SetNumberField(TEXT("unassigned_material_slots"), Missing); if (Missing) Issue(TEXT("unassigned_material_slots"));
    }
    else if (auto* Skeletal = Cast<USkeletalMesh>(O)) Import = Skeletal->GetAssetImportData();
    if (Import) for (int32 I = 0; I < FMath::Min(8, Import->SourceData.SourceFiles.Num()); ++I)
    {
        const auto& Source = Import->SourceData.SourceFiles[I]; auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("basename"), FPaths::GetCleanFilename(Source.RelativeFilename).Left(256)); Row->SetStringField(TEXT("recorded_hash"), LexToString(Source.FileHash)); Sources.Add(MakeShared<FJsonValueObject>(Row));
    }
    R->SetArrayField(TEXT("recorded_sources"), Sources); R->SetBoolField(TEXT("sources_truncated"), Import && Import->SourceData.SourceFiles.Num() > 8);
    if (Sources.IsEmpty()) Issue(TEXT("no_recorded_import_source"));
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<TPair<FName, int32>> Queue = {{FName(*FPackageName::ObjectPathToPackageName(Path)), 0}};
    TSet<FName> Seen; bool Truncated = false;
    for (int32 Q = 0; Q < Queue.Num() && Q < 128; ++Q)
    {
        const auto Entry = Queue[Q]; if (Seen.Contains(Entry.Key)) continue; Seen.Add(Entry.Key);
        TArray<FName> Dependencies; Registry.GetDependencies(Entry.Key, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
        Dependencies.Sort(FNameLexicalLess());
        for (FName Dep : Dependencies)
        {
            if (Edges.Num() >= 128) { Truncated = true; break; }
            auto Edge = MakeShared<FJsonObject>(); Edge->SetStringField(TEXT("from"), Entry.Key.ToString().Left(1024)); Edge->SetStringField(TEXT("to"), Dep.ToString().Left(1024));
            TArray<FAssetData> Assets; Registry.GetAssetsByPackageName(Dep, Assets, true);
            Edge->SetBoolField(TEXT("registry_package_present"), !Assets.IsEmpty()); Edge->SetBoolField(TEXT("script_package"), Dep.ToString().StartsWith(TEXT("/Script/"))); Edge->SetNumberField(TEXT("depth"), Entry.Value + 1); Edges.Add(MakeShared<FJsonValueObject>(Edge));
            if (Entry.Value + 1 < Depth && !Seen.Contains(Dep) && !Dep.ToString().StartsWith(TEXT("/Script/")))
            {
                if (Queue.Num() < 128) Queue.Add({Dep, Entry.Value + 1}); else Truncated = true;
            }
        }
        if (Truncated && Edges.Num() == 128) break;
    }
    R->SetArrayField(TEXT("dependencies"), Edges); R->SetBoolField(TEXT("dependencies_truncated"), Truncated); R->SetArrayField(TEXT("findings"), Issues);
    R->SetStringField(TEXT("scope"), TEXT("Stored imports, loaded mesh data and bounded package-registry traversal. An absent registry entry needs investigation and is not proof of a broken runtime reference. Source file availability, import units, missing DCC textures, Nanite cost and pivot correctness are not inferred."));
    return Success(R);
}

TSharedRef<FJsonObject> Rig(const TSharedPtr<FJsonObject>& P)
{
    FString Path, AnimationPath; const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
    if (!Only(P, {TEXT("kind"), TEXT("target_path"), TEXT("animation_path"), TEXT("required_bones")}) || !Text(P, TEXT("target_path"), Path) || !P->TryGetArrayField(TEXT("required_bones"), Required) || Required->Num() > 64 || (P->HasField(TEXT("animation_path")) && !Text(P, TEXT("animation_path"), AnimationPath))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a mesh/skeleton, up to 64 required bone names and optional sequence."));
    auto* O = Loaded(Path); auto* Mesh = Cast<USkeletalMesh>(O); auto* Skeleton = Mesh ? Mesh->GetSkeleton() : Cast<USkeleton>(O);
    if (!Skeleton || (O->GetClass() != USkeletalMesh::StaticClass() && O->GetClass() != USkeleton::StaticClass())) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("Open a native skeletal mesh or skeleton with a skeleton assigned."));
    const auto& Ref = Mesh ? Mesh->GetRefSkeleton() : Skeleton->GetReferenceSkeleton();
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("asset_path"), Path); R->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName()); R->SetNumberField(TEXT("bone_count"), Ref.GetNum());
    TArray<TSharedPtr<FJsonValue>> Bones, Missing;
    for (int32 I = 0; I < FMath::Min(512, Ref.GetNum()); ++I)
    {
        auto B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("name"), Ref.GetBoneName(I).ToString().Left(128)); B->SetNumberField(TEXT("index"), I); B->SetNumberField(TEXT("parent_index"), Ref.GetParentIndex(I)); Bones.Add(MakeShared<FJsonValueObject>(B));
    }
    for (const auto& Value : *Required)
    {
        FString Name; if (!Value || Value->Type != EJson::String || !Value->TryGetString(Name) || Name.IsEmpty() || Name.Len() > 128) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Bone names must be bounded strings."));
        if (Ref.FindBoneIndex(FName(*Name)) == INDEX_NONE) Missing.Add(MakeShared<FJsonValueString>(Name));
    }
    R->SetArrayField(TEXT("bones"), Bones); R->SetBoolField(TEXT("bones_truncated"), Ref.GetNum() > 512); R->SetArrayField(TEXT("missing_required_bones"), Missing); R->SetBoolField(TEXT("required_bones_present"), Missing.IsEmpty());
    if (!AnimationPath.IsEmpty())
    {
        auto* Sequence = Cast<UAnimSequence>(Loaded(AnimationPath));
        if (!Sequence || Sequence->GetClass() != UAnimSequence::StaticClass()) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open the native animation sequence first."));
        R->SetStringField(TEXT("animation_path"), AnimationPath); R->SetStringField(TEXT("animation_skeleton_path"), GetPathNameSafe(Sequence->GetSkeleton())); R->SetBoolField(TEXT("exact_skeleton_match"), Skeleton == Sequence->GetSkeleton());
        R->SetBoolField(TEXT("root_motion_enabled"), Sequence->bEnableRootMotion); R->SetNumberField(TEXT("duration_seconds"), Sequence->GetPlayLength());
    }
    R->SetStringField(TEXT("scope"), TEXT("Stored bone hierarchy, exact skeleton identity, requested bone presence and root-motion flag. A different skeleton may work through an approved retargeter; playback, extracted motion and retarget quality require separate acceptance.")); return Success(R);
}

TSharedRef<FJsonObject> Widgets(const TSharedPtr<FJsonObject>& P)
{
    FString Path;
    if (!Only(P, {TEXT("kind"), TEXT("target_path")}) || !Text(P, TEXT("target_path"), Path)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply an exact Widget Blueprint path."));
    auto* BP = Cast<UWidgetBlueprint>(Loaded(Path));
    if (!BP || BP->GetClass() != UWidgetBlueprint::StaticClass() || !BP->WidgetTree) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open a native Widget Blueprint with a widget tree."));
    const auto* EnabledProperty = FindFProperty<FBoolProperty>(UWidget::StaticClass(), TEXT("bIsEnabled"));
    const auto* VisibilityProperty = FindFProperty<FEnumProperty>(UWidget::StaticClass(), TEXT("Visibility"));
    const auto* TextProperty = FindFProperty<FTextProperty>(UTextBlock::StaticClass(), TEXT("Text"));
    const auto* LayoutProperty = FindFProperty<FStructProperty>(UCanvasPanelSlot::StaticClass(), TEXT("LayoutData"));
    if (!EnabledProperty || !VisibilityProperty || !TextProperty || !LayoutProperty || LayoutProperty->Struct != FAnchorData::StaticStruct()) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("This engine's stored widget property layout is not supported."));
    // Traverse only native panel children; do not create widgets or invoke bound getters.
    TArray<UWidget*> Queue; if (BP->WidgetTree->RootWidget) Queue.Add(BP->WidgetTree->RootWidget);
    TSet<UWidget*> Seen; TArray<TSharedPtr<FJsonValue>> Rows; bool Truncated = false;
    for (int32 I = 0; I < Queue.Num() && I < 256; ++I)
    {
        UWidget* W = Queue[I]; if (!IsValid(W) || Seen.Contains(W)) continue; Seen.Add(W);
        auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), W->GetName().Left(128)); Row->SetStringField(TEXT("class"), W->GetClass()->GetPathName()); Row->SetStringField(TEXT("parent"), GetNameSafe(W->GetParent()).Left(128));
        // These three public getters consult cached Slate state when a designer is
        // open. Read fixed serialized property offsets instead; never call a
        // getter supplied by a request, property binding or custom widget class.
        Row->SetNumberField(TEXT("stored_visibility"), VisibilityProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(VisibilityProperty->ContainerPtrToValuePtr<void>(W)));
        Row->SetBoolField(TEXT("stored_enabled"), EnabledProperty->GetPropertyValue(EnabledProperty->ContainerPtrToValuePtr<void>(W)));
        if (W->GetClass() == UButton::StaticClass()) Row->SetBoolField(TEXT("stored_focusable"), CastChecked<UButton>(W)->GetIsFocusable());
        if (W->GetClass() == UTextBlock::StaticClass())
        {
            auto* T = CastChecked<UTextBlock>(W); const FString Stored = TextProperty->ContainerPtrToValuePtr<FText>(T)->ToString();
            Row->SetStringField(TEXT("stored_text"), Stored.Left(512)); Row->SetBoolField(TEXT("text_truncated"), Stored.Len() > 512); Row->SetBoolField(TEXT("text_bound"), T->TextDelegate.IsBound());
            Row->SetNumberField(TEXT("overflow_policy"), static_cast<int32>(T->GetTextOverflowPolicy()));
        }
        if (auto* Slot = Cast<UCanvasPanelSlot>(W->Slot))
        {
            const auto& Layout = *LayoutProperty->ContainerPtrToValuePtr<FAnchorData>(Slot);
            const bool Fixed = Layout.Anchors.Minimum == Layout.Anchors.Maximum;
            const FVector2D Size(Layout.Offsets.Right, Layout.Offsets.Bottom);
            Row->SetArrayField(TEXT("canvas_offsets"), {MakeShared<FJsonValueNumber>(Layout.Offsets.Left), MakeShared<FJsonValueNumber>(Layout.Offsets.Top), MakeShared<FJsonValueNumber>(Layout.Offsets.Right), MakeShared<FJsonValueNumber>(Layout.Offsets.Bottom)});
            if (Fixed) Row->SetArrayField(TEXT("canvas_size"), {MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)});
            else Row->SetField(TEXT("canvas_size"), MakeShared<FJsonValueNull>());
            Row->SetBoolField(TEXT("fixed_canvas_anchors"), Fixed); Row->SetBoolField(TEXT("zero_canvas_size"), Fixed && (Size.X <= 0 || Size.Y <= 0));
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
        if (auto* Panel = Cast<UPanelWidget>(W)) for (int32 C = 0; C < Panel->GetChildrenCount(); ++C)
        {
            if (Queue.Num() >= 256) { Truncated = true; break; } Queue.Add(Panel->GetChildAt(C));
        }
    }
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("asset_path"), Path); R->SetArrayField(TEXT("widgets"), Rows); R->SetBoolField(TEXT("truncated"), Truncated); R->SetBoolField(TEXT("runtime_instantiated"), false);
    R->SetStringField(TEXT("scope"), TEXT("Stored design tree and native properties only. Fixed anchors/zero sizes are review hints; they do not prove overflow. No bindings are executed. Runtime geometry, keyboard focus, named-slot content, DPI, translations and viewport variants require play/capture tests.")); return Success(R);
}
}
