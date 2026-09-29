#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetCompilingManager.h"
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
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"

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
        TArray<TSharedPtr<FJsonValue>> SlotNames;
        for (int32 Index = 0; Index < FMath::Min(256, Mesh->GetStaticMaterials().Num()); ++Index) SlotNames.Add(MakeShared<FJsonValueString>(Mesh->GetStaticMaterials()[Index].MaterialSlotName.ToString().Left(128)));
        R->SetArrayField(TEXT("material_slot_names"), SlotNames); R->SetBoolField(TEXT("material_slots_truncated"), Mesh->GetStaticMaterials().Num() > 256);
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
    FString Path, AnimationPath; const TArray<TSharedPtr<FJsonValue>>* Required = nullptr; bool Weights = false; double RootSamples = 8;
    if (!Only(P, {TEXT("kind"), TEXT("target_path"), TEXT("animation_path"), TEXT("required_bones"), TEXT("inspect_skin_weights"), TEXT("root_motion_samples")}) || !Text(P, TEXT("target_path"), Path) || !P->TryGetArrayField(TEXT("required_bones"), Required) || Required->Num() > 64 || (P->HasField(TEXT("animation_path")) && !Text(P, TEXT("animation_path"), AnimationPath)) || (P->HasField(TEXT("inspect_skin_weights")) && !P->TryGetBoolField(TEXT("inspect_skin_weights"), Weights)) || (P->HasField(TEXT("root_motion_samples")) && (!Number(P, TEXT("root_motion_samples"), RootSamples, 1, 64) || RootSamples != FMath::FloorToDouble(RootSamples)))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a mesh/skeleton, up to 64 required bone names, optional sequence, skin-weight inspection and 1..64 root-motion samples."));
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
    if (Weights)
    {
        auto WeightResult = MakeShared<FJsonObject>(); WeightResult->SetBoolField(TEXT("available"), false); WeightResult->SetNumberField(TEXT("lod"), 0);
        const auto* Imported = Mesh && !Mesh->IsCompiling() ? Mesh->GetImportedModel() : nullptr;
        if (Imported && !Imported->LODModels.IsEmpty())
        {
            const auto& LOD = Imported->LODModels[0]; int32 Checked = 0, Unweighted = 0, InvalidBones = 0, Unnormalized = 0; bool Truncated = false, MissingData = false;
            for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
            {
                if (SectionIndex >= 256) { Truncated = true; break; }
                const auto& Section = LOD.Sections[SectionIndex]; if (Section.NumVertices != Section.SoftVertices.Num()) MissingData = true;
                for (const auto& Vertex : Section.SoftVertices)
                {
                    if (Checked >= 65536) { Truncated = true; break; }
                    uint32 Sum = 0; bool Invalid = false;
                    for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
                    {
                        const uint16 Weight = Vertex.InfluenceWeights[Influence]; Sum += Weight;
                        const int32 Bone = Vertex.InfluenceBones[Influence];
                        if (Weight && (!Section.BoneMap.IsValidIndex(Bone) || !Ref.IsValidIndex(Section.BoneMap[Bone]))) Invalid = true;
                    }
                    ++Checked; if (!Sum) ++Unweighted; if (Invalid) ++InvalidBones; if (FMath::Abs(static_cast<int64>(Sum) - 65535) > 1) ++Unnormalized;
                }
                if (Truncated) break;
            }
            WeightResult->SetBoolField(TEXT("available"), Checked > 0); WeightResult->SetNumberField(TEXT("checked_vertices"), Checked); WeightResult->SetNumberField(TEXT("unweighted_vertices"), Unweighted); WeightResult->SetNumberField(TEXT("invalid_bone_vertices"), InvalidBones); WeightResult->SetNumberField(TEXT("unnormalized_vertices"), Unnormalized); WeightResult->SetBoolField(TEXT("truncated"), Truncated); WeightResult->SetBoolField(TEXT("missing_section_data"), MissingData);
            WeightResult->SetBoolField(TEXT("all_checked_weights_valid"), Checked > 0 && !Unweighted && !InvalidBones && !Unnormalized); WeightResult->SetBoolField(TEXT("complete_lod_checked"), Checked > 0 && !Truncated && !MissingData);
        }
        WeightResult->SetStringField(TEXT("scope"), TEXT("At most 65,536 imported LOD0 vertices and 256 sections; normalized uint16 weights and section bone-map references. Does not evaluate deformed geometry, seams, alternative skin-weight profiles or retarget quality.")); R->SetObjectField(TEXT("skin_weights"), WeightResult);
    }
    if (!AnimationPath.IsEmpty())
    {
        auto* Sequence = Cast<UAnimSequence>(Loaded(AnimationPath));
        if (!Sequence || Sequence->GetClass() != UAnimSequence::StaticClass()) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open the native animation sequence first."));
        R->SetStringField(TEXT("animation_path"), AnimationPath); R->SetStringField(TEXT("animation_skeleton_path"), GetPathNameSafe(Sequence->GetSkeleton())); R->SetBoolField(TEXT("exact_skeleton_match"), Skeleton == Sequence->GetSkeleton());
        R->SetBoolField(TEXT("root_motion_enabled"), Sequence->bEnableRootMotion); R->SetNumberField(TEXT("duration_seconds"), Sequence->GetPlayLength());
        const bool Ready = FAssetCompilingManager::Get().GetNumRemainingAssets() == 0 && Sequence->GetSkeleton() && Sequence->GetSkeleton()->GetReferenceSkeleton().GetNum() > 0 && Sequence->GetPlayLength() > 0 && Sequence->GetPlayLength() <= 3600;
        R->SetBoolField(TEXT("root_motion_extraction_available"), Ready);
        if (Ready)
        {
            TArray<TSharedPtr<FJsonValue>> Motion; bool Finite = true; FAnimExtractContext Context(0, true); Context.bExtractWithRootMotionProvider = false;
            const FTransform Full = Sequence->ExtractRootMotionFromRange(0, Sequence->GetPlayLength(), Context);
            for (int32 Index = 0; Index < static_cast<int32>(RootSamples); ++Index)
            {
                const double Begin = Sequence->GetPlayLength() * Index / RootSamples, End = Sequence->GetPlayLength() * (Index + 1) / RootSamples;
                const FTransform Extracted = Sequence->ExtractRootMotionFromRange(Begin, End, Context); auto Row = MakeShared<FJsonObject>(); const bool Valid = !Extracted.ContainsNaN(); Finite &= Valid;
                Row->SetNumberField(TEXT("start_seconds"), Begin); Row->SetNumberField(TEXT("end_seconds"), End); Row->SetBoolField(TEXT("finite"), Valid);
                if (Valid) { Row->SetArrayField(TEXT("translation_cm"), Vector(Extracted.GetTranslation())); Row->SetNumberField(TEXT("rotation_degrees"), FMath::RadiansToDegrees(Extracted.GetRotation().GetAngle())); }
                Motion.Add(MakeShared<FJsonValueObject>(Row));
            }
            R->SetArrayField(TEXT("root_motion_intervals"), Motion); R->SetBoolField(TEXT("root_motion_finite"), Finite && !Full.ContainsNaN());
            if (!Full.ContainsNaN()) { R->SetArrayField(TEXT("root_motion_total_translation_cm"), Vector(Full.GetTranslation())); R->SetNumberField(TEXT("root_motion_total_rotation_degrees"), FMath::RadiansToDegrees(Full.GetRotation().GetAngle())); }
        }
    }
    R->SetStringField(TEXT("scope"), TEXT("Stored hierarchy, skeleton identity, optional bounded imported skin-weight checks and native root-track extraction. Extraction does not tick actors, evaluate notifies, advance animation graphs or apply root motion to a character. Playback, retargeting and visual deformation require separate gameplay acceptance.")); return Success(R);
}

TSharedRef<FJsonObject> Widgets(const TSharedPtr<FJsonObject>& P)
{
    FString Path, RuntimePath;
    if (!Only(P, {TEXT("kind"), TEXT("target_path"), TEXT("runtime_instance_path")}) || !Text(P, TEXT("target_path"), Path) || (P->HasField(TEXT("runtime_instance_path")) && !Text(P, TEXT("runtime_instance_path"), RuntimePath))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply an exact Widget Blueprint and optional existing runtime instance path."));
    auto* BP = Cast<UWidgetBlueprint>(Loaded(Path));
    if (!BP || BP->GetClass() != UWidgetBlueprint::StaticClass() || !BP->WidgetTree) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open a native Widget Blueprint with a widget tree."));
    if (!RuntimePath.IsEmpty())
    {
        auto* Instance = FindObject<UUserWidget>(nullptr, *RuntimePath); UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
        if (!IsValid(Instance) || Instance->GetPathName() != RuntimePath || !World || BP->ParentClass != UUserWidget::StaticClass() || Instance->GetWorld() != World || !World->HasBegunPlay() || Instance->GetClass() != BP->GeneratedClass || !Instance->IsInViewport() || !Instance->WidgetTree || !Instance->GetCachedWidget().IsValid()) return FJevEditorBridge::Error(TEXT("runtime_widget_unavailable"), TEXT("Supply an existing on-screen instance of this exact Widget Blueprint directly based on native UserWidget in the active PIE world. Inspection never creates or ticks widgets."));
        auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("asset_path"), Path); R->SetStringField(TEXT("runtime_instance_path"), RuntimePath); R->SetBoolField(TEXT("runtime_instantiated"), true); R->SetBoolField(TEXT("runtime_created_by_inspection"), false); R->SetStringField(TEXT("runtime_world_path"), World->GetPathName());
        FVector2D ViewportSize = FVector2D::ZeroVector; if (World->GetGameViewport()) World->GetGameViewport()->GetViewportSize(ViewportSize);
        R->SetArrayField(TEXT("viewport_size_pixels"), {MakeShared<FJsonValueNumber>(ViewportSize.X), MakeShared<FJsonValueNumber>(ViewportSize.Y)});
        R->SetNumberField(TEXT("root_accumulated_layout_scale"), Instance->GetCachedWidget()->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale());
        R->SetNumberField(TEXT("slate_application_scale"), FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetApplicationScale() : 1);
        const auto Focused = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetKeyboardFocusedWidget() : TSharedPtr<SWidget>();
        TArray<UWidget*> RuntimeQueue; if (Instance->WidgetTree->RootWidget) RuntimeQueue.Add(Instance->WidgetTree->RootWidget); TSet<UWidget*> Seen; TArray<TSharedPtr<FJsonValue>> Rows; bool Truncated = false;
        for (int32 Index = 0; Index < RuntimeQueue.Num() && Index < 256; ++Index)
        {
            auto* Widget = RuntimeQueue[Index]; if (!IsValid(Widget) || Seen.Contains(Widget)) continue; Seen.Add(Widget);
            auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), Widget->GetName().Left(128)); Row->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName()); Row->SetStringField(TEXT("parent"), GetNameSafe(Widget->GetParent()).Left(128));
            const auto Cached = Widget->GetCachedWidget(); Row->SetBoolField(TEXT("slate_cached"), Cached.IsValid());
            if (Cached.IsValid())
            {
                const auto& Geometry = Cached->GetCachedGeometry(); const FVector2D Size = Geometry.GetLocalSize(); const FVector2D Desired = Cached->GetDesiredSize();
                Row->SetArrayField(TEXT("allocated_size"), {MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)}); Row->SetArrayField(TEXT("desired_size"), {MakeShared<FJsonValueNumber>(Desired.X), MakeShared<FJsonValueNumber>(Desired.Y)}); Row->SetBoolField(TEXT("keyboard_focus"), Cached == Focused);
                Row->SetBoolField(TEXT("desired_size_exceeds_allocation"), Desired.X > Size.X + 0.5 || Desired.Y > Size.Y + 0.5);
                if (auto* Parent = Widget->GetParent(); Parent && Parent->GetCachedWidget().IsValid())
                {
                    const auto& ParentGeometry = Parent->GetCachedWidget()->GetCachedGeometry(); const FVector2D TopLeft = ParentGeometry.AbsoluteToLocal(Geometry.LocalToAbsolute(FVector2D::ZeroVector)); const FVector2D BottomRight = ParentGeometry.AbsoluteToLocal(Geometry.LocalToAbsolute(Size)); const FVector2D ParentSize = ParentGeometry.GetLocalSize();
                    Row->SetArrayField(TEXT("parent_local_top_left"), {MakeShared<FJsonValueNumber>(TopLeft.X), MakeShared<FJsonValueNumber>(TopLeft.Y)}); Row->SetBoolField(TEXT("outside_parent_bounds_hint"), TopLeft.X < -0.5 || TopLeft.Y < -0.5 || BottomRight.X > ParentSize.X + 0.5 || BottomRight.Y > ParentSize.Y + 0.5);
                }
            }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            if (auto* Panel = Cast<UPanelWidget>(Widget)) for (int32 Child = 0; Child < Panel->GetChildrenCount(); ++Child) { if (RuntimeQueue.Num() >= 256) { Truncated = true; break; } RuntimeQueue.Add(Panel->GetChildAt(Child)); }
        }
        R->SetArrayField(TEXT("widgets"), Rows); R->SetBoolField(TEXT("truncated"), Truncated); R->SetStringField(TEXT("scope"), TEXT("Read-only cached Slate geometry and current keyboard focus from an existing active PIE instance; no bindings, widget ticks, viewport resize or focus changes are requested. Layout scale includes inherited scaling and is not isolated DPI. Desired-size and parent-bounds comparisons are hints, not pixel-visible overflow or accessibility proof. Nested UserWidget internals, named slots, transformed clipping, gamepad focus and screen readers require separate acceptance.")); return Success(R);
    }
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
