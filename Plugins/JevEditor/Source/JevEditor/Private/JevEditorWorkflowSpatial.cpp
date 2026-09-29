#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavMesh/RecastNavMesh.h"
#include "Engine/OverlapResult.h"
#include "Engine/LevelStreaming.h"

namespace JevWorkflow
{
TSharedRef<FJsonObject> Surface(const TSharedPtr<FJsonObject>& P)
{
    FString Path, Channel; double Up = 0, Down = 0, MaxSlope = 0, Gap = 0, SampleCount = 5, Variation = 10; bool Align = false, Complex = false; const TArray<TSharedPtr<FJsonValue>>* Surfaces = nullptr;
    if (!Only(P, {TEXT("kind"), TEXT("actor_path"), TEXT("surface_paths"), TEXT("trace_channel"), TEXT("trace_up_cm"), TEXT("trace_down_cm"), TEXT("max_slope_degrees"), TEXT("clearance_cm"), TEXT("align_to_normal"), TEXT("trace_complex"), TEXT("support_samples"), TEXT("max_support_variation_cm")}) || !Text(P, TEXT("actor_path"), Path) || !Text(P, TEXT("trace_channel"), Channel, 32) || (Channel != TEXT("visibility") && Channel != TEXT("camera")) || !Number(P, TEXT("trace_up_cm"), Up, 0, 100000) || !Number(P, TEXT("trace_down_cm"), Down, 1, 100000) || !Number(P, TEXT("max_slope_degrees"), MaxSlope, 0, 60) || !Number(P, TEXT("clearance_cm"), Gap, 0, 1000) || !P->TryGetBoolField(TEXT("align_to_normal"), Align) || !P->TryGetArrayField(TEXT("surface_paths"), Surfaces) || Surfaces->IsEmpty() || Surfaces->Num() > 32 || (P->HasField(TEXT("trace_complex")) && !P->TryGetBoolField(TEXT("trace_complex"), Complex)) || (P->HasField(TEXT("support_samples")) && (!Number(P, TEXT("support_samples"), SampleCount, 1, 9) || (SampleCount != 1 && SampleCount != 5 && SampleCount != 9))) || (P->HasField(TEXT("max_support_variation_cm")) && !Number(P, TEXT("max_support_variation_cm"), Variation, 0, 1000))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a native mesh actor, 1..32 approved surfaces, 1/5/9 support samples and bounded trace settings."));
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    auto* Actor = FindObject<AStaticMeshActor>(nullptr, *Path);
    if (!World || !Actor || Actor->GetWorld() != World || Actor->GetClass() != AStaticMeshActor::StaticClass() || !Actor->GetStaticMeshComponent()->GetStaticMesh() || Actor->GetStaticMeshComponent()->IsSimulatingPhysics()) return FJevEditorBridge::Error(TEXT("actor_unsupported"), TEXT("Select an existing native non-simulating mesh actor in this editor."));
    if (World->GetWorldPartition() || !World->GetStreamingLevels().IsEmpty()) return FJevEditorBridge::Error(TEXT("streaming_world_unsupported"), TEXT("Placement refuses World Partition and streaming-level worlds because unloaded collision cannot be verified."));
    if (Actor->GetActorLocation().ContainsNaN() || Actor->GetActorLocation().GetAbsMax() > 10000000 || Actor->GetActorQuat().ContainsNaN() || Actor->GetActorScale3D().ContainsNaN()) return FJevEditorBridge::Error(TEXT("actor_bounds_unavailable"), TEXT("Source actor transform must be finite and within supported editor bounds."));
    TSet<AActor*> Approved;
    for (const auto& Value : *Surfaces)
    {
        FString S; if (!Value || Value->Type != EJson::String || !Value->TryGetString(S) || S.Len() > 1024) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Surface paths must be exact bounded strings."));
        auto* A = FindObject<AActor>(nullptr, *S); if (!IsValid(A) || A == Actor || A->GetWorld() != World || A->GetPathName() != S) return FJevEditorBridge::Error(TEXT("actor_not_found"), TEXT("An approved surface is not in this editor world.")); Approved.Add(A);
    }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(JevSurface), Complex); Query.AddIgnoredActor(Actor);
    const FVector Origin = Actor->GetActorLocation(); FHitResult Hit;
    const bool HitFound = World->LineTraceSingleByChannel(Hit, Origin + FVector(0, 0, Up), Origin - FVector(0, 0, Down), Channel == TEXT("visibility") ? ECC_Visibility : ECC_Camera, Query);
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("actor_path"), Path); R->SetBoolField(TEXT("blocking_hit"), HitFound); R->SetStringField(TEXT("trace_channel"), Channel); R->SetBoolField(TEXT("trace_complex"), Complex); R->SetNumberField(TEXT("requested_support_samples"), SampleCount); R->SetNumberField(TEXT("max_support_variation_cm"), Variation);
    R->SetBoolField(TEXT("placement_valid"), false);
    R->SetStringField(TEXT("scope"), TEXT("Bounded downward collision probes at the center and optional footprint corners/edge centers. Every first blocker must be approved, within slope and center-plane variation limits. Complex traces use existing triangle collision. Conservative simple bounds overlaps remain separate. Streaming worlds are refused. Sampled support does not prove continuous support, stability or physics settling."));
    if (!HitFound) { R->SetStringField(TEXT("reason"), TEXT("no_surface_hit")); return Success(R); }
    R->SetStringField(TEXT("hit_actor_path"), GetPathNameSafe(Hit.GetActor())); R->SetArrayField(TEXT("hit_location"), Vector(Hit.ImpactPoint)); R->SetArrayField(TEXT("hit_normal"), Vector(Hit.ImpactNormal));
    if (!Approved.Contains(Hit.GetActor())) { R->SetStringField(TEXT("reason"), TEXT("first_blocker_not_approved")); return Success(R); }
    const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Hit.ImpactNormal.Z, -1.0, 1.0))); R->SetNumberField(TEXT("slope_degrees"), Slope);
    if (Slope > MaxSlope) { R->SetStringField(TEXT("reason"), TEXT("slope_exceeds_limit")); return Success(R); }
    FQuat Rotation = Actor->GetActorQuat(); if (Align) Rotation = FQuat::FindBetweenNormals(Rotation.GetUpVector(), Hit.ImpactNormal) * Rotation;
    const auto Bounds = Actor->GetStaticMeshComponent()->GetStaticMesh()->GetBounds();
    const FVector Scale = Actor->GetActorScale3D();
    if (Scale.GetMin() <= 0 || Scale.GetMax() > 1000 || Bounds.BoxExtent.ContainsNaN() || Bounds.Origin.ContainsNaN() || Bounds.BoxExtent.GetMin() <= 0 || (Bounds.BoxExtent * Scale).GetMax() > 1000000) return FJevEditorBridge::Error(TEXT("actor_bounds_unavailable"), TEXT("Finite positive mesh bounds and scale are required, with scaled extents up to one million centimeters."));
    const FVector Extent = Bounds.BoxExtent * Scale;
    const double Support = FMath::Abs(FVector::DotProduct(Rotation.GetAxisX(), Hit.ImpactNormal)) * Extent.X + FMath::Abs(FVector::DotProduct(Rotation.GetAxisY(), Hit.ImpactNormal)) * Extent.Y + FMath::Abs(FVector::DotProduct(Rotation.GetAxisZ(), Hit.ImpactNormal)) * Extent.Z;
    FVector Center = Hit.ImpactPoint + Hit.ImpactNormal * (Support + Gap);
    FVector Location = Center - Rotation.RotateVector(Bounds.Origin * Scale);
    TArray<FVector2D> Offsets = {FVector2D::ZeroVector};
    if (SampleCount >= 5) for (double X : {-0.9, 0.9}) for (double Y : {-0.9, 0.9}) Offsets.Add(FVector2D(X, Y));
    if (SampleCount == 9) { Offsets.Add(FVector2D(-0.9, 0)); Offsets.Add(FVector2D(0.9, 0)); Offsets.Add(FVector2D(0, -0.9)); Offsets.Add(FVector2D(0, 0.9)); }
    TArray<TSharedPtr<FJsonValue>> SupportRows; FString SupportFailure; double LargestVariation = 0, HighestSupport = 0;
    for (const FVector2D Offset : Offsets)
    {
        const FVector OffsetWorld = Rotation.RotateVector(FVector(Offset.X * Extent.X, Offset.Y * Extent.Y, 0));
        const FVector XY = Hit.ImpactPoint + OffsetWorld; FHitResult Probe;
        const bool Found = Offset.IsNearlyZero() ? (Probe = Hit, true) : World->LineTraceSingleByChannel(Probe, FVector(XY.X, XY.Y, Origin.Z + Up), FVector(XY.X, XY.Y, Origin.Z - Down), Channel == TEXT("visibility") ? ECC_Visibility : ECC_Camera, Query);
        auto Row = MakeShared<FJsonObject>(); Row->SetBoolField(TEXT("blocking_hit"), Found); Row->SetArrayField(TEXT("footprint_offset"), {MakeShared<FJsonValueNumber>(Offset.X), MakeShared<FJsonValueNumber>(Offset.Y)});
        if (!Found) { if (SupportFailure.IsEmpty()) SupportFailure = TEXT("incomplete_footprint_support"); }
        else
        {
            const double ProbeSlope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Probe.ImpactNormal.Z, -1.0, 1.0)));
            const double PlaneDistance = FVector::DotProduct(Probe.ImpactPoint - Hit.ImpactPoint, Hit.ImpactNormal);
            LargestVariation = FMath::Max(LargestVariation, FMath::Abs(PlaneDistance));
            HighestSupport = FMath::Max(HighestSupport, PlaneDistance);
            Row->SetStringField(TEXT("hit_actor_path"), GetPathNameSafe(Probe.GetActor())); Row->SetArrayField(TEXT("hit_location"), Vector(Probe.ImpactPoint)); Row->SetNumberField(TEXT("slope_degrees"), ProbeSlope); Row->SetNumberField(TEXT("center_plane_distance_cm"), PlaneDistance);
            if (!Approved.Contains(Probe.GetActor()) && SupportFailure.IsEmpty()) SupportFailure = TEXT("support_blocker_not_approved");
            if (ProbeSlope > MaxSlope && SupportFailure.IsEmpty()) SupportFailure = TEXT("support_slope_exceeds_limit");
            if (FMath::Abs(PlaneDistance) > Variation && SupportFailure.IsEmpty()) SupportFailure = TEXT("support_variation_exceeds_limit");
        }
        SupportRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    R->SetArrayField(TEXT("support_samples"), SupportRows); R->SetNumberField(TEXT("observed_support_variation_cm"), LargestVariation);
    if (!SupportFailure.IsEmpty()) { R->SetStringField(TEXT("reason"), SupportFailure); return Success(R); }
    // Preserve the sampled XY footprint while lifting above the highest accepted
    // support point. Ignoring the ground in the overlap pass must not permit a
    // sampled bump to penetrate the proposed mesh bounds.
    const double Lift = HighestSupport / Hit.ImpactNormal.Z; Center.Z += Lift; Location.Z += Lift; R->SetNumberField(TEXT("support_lift_cm"), Lift);
    FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    Query.AddIgnoredActor(Hit.GetActor()); Query.bTraceComplex = false; TArray<FOverlapResult> Overlaps;
    World->OverlapMultiByObjectType(Overlaps, Center, Rotation, Objects, FCollisionShape::MakeBox(Extent.ComponentMax(FVector(0.01))), Query);
    TArray<TSharedPtr<FJsonValue>> Blockers; TSet<AActor*> Seen;
    for (const auto& Overlap : Overlaps) if (IsValid(Overlap.GetActor()) && !Seen.Contains(Overlap.GetActor())) { Seen.Add(Overlap.GetActor()); if (Blockers.Num() < 32) Blockers.Add(MakeShared<FJsonValueString>(Overlap.GetActor()->GetPathName())); }
    R->SetArrayField(TEXT("overlap_actor_paths"), Blockers); R->SetBoolField(TEXT("overlaps_truncated"), Seen.Num() > 32); R->SetNumberField(TEXT("overlap_actor_count"), Seen.Num());
    R->SetArrayField(TEXT("location"), Vector(Location)); const auto Rot = Rotation.Rotator(); R->SetArrayField(TEXT("rotation"), Vector(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll))); R->SetArrayField(TEXT("scale"), Vector(Scale));
    R->SetBoolField(TEXT("placement_valid"), Seen.IsEmpty()); R->SetStringField(TEXT("reason"), Seen.IsEmpty() ? TEXT("clear_bounds_on_approved_surface") : TEXT("bounds_overlap_other_geometry")); return Success(R);
}

TSharedRef<FJsonObject> Navigation(const TSharedPtr<FJsonObject>& P)
{
    FString Path; FVector Start, End, Extent; double Width = 0, Step = 0, Spacing = 50; bool ProbeGeometry = false;
    if (!Only(P, {TEXT("kind"), TEXT("nav_data_path"), TEXT("start"), TEXT("end"), TEXT("projection_extent_cm"), TEXT("required_width_cm"), TEXT("maximum_step_cm"), TEXT("probe_geometry"), TEXT("probe_spacing_cm")}) || !Text(P, TEXT("nav_data_path"), Path) || !Vector(P, TEXT("start"), Start) || !Vector(P, TEXT("end"), End) || !Vector(P, TEXT("projection_extent_cm"), Extent, 1000) || Extent.GetMin() <= 0 || !Number(P, TEXT("required_width_cm"), Width, 1, 10000) || !Number(P, TEXT("maximum_step_cm"), Step, 0, 1000) || (P->HasField(TEXT("probe_geometry")) && !P->TryGetBoolField(TEXT("probe_geometry"), ProbeGeometry)) || (P->HasField(TEXT("probe_spacing_cm")) && !Number(P, TEXT("probe_spacing_cm"), Spacing, 1, 200))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply exact Recast data, endpoints, bounded projection extent and project width/step requirements."));
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    auto* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
    auto* Data = FindObject<ARecastNavMesh>(nullptr, *Path);
    if (!Nav || !Data || Data->GetClass() != ARecastNavMesh::StaticClass() || Data->GetWorld() != World || Data->GetPathName() != Path) return FJevEditorBridge::Error(TEXT("asset_unavailable"), TEXT("An existing native Recast navmesh in the current world is required; inspect actor paths first."));
    const bool Busy = Nav->IsNavigationBuildInProgress() || Nav->IsNavigationBuildingLocked(static_cast<uint8>(~ENavigationBuildLock::NoUpdateInEditor));
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("nav_data_path"), Path); R->SetBoolField(TEXT("building_or_locked"), Busy);
    R->SetBoolField(TEXT("editor_auto_update_disabled"), Nav->IsNavigationBuildingLocked(static_cast<uint8>(ENavigationBuildLock::NoUpdateInEditor)));
    const auto& Config = Data->GetConfig(); const double AgentStep = Data->GetAgentMaxStepHeight(ENavigationDataResolution::Default); R->SetNumberField(TEXT("agent_radius_cm"), Config.AgentRadius); R->SetNumberField(TEXT("agent_height_cm"), Config.AgentHeight); R->SetNumberField(TEXT("agent_step_height_cm"), AgentStep);
    R->SetBoolField(TEXT("width_requirement_covered_by_agent"), 2 * Config.AgentRadius >= Width); R->SetBoolField(TEXT("step_requirement_covered_by_agent"), AgentStep >= 0 && AgentStep <= Step);
    R->SetBoolField(TEXT("complete_path"), false);
    R->SetBoolField(TEXT("geometry_probe_requested"), ProbeGeometry); R->SetBoolField(TEXT("geometry_probe_complete"), false); R->SetBoolField(TEXT("geometry_clear"), false);
    R->SetStringField(TEXT("scope"), TEXT("Native Recast projection and path with the existing default filter. Agent width/step configuration checks are separate from optional bounded ECC_Pawn collision sweeps and ground-height samples. Sweeps use the requested width and configured height; they do not simulate movement, step-up logic, door interactions or dynamic obstacles. Streaming geometry probes are refused."));
    if (ProbeGeometry && (World->GetWorldPartition() || !World->GetStreamingLevels().IsEmpty())) return FJevEditorBridge::Error(TEXT("streaming_world_unsupported"), TEXT("Geometry probes require a fully resident non-streaming world."));
    if (Busy) { R->SetStringField(TEXT("reason"), TEXT("navigation_busy")); return Success(R); }
    FNavLocation A, B;
    const bool StartOk = Nav->ProjectPointToNavigation(Start, A, Extent, Data); const bool EndOk = Nav->ProjectPointToNavigation(End, B, Extent, Data);
    R->SetBoolField(TEXT("start_projected"), StartOk); R->SetBoolField(TEXT("end_projected"), EndOk);
    if (!StartOk || !EndOk) { R->SetStringField(TEXT("reason"), TEXT("endpoint_projection_failed")); return Success(R); }
    R->SetArrayField(TEXT("projected_start"), Vector(A.Location)); R->SetArrayField(TEXT("projected_end"), Vector(B.Location));
    FPathFindingQuery Query(nullptr, *Data, A.Location, B.Location); Query.SetAllowPartialPaths(false);
    const auto Result = Nav->FindPathSync(Query);
    const bool Complete = Result.IsSuccessful() && Result.Path.IsValid() && !Result.Path->IsPartial();
    R->SetBoolField(TEXT("complete_path"), Complete); R->SetStringField(TEXT("reason"), Complete ? TEXT("complete_native_path") : TEXT("no_complete_native_path"));
    if (Result.Path.IsValid())
    {
        const auto& Points = Result.Path->GetPathPoints(); TArray<TSharedPtr<FJsonValue>> Rows;
        for (int32 I = 0; I < FMath::Min(256, Points.Num()); ++I) Rows.Add(MakeShared<FJsonValueArray>(Vector(Points[I].Location)));
        R->SetArrayField(TEXT("path_points"), Rows); R->SetBoolField(TEXT("path_truncated"), Points.Num() > 256); R->SetNumberField(TEXT("path_length_cm"), Result.Path->GetLength());
        if (ProbeGeometry && Complete)
        {
            if (Points.IsEmpty() || Points.Num() > 256 || Config.AgentHeight < Width || Config.AgentHeight > 10000)
            { R->SetStringField(TEXT("geometry_reason"), TEXT("unsupported_path_or_capsule")); return Success(R); }
            TArray<FVector> Samples = {Points[0].Location};
            for (int32 Segment = 1; Segment < Points.Num(); ++Segment)
            {
                const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector::Dist(Points[Segment - 1].Location, Points[Segment].Location) / Spacing));
                if (Steps > 256 - Samples.Num()) { R->SetStringField(TEXT("geometry_reason"), TEXT("probe_budget_exceeded")); return Success(R); }
                for (int32 J = 1; J <= Steps; ++J) Samples.Add(FMath::Lerp(Points[Segment - 1].Location, Points[Segment].Location, static_cast<double>(J) / Steps));
            }
            FCollisionQueryParams ProbeQuery(SCENE_QUERY_STAT(JevNavigationGeometry), false); ProbeQuery.AddIgnoredActor(Data);
            TArray<TSharedPtr<FJsonValue>> Probes; FString Failure; FVector Previous = FVector::ZeroVector; bool HavePrevious = false;
            double MaxObservedStep = 0; const auto Shape = FCollisionShape::MakeCapsule(Width / 2, Config.AgentHeight / 2);
            for (const FVector& Point : Samples)
            {
                auto Row = MakeShared<FJsonObject>(); FHitResult Ground;
                const bool Grounded = World->LineTraceSingleByChannel(Ground, Point + FVector(0, 0, Step + 10), Point - FVector(0, 0, Step + 100), ECC_Pawn, ProbeQuery);
                Row->SetBoolField(TEXT("ground_hit"), Grounded);
                if (!Grounded) { if (Failure.IsEmpty()) Failure = TEXT("ground_gap"); HavePrevious = false; }
                else
                {
                    const FVector Center = Ground.ImpactPoint + FVector(0, 0, Config.AgentHeight / 2 + 2);
                    Row->SetArrayField(TEXT("ground_location"), Vector(Ground.ImpactPoint));
                    bool Blocked = World->OverlapBlockingTestByChannel(Center, FQuat::Identity, ECC_Pawn, Shape, ProbeQuery);
                    if (HavePrevious)
                    {
                        const double HeightChange = FMath::Abs(Center.Z - Previous.Z); MaxObservedStep = FMath::Max(MaxObservedStep, HeightChange);
                        if (HeightChange > Step + 0.1 && Failure.IsEmpty()) Failure = TEXT("sampled_height_change_exceeds_step");
                        FHitResult Obstacle; Blocked |= World->SweepSingleByChannel(Obstacle, Previous, Center, FQuat::Identity, ECC_Pawn, Shape, ProbeQuery);
                    }
                    Row->SetBoolField(TEXT("capsule_blocked"), Blocked); if (Blocked && Failure.IsEmpty()) Failure = TEXT("capsule_clearance_blocked");
                    Previous = Center; HavePrevious = true;
                }
                Probes.Add(MakeShared<FJsonValueObject>(Row));
            }
            R->SetArrayField(TEXT("geometry_samples"), Probes); R->SetBoolField(TEXT("geometry_probe_complete"), true); R->SetBoolField(TEXT("geometry_clear"), Failure.IsEmpty());
            R->SetNumberField(TEXT("maximum_sampled_height_change_cm"), MaxObservedStep); R->SetNumberField(TEXT("probe_spacing_cm"), Spacing); R->SetNumberField(TEXT("tested_capsule_radius_cm"), Width / 2); R->SetNumberField(TEXT("tested_capsule_half_height_cm"), Config.AgentHeight / 2);
            R->SetStringField(TEXT("geometry_reason"), Failure.IsEmpty() ? TEXT("sampled_corridor_clear") : Failure);
        }
    }
    return Success(R);
}
}
