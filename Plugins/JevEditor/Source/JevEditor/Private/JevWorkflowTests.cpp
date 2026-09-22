#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Widgets/Text/STextBlock.h"

namespace JevWorkflowTests
{
TSharedRef<FJsonObject> Identity()
{
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("project_file"), TEXT("/fixture/JevSandbox.uproject")); R->SetStringField(TEXT("session_id"), TEXT("domain-session")); R->SetStringField(TEXT("world_path"), TEXT("/fixture/World")); R->SetStringField(TEXT("revision"), TEXT("domain-revision")); R->SetBoolField(TEXT("play_in_editor"), false); R->SetBoolField(TEXT("simulating"), false); return R;
}
TSharedRef<FJsonObject> Query(const TCHAR* Kind, const FString& Path = {})
{
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("kind"), Kind); if (!Path.IsEmpty()) R->SetStringField(TEXT("target_path"), Path); return R;
}
TSharedRef<FJsonObject> Preview(const TSharedRef<FJsonObject>& Change)
{
    auto R = MakeShared<FJsonObject>(); R->SetObjectField(TEXT("change"), Change); R->SetStringField(TEXT("expected_project"), Identity()->GetStringField(TEXT("project_file")));
    auto S = MakeShared<FJsonObject>(); for (const TCHAR* K : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) S->SetStringField(K, Identity()->GetStringField(K)); R->SetObjectField(TEXT("expected_state"), S); return R;
}
TSharedRef<FJsonObject> Commit(const TSharedRef<FJsonObject>& Plan)
{
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("plan_id"), Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"))); R->SetStringField(TEXT("expected_project"), Identity()->GetStringField(TEXT("project_file"))); return R;
}
FString Code(const TSharedRef<FJsonObject>& R) { return R->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : R->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")); }
struct FAssets
{
    UPackage* Package; TArray<UObject*> Assets;
    FAssets() { Package = CreatePackage(*(TEXT("/Game/JevDomainFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); Package->AddToRoot(); }
    template <typename T> T* Make(const TCHAR* Name) { T* O = NewObject<T>(Package, Name, RF_Public | RF_Standalone | RF_Transactional); Assets.Add(O); FAssetRegistryModule::AssetCreated(O); return O; }
    ~FAssets() { for (auto* O : Assets) { FAssetRegistryModule::AssetDeleted(O); O->ClearFlags(RF_Public | RF_Standalone); } Package->SetDirtyFlag(false); Package->RemoveFromRoot(); }
};
AStaticMeshActor* Cube(UWorld* W, const FVector& Location, const FVector& Scale)
{
    auto* A = W->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
    A->SetFlags(RF_Transactional); auto* C = A->GetStaticMeshComponent(); C->SetFlags(RF_Transactional); C->SetMobility(EComponentMobility::Movable);
    C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"))); C->SetCollisionProfileName(TEXT("BlockAll")); A->SetActorScale3D(Scale); return A;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainMaterials, "Jev.Editor.DomainMaterials", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainMaterials::RunTest(const FString&)
{
    using namespace JevWorkflowTests; FAssets Assets;
    auto* M = Assets.Make<UMaterial>(TEXT("Parent"));
    auto* Scalar = NewObject<UMaterialExpressionScalarParameter>(M); Scalar->ParameterName = TEXT("Roughness"); Scalar->DefaultValue = 0.3f; M->GetExpressionCollection().AddExpression(Scalar); M->GetEditorOnlyData()->Roughness.Expression = Scalar;
    auto* Color = NewObject<UMaterialExpressionVectorParameter>(M); Color->ParameterName = TEXT("Tint"); Color->DefaultValue = FLinearColor::Red; M->GetExpressionCollection().AddExpression(Color); M->GetEditorOnlyData()->BaseColor.Expression = Color; M->PostEditChange();
    auto* Instance = Assets.Make<UMaterialInstanceConstant>(TEXT("Instance")); Instance->SetParentEditorOnly(M); Instance->PostEditChange();
    TArray<FString> Old; bool Enabled = false; const bool Had = GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), Enabled, GGameIni); GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Old, GGameIni);
    ON_SCOPE_EXIT { if (Had) GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), Enabled, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Old, GGameIni); };
    GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), true, GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), {Instance->GetPathName()}, GGameIni);
    FJevWorkflowTools Tools;
    const auto Inspected = Tools.Execute(TEXT("workflow_inspect"), Query(TEXT("material"), Instance->GetPathName()), Identity());
    if (!TestTrue(TEXT("native instance inspection"), Inspected->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("two exposed parameters"), Inspected->GetObjectField(TEXT("result"))->GetArrayField(TEXT("parameters")).Num(), 2);
    for (const TCHAR* K : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) TestEqual(TEXT("material response preserves inspected identity"), Inspected->GetObjectField(TEXT("result"))->GetStringField(K), Identity()->GetStringField(K));
    auto Change = Query(TEXT("material_scalar"), Instance->GetPathName()); Change->SetStringField(TEXT("parameter"), TEXT("Roughness")); Change->SetNumberField(TEXT("value"), 0.8);
    const auto Plan = Tools.Execute(TEXT("workflow_preview"), Preview(Change), Identity()); if (!TestTrue(TEXT("scalar plan"), Plan->GetBoolField(TEXT("ok")))) return false;
    float Value = 0; Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Roughness")), Value); TestEqual(TEXT("preview preserves value"), Value, 0.3f);
    const auto Done = Tools.Execute(TEXT("workflow_apply"), Commit(Plan), Identity()); TestTrue(TEXT("scalar applied"), Done->GetBoolField(TEXT("ok"))); Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Roughness")), Value); TestEqual(TEXT("scalar readback"), Value, 0.8f);
    TestEqual(TEXT("no replay"), Code(Tools.Execute(TEXT("workflow_apply"), Commit(Plan), Identity())), FString(TEXT("plan_consumed")));
    GEditor->UndoTransaction(); Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Roughness")), Value); TestEqual(TEXT("Undo restores inherited scalar"), Value, 0.3f);
    auto VectorChange = Query(TEXT("material_vector"), Instance->GetPathName()); VectorChange->SetStringField(TEXT("parameter"), TEXT("Tint")); VectorChange->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1)});
    auto VectorPlan = Tools.Execute(TEXT("workflow_preview"), Preview(VectorChange), Identity()); if (!TestTrue(TEXT("vector plan"), VectorPlan->GetBoolField(TEXT("ok")))) return false;
    TestTrue(TEXT("vector applied"), Tools.Execute(TEXT("workflow_apply"), Commit(VectorPlan), Identity())->GetBoolField(TEXT("ok"))); FLinearColor C; Instance->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")), C); TestEqual(TEXT("vector readback"), C, FLinearColor::Green);
    Change->SetStringField(TEXT("parameter"), TEXT("NotExposed")); TestEqual(TEXT("unknown parameter refused"), Code(Tools.Execute(TEXT("workflow_preview"), Preview(Change), Identity())), FString(TEXT("bad_request")));
    Change->SetStringField(TEXT("parameter"), TEXT("Roughness")); auto Revoked = Tools.Execute(TEXT("workflow_preview"), Preview(Change), Identity()); if (!TestTrue(TEXT("revocation plan"), Revoked->GetBoolField(TEXT("ok")))) return false;
    GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), false, GGameIni); TestEqual(TEXT("revocation at commit"), Code(Tools.Execute(TEXT("workflow_apply"), Commit(Revoked), Identity())), FString(TEXT("policy_invalid")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainLights, "Jev.Editor.DomainLights", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainLights::RunTest(const FString&)
{
    using namespace JevWorkflowTests; UWorld* W = FAutomationEditorCommonUtils::CreateNewMap(); auto* A = W->SpawnActor<APointLight>(); A->SetFlags(RF_Transactional); A->GetLightComponent()->SetFlags(RF_Transactional); A->GetLightComponent()->SetMobility(EComponentMobility::Movable);
    ON_SCOPE_EXIT { W->EditorDestroyActor(A, true); };
    double Now = 10; FJevWorkflowTools Tools([&Now] { return Now; }); auto Change = Query(TEXT("light"), A->GetPathName()); Change->SetNumberField(TEXT("intensity"), 321); Change->SetArrayField(TEXT("color_rgb"), JevWorkflow::Vector(FVector(0.2, 0.4, 0.8)));
    auto Inspection = Tools.Execute(TEXT("workflow_inspect"), Query(TEXT("light"), A->GetPathName()), Identity());
    TestFalse(TEXT("local light reports its actual unit enum"), Inspection->GetObjectField(TEXT("result"))->GetStringField(TEXT("intensity_units")).IsEmpty());
    TestEqual(TEXT("local light reports native influence radius"), Inspection->GetObjectField(TEXT("result"))->GetNumberField(TEXT("attenuation_radius_cm")), static_cast<double>(CastChecked<UPointLightComponent>(A->GetLightComponent())->AttenuationRadius));
    auto Plan = Tools.Execute(TEXT("workflow_preview"), Preview(Change), Identity()); if (!TestTrue(TEXT("native light preview"), Plan->GetBoolField(TEXT("ok")))) return false;
    const float Before = A->GetLightComponent()->Intensity; auto Applied = Tools.Execute(TEXT("workflow_apply"), Commit(Plan), Identity()); TestTrue(TEXT("light applies"), Applied->GetBoolField(TEXT("ok"))); TestTrue(TEXT("intensity and linear color verified"), Applied->GetObjectField(TEXT("result"))->GetBoolField(TEXT("readback_verified"))); TestEqual(TEXT("native intensity changed"), A->GetLightComponent()->Intensity, 321.0f);
    GEditor->UndoTransaction(); TestEqual(TEXT("light Undo"), A->GetLightComponent()->Intensity, Before);
    Plan = Tools.Execute(TEXT("workflow_preview"), Preview(Change), Identity()); if (!TestTrue(TEXT("stale plan created"), Plan->GetBoolField(TEXT("ok")))) return false;
    A->GetLightComponent()->SetIntensity(222); TestEqual(TEXT("unnnotified property edit detected"), Code(Tools.Execute(TEXT("workflow_apply"), Commit(Plan), Identity())), FString(TEXT("stale_plan")));
    Plan = Tools.Execute(TEXT("workflow_preview"), Preview(Change), Identity()); if (!TestTrue(TEXT("expiry plan"), Plan->GetBoolField(TEXT("ok")))) return false;
    Now += 121; TestEqual(TEXT("expired plan consumed"), Code(Tools.Execute(TEXT("workflow_apply"), Commit(Plan), Identity())), FString(TEXT("expired_plan")));
    auto Wrong = Preview(Change); Wrong->SetStringField(TEXT("expected_project"), TEXT("other")); TestEqual(TEXT("wrong project"), Code(Tools.Execute(TEXT("workflow_preview"), Wrong, Identity())), FString(TEXT("wrong_project")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainSurface, "Jev.Editor.DomainSurface", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainSurface::RunTest(const FString&)
{
    using namespace JevWorkflowTests; UWorld* W = FAutomationEditorCommonUtils::CreateNewMap(); auto* Floor = Cube(W, FVector(0, 0, -50), FVector(20, 20, 1)); auto* A = Cube(W, FVector(0, 0, 400), FVector(1));
    ON_SCOPE_EXIT { W->EditorDestroyActor(A, true); W->EditorDestroyActor(Floor, true); };
    FJevWorkflowTools Tools; auto Q = Query(TEXT("surface")); Q->SetStringField(TEXT("actor_path"), A->GetPathName()); Q->SetArrayField(TEXT("surface_paths"), {MakeShared<FJsonValueString>(Floor->GetPathName())}); Q->SetStringField(TEXT("trace_channel"), TEXT("visibility")); Q->SetNumberField(TEXT("trace_up_cm"), 100); Q->SetNumberField(TEXT("trace_down_cm"), 1000); Q->SetNumberField(TEXT("max_slope_degrees"), 30); Q->SetNumberField(TEXT("clearance_cm"), 1); Q->SetBoolField(TEXT("align_to_normal"), true);
    auto R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity()); if (!TestTrue(TEXT("trace responds"), R->GetBoolField(TEXT("ok")))) return false;
    TestTrue(TEXT("approved surface clear"), R->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid"))); TestEqual(TEXT("bounds supported above floor"), R->GetObjectField(TEXT("result"))->GetArrayField(TEXT("location"))[2]->AsNumber(), 51.0);
    for (const TCHAR* K : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) TestEqual(TEXT("surface response preserves inspected identity"), R->GetObjectField(TEXT("result"))->GetStringField(K), Identity()->GetStringField(K));
    TestEqual(TEXT("inspection preserves actor transform"), A->GetActorLocation().Z, 400.0);
    auto* Blocker = Cube(W, FVector(80, 0, 60), FVector(1)); ON_SCOPE_EXIT { W->EditorDestroyActor(Blocker, true); };
    R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity()); TestFalse(TEXT("overlap refuses placement"), R->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid")));
    Blocker->SetActorLocation(FVector(0, 0, 200)); R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity()); TestEqual(TEXT("unapproved first blocker refused"), R->GetObjectField(TEXT("result"))->GetStringField(TEXT("reason")), FString(TEXT("first_blocker_not_approved")));
    Blocker->SetActorLocation(FVector(5000)); Floor->SetActorRotation(FRotator(45, 0, 0)); R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity()); TestEqual(TEXT("slope limit"), R->GetObjectField(TEXT("result"))->GetStringField(TEXT("reason")), FString(TEXT("slope_exceeds_limit")));
    Floor->SetActorLocation(FVector(5000)); R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity()); TestFalse(TEXT("missing surface"), R->GetObjectField(TEXT("result"))->GetBoolField(TEXT("placement_valid")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainAssets, "Jev.Editor.DomainAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainAssets::RunTest(const FString&)
{
    using namespace JevWorkflowTests; FJevWorkflowTools Tools;
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); Registry.ScanPathsSynchronous({TEXT("/Engine/BasicShapes")}); LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto Q = Query(TEXT("asset_diagnosis"), TEXT("/Engine/BasicShapes/Cube.Cube")); Q->SetNumberField(TEXT("dependency_depth"), 3); auto R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity());
    if (!TestTrue(TEXT("mesh diagnosis"), R->GetBoolField(TEXT("ok")))) return false;
    TestTrue(TEXT("real render LOD"), R->GetObjectField(TEXT("result"))->GetNumberField(TEXT("lod_count")) > 0); TestTrue(TEXT("real collision shapes"), R->GetObjectField(TEXT("result"))->GetNumberField(TEXT("simple_collision_shapes")) > 0);
    for (const TCHAR* K : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) TestEqual(TEXT("asset response preserves inspected identity"), R->GetObjectField(TEXT("result"))->GetStringField(K), Identity()->GetStringField(K));
    TestTrue(TEXT("bounded dependencies"), R->GetObjectField(TEXT("result"))->GetArrayField(TEXT("dependencies")).Num() <= 128);
    Q->SetNumberField(TEXT("dependency_depth"), 4); TestEqual(TEXT("depth bounded"), Code(Tools.Execute(TEXT("workflow_inspect"), Q, Identity())), FString(TEXT("bad_request")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainRigWidgets, "Jev.Editor.DomainRigWidgets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainRigWidgets::RunTest(const FString&)
{
    using namespace JevWorkflowTests; FAssets Assets; FJevWorkflowTools Tools;
    auto* Skeleton = Assets.Make<USkeleton>(TEXT("Skeleton")); auto* Other = Assets.Make<USkeleton>(TEXT("OtherSkeleton"));
    { FReferenceSkeletonModifier Modifier(Skeleton); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); Modifier.Add(FMeshBoneInfo(TEXT("hand"), TEXT("hand"), 0), FTransform::Identity); }
    auto* Sequence = Assets.Make<UAnimSequence>(TEXT("Sequence")); Sequence->SetSkeleton(Other); Sequence->bEnableRootMotion = true;
    auto Q = Query(TEXT("rig"), Skeleton->GetPathName()); Q->SetStringField(TEXT("animation_path"), Sequence->GetPathName()); Q->SetArrayField(TEXT("required_bones"), {MakeShared<FJsonValueString>(TEXT("hand")), MakeShared<FJsonValueString>(TEXT("foot"))}); auto R = Tools.Execute(TEXT("workflow_inspect"), Q, Identity()); if (!TestTrue(TEXT("rig inspected"), R->GetBoolField(TEXT("ok")))) return false;
    auto Result = R->GetObjectField(TEXT("result")); TestEqual(TEXT("missing bone reported"), Result->GetArrayField(TEXT("missing_required_bones")).Num(), 1); TestFalse(TEXT("skeleton mismatch"), Result->GetBoolField(TEXT("exact_skeleton_match"))); TestTrue(TEXT("root motion flag"), Result->GetBoolField(TEXT("root_motion_enabled")));
    Sequence->SetSkeleton(Skeleton); TestTrue(TEXT("exact compatibility"), Tools.Execute(TEXT("workflow_inspect"), Q, Identity())->GetObjectField(TEXT("result"))->GetBoolField(TEXT("exact_skeleton_match")));
    auto* BP = Assets.Make<UWidgetBlueprint>(TEXT("Widget")); BP->WidgetTree = NewObject<UWidgetTree>(BP); auto* Root = BP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root")); BP->WidgetTree->RootWidget = Root;
    auto* Button = BP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Action")); Root->AddChild(Button); auto* Text = BP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label")); Text->SetText(FText::FromString(TEXT("Inspect me"))); Button->AddChild(Text);
    R = Tools.Execute(TEXT("workflow_inspect"), Query(TEXT("widgets"), BP->GetPathName()), Identity()); if (!TestTrue(TEXT("stored widget tree"), R->GetBoolField(TEXT("ok")))) return false;
    TestEqual(TEXT("three widget identities"), R->GetObjectField(TEXT("result"))->GetArrayField(TEXT("widgets")).Num(), 3); TestFalse(TEXT("no runtime construction"), R->GetObjectField(TEXT("result"))->GetBoolField(TEXT("runtime_instantiated")));
    TestFalse(TEXT("no Slate instance created by inspection"), Button->GetCachedWidget().IsValid());
    auto CachedRoot = Root->TakeWidget();
    auto* CanvasSlot = CastChecked<UCanvasPanelSlot>(Button->Slot);
    auto* LayoutProperty = FindFProperty<FStructProperty>(UCanvasPanelSlot::StaticClass(), TEXT("LayoutData"));
    auto* StoredLayout = LayoutProperty->ContainerPtrToValuePtr<FAnchorData>(CanvasSlot); StoredLayout->Offsets.Right = 123; StoredLayout->Offsets.Bottom = 45;
    auto CachedButton = Button->TakeWidget(); CachedButton->SetEnabled(false); CachedButton->SetVisibility(EVisibility::Collapsed);
    auto CachedText = StaticCastSharedRef<STextBlock>(Text->TakeWidget()); int32 BindingCalls = 0;
    CachedText->SetText(TAttribute<FText>::CreateLambda([&BindingCalls] { ++BindingCalls; return FText::FromString(TEXT("Bound Slate text")); }));
    BindingCalls = 0;
    R = Tools.Execute(TEXT("workflow_inspect"), Query(TEXT("widgets"), BP->GetPathName()), Identity());
    for (const auto& RowValue : R->GetObjectField(TEXT("result"))->GetArrayField(TEXT("widgets")))
    {
        const auto Row = RowValue->AsObject();
        if (Row->GetStringField(TEXT("name")) == TEXT("Action"))
        {
            TestTrue(TEXT("stored enabled ignores cached Slate value"), Row->GetBoolField(TEXT("stored_enabled")));
            TestEqual(TEXT("stored visibility ignores cached Slate value"), Row->GetNumberField(TEXT("stored_visibility")), static_cast<double>(ESlateVisibility::Visible));
            TestEqual(TEXT("stored canvas size ignores cached Slate slot"), Row->GetArrayField(TEXT("canvas_size"))[0]->AsNumber(), 123.0);
        }
        if (Row->GetStringField(TEXT("name")) == TEXT("Label")) TestEqual(TEXT("stored text ignores cached Slate binding"), Row->GetStringField(TEXT("stored_text")), FString(TEXT("Inspect me")));
    }
    TestEqual(TEXT("inspection never evaluates Slate text binding"), BindingCalls, 0);
    StoredLayout->Anchors.Maximum = FVector2D(1, 1); StoredLayout->Offsets.Right = 0; StoredLayout->Offsets.Bottom = 0;
    R = Tools.Execute(TEXT("workflow_inspect"), Query(TEXT("widgets"), BP->GetPathName()), Identity());
    for (const auto& RowValue : R->GetObjectField(TEXT("result"))->GetArrayField(TEXT("widgets"))) if (RowValue->AsObject()->GetStringField(TEXT("name")) == TEXT("Action"))
    {
        TestTrue(TEXT("stretched margins are not reported as actual size"), RowValue->AsObject()->HasTypedField<EJson::Null>(TEXT("canvas_size")));
        TestFalse(TEXT("zero stretch margins are not zero-sized widgets"), RowValue->AsObject()->GetBoolField(TEXT("zero_canvas_size")));
    }
    CachedText->SetText(FText::FromString(TEXT("Detached test binding")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainPerformance, "Jev.Editor.DomainPerformance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainPerformance::RunTest(const FString&)
{
    using namespace JevWorkflowTests; double Now = 100; FJevWorkflowTools Tools([&Now] { return Now; });
    auto P = Preview(Query(TEXT("unused"))); P->RemoveField(TEXT("change")); P->SetStringField(TEXT("protocol_id"), TEXT("fixture-constant")); P->SetNumberField(TEXT("sample_count"), 10);
    auto R = Tools.Execute(TEXT("performance_start"), P, Identity()); if (!TestTrue(TEXT("capture starts"), R->GetBoolField(TEXT("ok")))) return false;
    auto Job = MakeShared<FJsonObject>(); Job->SetStringField(TEXT("job_id"), R->GetObjectField(TEXT("result"))->GetStringField(TEXT("job_id")));
    TestEqual(TEXT("concurrent capture refused"), Code(Tools.Execute(TEXT("performance_start"), P, Identity())), FString(TEXT("job_busy")));
    for (int32 I = 0; I <= 10; ++I) { Now += 0.02; Tools.Tick(Identity(), 999); }
    R = Tools.Execute(TEXT("performance_job"), Job, Identity()); auto Result = R->GetObjectField(TEXT("result")); TestEqual(TEXT("complete sample"), Result->GetStringField(TEXT("status")), FString(TEXT("completed"))); TestTrue(TEXT("wall clock rather than caller delta"), FMath::IsNearlyEqual(Result->GetObjectField(TEXT("editor_tick_interval"))->GetNumberField(TEXT("mean_ms")), 20.0, 0.001));
    R = Tools.Execute(TEXT("performance_start"), P, Identity()); Job->SetStringField(TEXT("job_id"), R->GetObjectField(TEXT("result"))->GetStringField(TEXT("job_id"))); auto Changed = Identity(); Changed->SetStringField(TEXT("revision"), TEXT("changed")); Now += 0.02; Tools.Tick(Changed, 0); TestEqual(TEXT("scene change invalidates measurement"), Tools.Execute(TEXT("performance_job"), Job, Identity())->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("state_changed")));
    R = Tools.Execute(TEXT("performance_start"), P, Identity()); Job->SetStringField(TEXT("job_id"), R->GetObjectField(TEXT("result"))->GetStringField(TEXT("job_id"))); Job->SetStringField(TEXT("expected_project"), Identity()->GetStringField(TEXT("project_file"))); TestEqual(TEXT("cooperative cancellation"), Tools.Execute(TEXT("performance_cancel"), Job, Identity())->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("cancelled")));
    return true;
}

#endif
