#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorWorkflowTools.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace JevAdvancedInspectionTests
{
struct FAssets
{
    UPackage* Package = CreatePackage(*(TEXT("/Game/JevAdvancedFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); TArray<UObject*> Assets;
    FAssets() { Package->AddToRoot(); }
    template <typename T> T* Make(const TCHAR* Name) { auto* O = NewObject<T>(Package, Name, RF_Public | RF_Standalone | RF_Transactional); Assets.Add(O); FAssetRegistryModule::AssetCreated(O); return O; }
    ~FAssets() { for (auto* O : Assets) { FAssetRegistryModule::AssetDeleted(O); O->ClearFlags(RF_Public | RF_Standalone); } Package->SetDirtyFlag(false); Package->RemoveFromRoot(); }
};
AStaticMeshActor* Cube(UWorld* W, const FVector& Location, const FVector& Scale)
{
    auto* A = W->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator); auto* C = A->GetStaticMeshComponent(); C->SetMobility(EComponentMobility::Movable); C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"))); C->SetCollisionProfileName(TEXT("BlockAll")); A->SetActorScale3D(Scale); return A;
}
TSharedRef<FJsonObject> Query(const TCHAR* Kind, const FString& Path = {}) { auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("kind"), Kind); if (!Path.IsEmpty()) P->SetStringField(TEXT("target_path"), Path); return P; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevAdvancedSurface, "Jev.Editor.AdvancedSurfaceSupport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevAdvancedSurface::RunTest(const FString&)
{
    using namespace JevAdvancedInspectionTests; auto* W = FAutomationEditorCommonUtils::CreateNewMap(); auto* Floor = Cube(W, FVector(0, 0, -50), FVector(10, 10, 1)); auto* Actor = Cube(W, FVector(0, 0, 400), FVector(1));
    ON_SCOPE_EXIT { W->EditorDestroyActor(Actor, true); W->EditorDestroyActor(Floor, true); };
    auto Q = Query(TEXT("surface")); Q->SetStringField(TEXT("actor_path"), Actor->GetPathName()); Q->SetArrayField(TEXT("surface_paths"), {MakeShared<FJsonValueString>(Floor->GetPathName())}); Q->SetStringField(TEXT("trace_channel"), TEXT("visibility")); Q->SetNumberField(TEXT("trace_up_cm"), 100); Q->SetNumberField(TEXT("trace_down_cm"), 1000); Q->SetNumberField(TEXT("max_slope_degrees"), 30); Q->SetNumberField(TEXT("clearance_cm"), 1); Q->SetBoolField(TEXT("align_to_normal"), true);
    auto Result = JevWorkflow::Surface(Q); if (!TestTrue(TEXT("default support query succeeds"), Result->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("default five samples"), Result->GetObjectField(TEXT("result"))->GetArrayField(TEXT("support_samples")).Num(), 5);
    TestTrue(TEXT("full flat footprint supported"), Result->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid")));
    Q->SetBoolField(TEXT("trace_complex"), true); Q->SetNumberField(TEXT("support_samples"), 9); Result = JevWorkflow::Surface(Q);
    TestTrue(TEXT("triangle collision supports full footprint"), Result->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid"))); TestEqual(TEXT("nine complex probes"), Result->GetObjectField(TEXT("result"))->GetArrayField(TEXT("support_samples")).Num(), 9);
    auto* Raised = Cube(W, FVector(45, 45, 5), FVector(0.2, 0.2, 0.1)); Q->SetArrayField(TEXT("surface_paths"), {MakeShared<FJsonValueString>(Floor->GetPathName()), MakeShared<FJsonValueString>(Raised->GetPathName())}); Result = JevWorkflow::Surface(Q);
    TestTrue(TEXT("accepted uneven support remains clear"), Result->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid"))); TestEqual(TEXT("placement lifts above highest sampled bump"), Result->GetObjectField(TEXT("result"))->GetArrayField(TEXT("location"))[2]->AsNumber(), 61.0, 0.1); W->EditorDestroyActor(Raised, true); Q->SetArrayField(TEXT("surface_paths"), {MakeShared<FJsonValueString>(Floor->GetPathName())});
    Floor->SetActorScale3D(FVector(0.5, 0.5, 1)); Result = JevWorkflow::Surface(Q); TestFalse(TEXT("center-only support cannot hide missing corners"), Result->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid"))); TestEqual(TEXT("missing support is explicit"), Result->GetObjectField(TEXT("result"))->GetStringField(TEXT("reason")), FString(TEXT("incomplete_footprint_support")));
    Q->SetNumberField(TEXT("support_samples"), 6); TestFalse(TEXT("unsupported probe counts refused"), JevWorkflow::Surface(Q)->GetBoolField(TEXT("ok"))); Q->SetNumberField(TEXT("support_samples"), 5);
    auto* Streaming = NewObject<ULevelStreamingDynamic>(W); W->AddStreamingLevel(Streaming); Result = JevWorkflow::Surface(Q); W->RemoveStreamingLevel(Streaming);
    TestFalse(TEXT("unloaded streaming collision refused"), Result->GetBoolField(TEXT("ok"))); TestEqual(TEXT("streaming refusal code"), Result->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("streaming_world_unsupported")));
    TestEqual(TEXT("inspection never moves source"), Actor->GetActorLocation().Z, 400.0); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevAdvancedRig, "Jev.Editor.AdvancedRigSampling", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevAdvancedRig::RunTest(const FString&)
{
    using namespace JevAdvancedInspectionTests; FAssets Assets; auto* Skeleton = Assets.Make<USkeleton>(TEXT("Skeleton"));
    { FReferenceSkeletonModifier Modifier(Skeleton); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
    auto* Mesh = Assets.Make<USkeletalMesh>(TEXT("Mesh")); Mesh->SetSkeleton(Skeleton); Mesh->SetRefSkeleton(Skeleton->GetReferenceSkeleton());
    auto* Imported = Mesh->GetImportedModel(); if (!TestNotNull(TEXT("native imported model allocated"), Imported)) return false;
    auto* LOD = new FSkeletalMeshLODModel(); Imported->LODModels.Add(LOD); LOD->Sections.AddDefaulted(); auto& Section = LOD->Sections[0]; Section.BoneMap.Add(0); Section.NumVertices = 3; Section.SoftVertices.SetNumZeroed(3);
    Section.SoftVertices[0].InfluenceWeights[0] = 65535; Section.SoftVertices[1].InfluenceWeights[0] = 100; Section.SoftVertices[1].InfluenceBones[0] = 30;
    auto Q = Query(TEXT("rig"), Mesh->GetPathName()); Q->SetArrayField(TEXT("required_bones"), {}); Q->SetBoolField(TEXT("inspect_skin_weights"), true);
    auto Result = JevWorkflow::Rig(Q); if (!TestTrue(TEXT("native imported weights inspected"), Result->GetBoolField(TEXT("ok")))) return false;
    const auto Weights = Result->GetObjectField(TEXT("result"))->GetObjectField(TEXT("skin_weights")); TestEqual(TEXT("all three source vertices checked"), Weights->GetNumberField(TEXT("checked_vertices")), 3.0); TestEqual(TEXT("invalid section bone map detected"), Weights->GetNumberField(TEXT("invalid_bone_vertices")), 1.0); TestEqual(TEXT("unweighted source detected"), Weights->GetNumberField(TEXT("unweighted_vertices")), 1.0); TestEqual(TEXT("normalization defects detected"), Weights->GetNumberField(TEXT("unnormalized_vertices")), 2.0); TestFalse(TEXT("invalid weights do not pass"), Weights->GetBoolField(TEXT("all_checked_weights_valid")));
    auto* Sequence = Assets.Make<UAnimSequence>(TEXT("RootMotion")); Sequence->SetSkeleton(Skeleton); Sequence->bEnableRootMotion = true; auto& Controller = Sequence->GetController(); Controller.InitializeModel(); Controller.OpenBracket(FText::FromString(TEXT("Owned root-motion fixture")), false); Controller.SetFrameRate(FFrameRate(2, 1), false); Controller.SetNumberOfFrames(FFrameNumber(2), false); Controller.AddBoneCurve(TEXT("root"), false); Controller.SetBoneTrackKeys(TEXT("root"), TArray<FVector3f>{FVector3f::ZeroVector, FVector3f(50, 0, 0), FVector3f(100, 0, 0)}, TArray<FQuat4f>{FQuat4f::Identity, FQuat4f::Identity, FQuat4f::Identity}, TArray<FVector3f>{FVector3f::OneVector, FVector3f::OneVector, FVector3f::OneVector}, false); Controller.NotifyPopulated(); Controller.CloseBracket(false); FAssetCompilingManager::Get().FinishAllCompilation();
    Q->SetStringField(TEXT("animation_path"), Sequence->GetPathName()); Q->SetNumberField(TEXT("root_motion_samples"), 4); Result = JevWorkflow::Rig(Q);
    if (!TestTrue(TEXT("root-track extraction succeeds"), Result->GetBoolField(TEXT("ok")))) return false; const auto Motion = Result->GetObjectField(TEXT("result"));
    if (!TestTrue(TEXT("real root track is available"), Motion->GetBoolField(TEXT("root_motion_extraction_available")))) return false;
    TestTrue(TEXT("extracted transforms are finite"), Motion->GetBoolField(TEXT("root_motion_finite"))); TestEqual(TEXT("four root-motion intervals"), Motion->GetArrayField(TEXT("root_motion_intervals")).Num(), 4); TestEqual(TEXT("one hundred centimeters extracted from actual keys"), Motion->GetArrayField(TEXT("root_motion_total_translation_cm"))[0]->AsNumber(), 100.0, 0.1);
    Q->SetNumberField(TEXT("root_motion_samples"), 65); TestFalse(TEXT("root sample bound enforced"), JevWorkflow::Rig(Q)->GetBoolField(TEXT("ok"))); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevRuntimeWidgetGuards, "Jev.Editor.RuntimeWidgetGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevRuntimeWidgetGuards::RunTest(const FString&)
{
    using namespace JevAdvancedInspectionTests; FAssets Assets; auto* BP = Assets.Make<UWidgetBlueprint>(TEXT("Widget")); BP->WidgetTree = NewObject<UWidgetTree>(BP); auto Q = Query(TEXT("widgets"), BP->GetPathName()); Q->SetStringField(TEXT("runtime_instance_path"), TEXT("/Game/Unrelated.Widget"));
    const auto Result = JevWorkflow::Widgets(Q); TestFalse(TEXT("unrelated or nonexistent widget never instantiated"), Result->GetBoolField(TEXT("ok"))); TestEqual(TEXT("missing active PIE instance is explicit"), Result->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("runtime_widget_unavailable"))); TestNull(TEXT("inspection preserves empty design tree"), BP->WidgetTree->RootWidget.Get()); return true;
}

#endif
