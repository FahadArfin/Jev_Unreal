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

namespace JevWorkflow
{
TSharedRef<FJsonObject> Surface(const TSharedPtr<FJsonObject>& P)
{
    FString Path, Channel; double Up = 0, Down = 0, MaxSlope = 0, Gap = 0; bool Align = false; const TArray<TSharedPtr<FJsonValue>>* Surfaces = nullptr;
    if (!Only(P, {TEXT("kind"), TEXT("actor_path"), TEXT("surface_paths"), TEXT("trace_channel"), TEXT("trace_up_cm"), TEXT("trace_down_cm"), TEXT("max_slope_degrees"), TEXT("clearance_cm"), TEXT("align_to_normal")}) || !Text(P, TEXT("actor_path"), Path) || !Text(P, TEXT("trace_channel"), Channel, 32) || (Channel != TEXT("visibility") && Channel != TEXT("camera")) || !Number(P, TEXT("trace_up_cm"), Up, 0, 100000) || !Number(P, TEXT("trace_down_cm"), Down, 1, 100000) || !Number(P, TEXT("max_slope_degrees"), MaxSlope, 0, 60) || !Number(P, TEXT("clearance_cm"), Gap, 0, 1000) || !P->TryGetBoolField(TEXT("align_to_normal"), Align) || !P->TryGetArrayField(TEXT("surface_paths"), Surfaces) || Surfaces->IsEmpty() || Surfaces->Num() > 32) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a native mesh actor, 1..32 approved surfaces and bounded trace settings."));
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    auto* Actor = FindObject<AStaticMeshActor>(nullptr, *Path);
    if (!World || !Actor || Actor->GetWorld() != World || Actor->GetClass() != AStaticMeshActor::StaticClass() || !Actor->GetStaticMeshComponent()->GetStaticMesh() || Actor->GetStaticMeshComponent()->IsSimulatingPhysics()) return FJevEditorBridge::Error(TEXT("actor_unsupported"), TEXT("Select an existing native non-simulating mesh actor in this editor."));
    TSet<AActor*> Approved;
    for (const auto& Value : *Surfaces)
    {
        FString S; if (!Value || Value->Type != EJson::String || !Value->TryGetString(S) || S.Len() > 1024) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Surface paths must be exact bounded strings."));
        auto* A = FindObject<AActor>(nullptr, *S); if (!IsValid(A) || A == Actor || A->GetWorld() != World || A->GetPathName() != S) return FJevEditorBridge::Error(TEXT("actor_not_found"), TEXT("An approved surface is not in this editor world.")); Approved.Add(A);
    }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(JevSurface), false); Query.AddIgnoredActor(Actor);
    const FVector Origin = Actor->GetActorLocation(); FHitResult Hit;
    const bool HitFound = World->LineTraceSingleByChannel(Hit, Origin + FVector(0, 0, Up), Origin - FVector(0, 0, Down), Channel == TEXT("visibility") ? ECC_Visibility : ECC_Camera, Query);
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("actor_path"), Path); R->SetBoolField(TEXT("blocking_hit"), HitFound); R->SetStringField(TEXT("trace_channel"), Channel); R->SetBoolField(TEXT("trace_complex"), false);
    R->SetBoolField(TEXT("placement_valid"), false);
    R->SetStringField(TEXT("scope"), TEXT("One simple-collision downward trace. The first blocker must be explicitly approved. Placement uses the oriented mesh bounds and conservative WorldStatic/WorldDynamic box overlaps. No streaming, collision creation, foliage tracing, pawn or physics simulation is performed."));
    if (!HitFound) { R->SetStringField(TEXT("reason"), TEXT("no_surface_hit")); return Success(R); }
    R->SetStringField(TEXT("hit_actor_path"), GetPathNameSafe(Hit.GetActor())); R->SetArrayField(TEXT("hit_location"), Vector(Hit.ImpactPoint)); R->SetArrayField(TEXT("hit_normal"), Vector(Hit.ImpactNormal));
    if (!Approved.Contains(Hit.GetActor())) { R->SetStringField(TEXT("reason"), TEXT("first_blocker_not_approved")); return Success(R); }
    const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Hit.ImpactNormal.Z, -1.0, 1.0))); R->SetNumberField(TEXT("slope_degrees"), Slope);
    if (Slope > MaxSlope) { R->SetStringField(TEXT("reason"), TEXT("slope_exceeds_limit")); return Success(R); }
    FQuat Rotation = Actor->GetActorQuat(); if (Align) Rotation = FQuat::FindBetweenNormals(Rotation.GetUpVector(), Hit.ImpactNormal) * Rotation;
    const auto Bounds = Actor->GetStaticMeshComponent()->GetStaticMesh()->GetBounds();
    const FVector Scale = Actor->GetActorScale3D();
    if (Scale.GetMin() <= 0 || Scale.GetMax() > 1000 || Bounds.BoxExtent.GetMax() <= 0) return FJevEditorBridge::Error(TEXT("actor_bounds_unavailable"), TEXT("Finite positive mesh bounds and scale are required."));
    const FVector Extent = Bounds.BoxExtent * Scale;
    const double Support = FMath::Abs(FVector::DotProduct(Rotation.GetAxisX(), Hit.ImpactNormal)) * Extent.X + FMath::Abs(FVector::DotProduct(Rotation.GetAxisY(), Hit.ImpactNormal)) * Extent.Y + FMath::Abs(FVector::DotProduct(Rotation.GetAxisZ(), Hit.ImpactNormal)) * Extent.Z;
    const FVector Center = Hit.ImpactPoint + Hit.ImpactNormal * (Support + Gap);
    const FVector Location = Center - Rotation.RotateVector(Bounds.Origin * Scale);
    FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    Query.AddIgnoredActor(Hit.GetActor()); TArray<FOverlapResult> Overlaps;
    World->OverlapMultiByObjectType(Overlaps, Center, Rotation, Objects, FCollisionShape::MakeBox(Extent.ComponentMax(FVector(0.01))), Query);
    TArray<TSharedPtr<FJsonValue>> Blockers; TSet<AActor*> Seen;
    for (const auto& Overlap : Overlaps) if (IsValid(Overlap.GetActor()) && !Seen.Contains(Overlap.GetActor())) { Seen.Add(Overlap.GetActor()); if (Blockers.Num() < 32) Blockers.Add(MakeShared<FJsonValueString>(Overlap.GetActor()->GetPathName())); }
    R->SetArrayField(TEXT("overlap_actor_paths"), Blockers); R->SetBoolField(TEXT("overlaps_truncated"), Seen.Num() > 32); R->SetNumberField(TEXT("overlap_actor_count"), Seen.Num());
    R->SetArrayField(TEXT("location"), Vector(Location)); const auto Rot = Rotation.Rotator(); R->SetArrayField(TEXT("rotation"), Vector(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll))); R->SetArrayField(TEXT("scale"), Vector(Scale));
    R->SetBoolField(TEXT("placement_valid"), Seen.IsEmpty()); R->SetStringField(TEXT("reason"), Seen.IsEmpty() ? TEXT("clear_bounds_on_approved_surface") : TEXT("bounds_overlap_other_geometry")); return Success(R);
}

TSharedRef<FJsonObject> Navigation(const TSharedPtr<FJsonObject>& P)
{
    FString Path; FVector Start, End, Extent; double Width = 0, Step = 0;
    if (!Only(P, {TEXT("kind"), TEXT("nav_data_path"), TEXT("start"), TEXT("end"), TEXT("projection_extent_cm"), TEXT("required_width_cm"), TEXT("maximum_step_cm")}) || !Text(P, TEXT("nav_data_path"), Path) || !Vector(P, TEXT("start"), Start) || !Vector(P, TEXT("end"), End) || !Vector(P, TEXT("projection_extent_cm"), Extent, 1000) || Extent.GetMin() <= 0 || !Number(P, TEXT("required_width_cm"), Width, 1, 10000) || !Number(P, TEXT("maximum_step_cm"), Step, 0, 1000)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply exact Recast data, endpoints, bounded projection extent and project width/step requirements."));
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
    R->SetStringField(TEXT("scope"), TEXT("Native Recast projection and synchronous path with the existing default filter. Width/step checks compare project requirements against this nav-agent configuration, not measured corridor geometry. No navmesh rebuild or pawn movement; dynamic obstacles, interactions and actual player accessibility need gameplay tests."));
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
    }
    return Success(R);
}
}
