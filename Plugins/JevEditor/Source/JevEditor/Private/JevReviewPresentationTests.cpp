#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorReviewPresentation.h"
#include "JevEditorBridge.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"

namespace JevPresentationTests
{
using FObject = TSharedPtr<FJsonObject>;

TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const TCHAR* Action, const TSharedRef<FJsonObject>& Params)
{
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), Action);
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

FObject Receipt(FJevEditorBridge& Bridge, const TSharedRef<FJsonObject>& Operation)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Operation)});
    const auto Response = Call(Bridge, TEXT("preview"), Params);
    if (!Response->GetBoolField(TEXT("ok"))) return nullptr;
    auto Lookup = MakeShared<FJsonObject>();
    Lookup->SetStringField(TEXT("plan_id"), Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")));
    const auto Status = Call(Bridge, TEXT("plan_status"), Lookup);
    return Status->GetBoolField(TEXT("ok")) ? Status->GetObjectField(TEXT("result")) : nullptr;
}

TSharedRef<FJsonObject> Operation(const TCHAR* Op, AActor* Actor = nullptr)
{
    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("op"), Op);
    if (Actor) Out->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    return Out;
}

FObject Clone(const FObject& Source)
{
    FString Json;
    FJsonSerializer::Serialize(Source.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
    FObject Out;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out);
    return Out;
}

FObject FirstOperation(const FObject& Record) { return Record->GetObjectField(TEXT("review"))->GetArrayField(TEXT("operations"))[0]->AsObject(); }
FObject FirstBefore(const FObject& Record) { return Record->GetObjectField(TEXT("review"))->GetArrayField(TEXT("before"))[0]->AsObject(); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewPresentationTest, "Jev.Editor.ReviewPresentation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewPresentationTest::RunTest(const FString& Parameters)
{
    using namespace JevPresentationTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Presentation fixture has an isolated editor world"), World)) return false;
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!TestTrue(TEXT("Installed engine fixture assets are available"), Cube && Sphere && Material)) return false;
    AStaticMeshActor* Source = World->SpawnActor<AStaticMeshActor>();
    if (!TestNotNull(TEXT("Native source actor created"), Source)) return false;
    Source->GetStaticMeshComponent()->SetStaticMesh(Cube);
    Source->SetActorLabel(TEXT("Review fixture"));
    Source->SetActorLocation(FVector(100, 200, 300));
    Source->SetFolderPath(TEXT("Jev/Review"));
    const FTransform SourceTransform = Source->GetActorTransform();
    FJevEditorBridge Bridge;
    TMap<FString, FObject> Records;
    TArray<TSharedRef<FJsonObject>> Operations;
    auto Primitive = Operation(TEXT("spawn_primitive"));
    Primitive->SetStringField(TEXT("shape"), TEXT("Cube"));
    Primitive->SetStringField(TEXT("label"), TEXT("Review primitive"));
    Operations.Add(Primitive);
    auto Static = Operation(TEXT("spawn_static_mesh"));
    Static->SetStringField(TEXT("asset_path"), Cube->GetPathName());
    Static->SetStringField(TEXT("label"), TEXT("Review mesh"));
    Operations.Add(Static);
    auto Transform = Operation(TEXT("set_transform"), Source);
    Transform->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(110), MakeShared<FJsonValueNumber>(180), MakeShared<FJsonValueNumber>(330)});
    Operations.Add(Transform);
    auto Metadata = Operation(TEXT("set_metadata"), Source);
    Metadata->SetStringField(TEXT("label"), TEXT("Reviewed label"));
    Metadata->SetStringField(TEXT("folder"), TEXT(""));
    Operations.Add(Metadata);
    auto Assign = Operation(TEXT("set_material"), Source);
    Assign->SetNumberField(TEXT("slot"), 0);
    Assign->SetStringField(TEXT("material_path"), Material->GetPathName());
    Operations.Add(Assign);
    auto Replace = Operation(TEXT("replace_mesh"), Source);
    Replace->SetStringField(TEXT("asset_path"), Sphere->GetPathName());
    Replace->SetStringField(TEXT("material_policy"), TEXT("mesh_defaults"));
    Operations.Add(Replace);
    auto Duplicate = Operation(TEXT("duplicate_mesh"), Source);
    Duplicate->SetStringField(TEXT("label"), TEXT("Reviewed copy"));
    Duplicate->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(400), MakeShared<FJsonValueNumber>(200), MakeShared<FJsonValueNumber>(300)});
    Operations.Add(Duplicate);
    for (const auto& Input : Operations)
    {
        const FString Kind = Input->GetStringField(TEXT("op"));
        const FObject Record = Receipt(Bridge, Input);
        if (!TestTrue(FString::Printf(TEXT("Native receipt available: %s"), *Kind), Record.IsValid())) return false;
        Records.Add(Kind, Record);
        const auto Presentation = FJevEditorReviewPresentation::Build(Record);
        TestTrue(FString::Printf(TEXT("Native operation has complete review: %s"), *Kind), Presentation.bValid);
        const FString Body = Presentation.Body.ToString();
        TestTrue(TEXT("Before and after are explicit"), Body.Contains(TEXT("Before:")) && Body.Contains(TEXT("After:")));
        TestTrue(TEXT("Plan identity is retained"), Presentation.Summary.ToString().Contains(Record->GetStringField(TEXT("plan_id"))));
        TestTrue(TEXT("Exact project identity is retained"), Presentation.Summary.ToString().Contains(Record->GetObjectField(TEXT("review"))->GetStringField(TEXT("project_file"))));
        TestTrue(TEXT("One-shot, unsaved and fresh verification limits are visible"), Body.Contains(TEXT("once")) && Body.Contains(TEXT("never saves")) && Body.Contains(TEXT("Fresh verification")));
        const auto Later = Clone(Record);
        Later->SetNumberField(TEXT("expires_in_seconds"), 12);
        Later->SetStringField(TEXT("status"), TEXT("expired"));
        const auto LaterPresentation = FJevEditorReviewPresentation::Build(Later);
        TestTrue(TEXT("Historical expired receipt remains reviewable"), LaterPresentation.bValid);
        TestEqual(TEXT("Countdown and status refresh does not replace body text"), LaterPresentation.Body.ToString(), Body);
        TestEqual(TEXT("Countdown and status refresh does not replace identity summary"), LaterPresentation.Summary.ToString(), Presentation.Summary.ToString());
    }
    TestTrue(TEXT("Presentation and preview never apply a transform"), Source->GetActorTransform().Equals(SourceTransform));
    TestEqual(TEXT("Presentation never changes label"), Source->GetActorLabel(), FString(TEXT("Review fixture")));
    const FString TransformBody = FJevEditorReviewPresentation::Build(Records[TEXT("set_transform")]).Body.ToString();
    TestTrue(TEXT("Transform values and units are readable"), TransformBody.Contains(TEXT("100.00, 200.00, 300.00")) && TransformBody.Contains(TEXT("110.00, 180.00, 330.00")) && TransformBody.Contains(TEXT("pitch, yaw, roll")) && TransformBody.Contains(TEXT("cm")));
    TestTrue(TEXT("Root folder is explicit"), FJevEditorReviewPresentation::Build(Records[TEXT("set_metadata")]).Body.ToString().Contains(TEXT("(root folder)")));
    auto MeshPresentation = FJevEditorReviewPresentation::Build(Records[TEXT("replace_mesh")]);
    TestTrue(TEXT("Replacement discloses material defaults and collision geometry"), MeshPresentation.Body.ToString().Contains(TEXT("mesh_defaults")) && MeshPresentation.Body.ToString().Contains(TEXT("collision shapes")));
    TestTrue(TEXT("Human summary excludes raw technical field names"), !MeshPresentation.Body.ToString().Contains(TEXT("collision_responses")));
    TestTrue(TEXT("Complete mesh details are separate"), MeshPresentation.TechnicalDetails.ToString().Contains(TEXT("collision_responses")) && MeshPresentation.TechnicalDetails.ToString().Contains(TEXT("override_path")) && MeshPresentation.TechnicalDetails.ToString().Contains(TEXT("body_guid")));
    const FString CopyBody = FJevEditorReviewPresentation::Build(Records[TEXT("duplicate_mesh")]).Body.ToString();
    TestTrue(TEXT("Copy clearly distinguishes source from new identity"), CopyBody.Contains(TEXT("source stays unchanged")) && CopyBody.Contains(TEXT("assigned during apply")) && CopyBody.Contains(TEXT("Reviewed copy")));
    TestTrue(TEXT("Absent explicit override is not an absent effective material"), CopyBody.Contains(TEXT("(no explicit override; mesh default)")) && CopyBody.Contains(Cube->GetMaterial(0)->GetPathName()));
    Replace->SetStringField(TEXT("material_policy"), TEXT("preserve_slots"));
    const auto PreserveRecord = Receipt(Bridge, Replace);
    if (!TestTrue(TEXT("Native preserve-slots receipt available"), PreserveRecord.IsValid())) return false;
    const auto PreservePresentation = FJevEditorReviewPresentation::Build(PreserveRecord);
    TestTrue(TEXT("Preserve-slots is reviewable and explains slot-index mapping"), PreservePresentation.bValid && PreservePresentation.Body.ToString().Contains(TEXT("equal slot index")));
    auto SmallMove = Clone(Records[TEXT("set_transform")]);
    FirstOperation(SmallMove)->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(0.000123456789), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)});
    TestTrue(TEXT("Small nonzero position is not rounded to zero"), FJevEditorReviewPresentation::Build(SmallMove).Body.ToString().Contains(TEXT("0.000123456789")));
    auto EscapedLabel = Clone(Records[TEXT("set_transform")]);
    FirstBefore(EscapedLabel)->SetStringField(TEXT("label"), TEXT("A\nAfter: hidden change"));
    TestTrue(TEXT("Multiline actor data cannot impersonate a review row"), FJevEditorReviewPresentation::Build(EscapedLabel).Body.ToString().Contains(TEXT("A\\nAfter: hidden change")));

    TestFalse(TEXT("Null record fails closed"), FJevEditorReviewPresentation::Build(nullptr).bValid);
    const auto Reject = [this](const TCHAR* Name, const FObject& Record) { TestFalse(Name, FJevEditorReviewPresentation::Build(Record).bValid); };
    auto Broken = Clone(Records[TEXT("spawn_primitive")]);
    Broken->GetObjectField(TEXT("review"))->RemoveField(TEXT("before"));
    Reject(TEXT("Absent before array cannot be inferred"), Broken);
    Broken = Clone(Records[TEXT("spawn_primitive")]);
    FirstOperation(Broken)->SetStringField(TEXT("op"), TEXT("execute_python"));
    Reject(TEXT("Unsupported operation is not presented as a safe edit"), Broken);
    Broken = Clone(Records[TEXT("spawn_primitive")]);
    FirstOperation(Broken)->SetStringField(TEXT("unreviewed_setting"), TEXT("unknown future behavior"));
    Reject(TEXT("Unknown operation fields cannot hide from review"), Broken);
    Broken = Clone(Records[TEXT("spawn_primitive")]);
    FirstOperation(Broken)->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueString>(TEXT("2")), MakeShared<FJsonValueNumber>(3)});
    Reject(TEXT("Non-number transform component is rejected"), Broken);
    Broken = Clone(Records[TEXT("spawn_primitive")]);
    Broken->GetObjectField(TEXT("review"))->SetStringField(TEXT("unknown_large_field"), FString::ChrN(4097, TEXT('x')));
    Reject(TEXT("Oversized unknown data is bounded before serialization"), Broken);
    Broken = Clone(Records[TEXT("spawn_primitive")]);
    FObject Nested = Broken->GetObjectField(TEXT("review"));
    for (int32 Depth = 0; Depth < 12; ++Depth) { auto Next = MakeShared<FJsonObject>(); Nested->SetObjectField(TEXT("nested"), Next); Nested = Next; }
    Reject(TEXT("Excessive nesting is rejected"), Broken);
    Broken = Clone(Records[TEXT("set_transform")]);
    FirstBefore(Broken)->SetStringField(TEXT("path"), TEXT("/DifferentActor"));
    Reject(TEXT("Mismatched before identity is rejected"), Broken);
    Broken = Clone(Records[TEXT("set_material")]);
    FirstBefore(Broken)->RemoveField(TEXT("material_path"));
    Reject(TEXT("Missing material is not silently presented as null"), Broken);
    auto NullMaterial = Clone(Records[TEXT("set_material")]);
    FirstBefore(NullMaterial)->SetField(TEXT("material_path"), MakeShared<FJsonValueNull>());
    TestTrue(TEXT("Explicit null material is distinguishable"), FJevEditorReviewPresentation::Build(NullMaterial).bValid && FJevEditorReviewPresentation::Build(NullMaterial).Body.ToString().Contains(TEXT("(no material)")));
    Broken = Clone(Records[TEXT("replace_mesh")]);
    FirstOperation(Broken)->SetStringField(TEXT("material_policy"), TEXT("automatic"));
    Reject(TEXT("Unknown material policy is rejected"), Broken);
    Broken = Clone(Records[TEXT("replace_mesh")]);
    FirstOperation(Broken)->GetObjectField(TEXT("mesh_settings"))->RemoveField(TEXT("use_mesh_default_collision"));
    Reject(TEXT("Incomplete collision inheritance is rejected"), Broken);
    Broken = Clone(Records[TEXT("replace_mesh")]);
    FirstOperation(Broken)->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(3)});
    Reject(TEXT("Replacement cannot claim unchanged transform for a contradictory record"), Broken);
    Broken = Clone(Records[TEXT("replace_mesh")]);
    FirstOperation(Broken)->GetArrayField(TEXT("materials"))[0]->AsObject()->RemoveField(TEXT("override_path"));
    Reject(TEXT("Absent material override field is not inferred"), Broken);
    Broken = Clone(Records[TEXT("duplicate_mesh")]);
    FirstOperation(Broken)->SetStringField(TEXT("source_instance_id"), TEXT("different-instance"));
    Reject(TEXT("Mismatched copy source identity is rejected"), Broken);
    Broken = Clone(Records[TEXT("duplicate_mesh")]);
    FirstOperation(Broken)->GetObjectField(TEXT("mesh_settings"))->SetBoolField(TEXT("tags_truncated"), true);
    Reject(TEXT("Truncated reviewed state is rejected"), Broken);
    Broken = Clone(Records[TEXT("duplicate_mesh")]);
    FirstOperation(Broken)->SetNumberField(TEXT("material_override_count"), 2);
    Reject(TEXT("Override entries beyond asset slots are not silently hidden"), Broken);
    // Current execution deliberately refuses this scope: the component slot
    // count follows the mesh, while SetMaterial can grow an override array.
    Source->GetStaticMeshComponent()->SetMaterial(1, Material);
    TestEqual(TEXT("Fixture has an extra override beyond one mesh slot"), Source->GetStaticMeshComponent()->GetNumOverrideMaterials(), 2);
    TestFalse(TEXT("Native copy refuses excess overrides before creating a receipt"), Receipt(Bridge, Duplicate).IsValid());
    TestFalse(TEXT("Native replacement refuses excess overrides before creating a receipt"), Receipt(Bridge, Replace).IsValid());
    Source->GetStaticMeshComponent()->EmptyOverrideMaterials();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewRecoveryTest, "Jev.Editor.ReviewRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewRecoveryTest::RunTest(const FString& Parameters)
{
    const FString Unknown = FJevEditorReviewPresentation::RecoveryMessage(TEXT("rollback_failed"), TEXT("Native diagnostic"), true).ToString();
    TestTrue(TEXT("Unknown outcome and original diagnostic remain explicit"), Unknown.Contains(TEXT("rollback_failed: Native diagnostic")) && Unknown.Contains(TEXT("outcome is unknown")) && Unknown.Contains(TEXT("Do not assume")));
    TestTrue(TEXT("Apply failures never promise a retry"), Unknown.Contains(TEXT("No automatic retry")));
    TestFalse(TEXT("Unknown outcome never claims nothing changed"), Unknown.Contains(TEXT("nothing changed")) || Unknown.Contains(TEXT("no changes were made")));
    const FString Missing = FJevEditorReviewPresentation::RecoveryMessage(TEXT("unknown_plan"), TEXT("Plan missing"), true).ToString();
    TestTrue(TEXT("Missing receipt is not evidence of nonexecution"), Missing.Contains(TEXT("does not prove whether an edit occurred")));
    const FString Stale = FJevEditorReviewPresentation::RecoveryMessage(TEXT("stale_plan"), TEXT("Changed scene"), true).ToString();
    TestTrue(TEXT("Stale plan recovery requires fresh inspection and preview"), Stale.Contains(TEXT("Inspect the current actors")) && Stale.Contains(TEXT("create a new preview")));
    const FString Selection = FJevEditorReviewPresentation::RecoveryMessage(TEXT("selection_empty"), TEXT("Select actors"), false).ToString();
    TestTrue(TEXT("Selection error has actionable bounded guidance"), Selection.Contains(TEXT("one to 20")) && Selection.Contains(TEXT("World Outliner")));
    TestFalse(TEXT("Read failure does not imply an apply attempt"), Selection.Contains(TEXT("retry")));
    const FString Huge = FJevEditorReviewPresentation::RecoveryMessage(FString::ChrN(1000, TEXT('x')), FString::ChrN(10000, TEXT('x')), true).ToString();
    TestTrue(TEXT("Unexpected error strings are bounded"), Huge.Len() < 5000);
    return true;
}

#endif
