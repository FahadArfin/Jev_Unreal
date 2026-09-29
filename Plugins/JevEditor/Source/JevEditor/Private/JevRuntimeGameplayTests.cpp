#if WITH_DEV_AUTOMATION_TESTS
#include "JevEditorRuntimeGameplay.h"
#include "JevEditorBridge.h"
#include "JevBlueprintGraphEdits.h"
#include "JevDoorGameplayFixture.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"

namespace JevRuntimeTests
{
const TCHAR* Section = TEXT("JevEditor.RuntimeGameplay");
TSharedRef<FJsonObject> Identity(FJevEditorBridge& Bridge)
{
    auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("action"), TEXT("status")); Request->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
    return Bridge.Execute(Request)->GetObjectField(TEXT("result")).ToSharedRef();
}
TSharedRef<FJsonObject> Preview(const TSharedRef<FJsonObject>& Current, const TCHAR* Operation, const FString& Owner = {})
{
    auto P = MakeShared<FJsonObject>(); auto Expected = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) Expected->SetStringField(Key, Current->GetStringField(Key));
    P->SetObjectField(TEXT("expected_state"), Expected); P->SetStringField(TEXT("expected_project"), Current->GetStringField(TEXT("project_file"))); P->SetStringField(TEXT("operation"), Operation);
    if (!Owner.IsEmpty()) P->SetStringField(TEXT("owned_session_id"), Owner); return P;
}
FString ErrorCode(const TSharedRef<FJsonObject>& R) { return R->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : R->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")); }
struct FState
{
    FAutomationTestBase* Test; FJevEditorBridge Bridge; FJevRuntimeGameplay Tools;
    bool HadEnabled = false, OldEnabled = false; TArray<FString> OldMaps;
    TWeakObjectPtr<AActor> OpenDoor, LockedDoor, Probe, Camera;
    bool bCapture = false; double SettledAt = 0; int32 SettledFrames = 0;
    UPackage* BlueprintPackage = nullptr; FString PlanId, Owner; int32 Stage = 0; double Began = FPlatformTime::Seconds();
    explicit FState(FAutomationTestBase* InTest) : Test(InTest) { HadEnabled = GConfig->GetBool(Section, TEXT("bEnabled"), OldEnabled, GGameIni); GConfig->GetArray(Section, TEXT("Maps"), OldMaps, GGameIni); }
    ~FState()
    {
        Tools.Shutdown(); if (HadEnabled) GConfig->SetBool(Section, TEXT("bEnabled"), OldEnabled, GGameIni); else GConfig->RemoveKey(Section, TEXT("bEnabled"), GGameIni); GConfig->SetArray(Section, TEXT("Maps"), OldMaps, GGameIni);
        if (BlueprintPackage) { BlueprintPackage->SetDirtyFlag(false); BlueprintPackage->RemoveFromRoot(); }
    }
    bool StartPlan(const TCHAR* Operation)
    {
        auto Current = Identity(Bridge); auto R = Tools.Execute(TEXT("runtime_preview"), Preview(Current, Operation, FString(Operation) == TEXT("stop") ? Owner : FString()), Current);
        if (!Test->TestTrue(TEXT("approved lifecycle preview"), R->GetBoolField(TEXT("ok")))) return false;
        PlanId = R->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")); auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("plan_id"), PlanId); P->SetStringField(TEXT("expected_project"), Current->GetStringField(TEXT("project_file")));
        R = Tools.Execute(TEXT("runtime_apply"), P, Current); if (!Test->TestTrue(TEXT("reviewed lifecycle applies"), R->GetBoolField(TEXT("ok")))) return false;
        Test->TestEqual(TEXT("lifecycle plans are one shot"), ErrorCode(Tools.Execute(TEXT("runtime_apply"), P, Current)), FString(TEXT("plan_consumed"))); return true;
    }
    bool DoorGraph(UWorld* World)
    {
        BlueprintPackage = CreatePackage(*(TEXT("/Game/JevDoorSource_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); BlueprintPackage->AddToRoot();
        auto* BP = FKismetEditorUtilities::CreateBlueprint(AJevDoorGameplayFixture::StaticClass(), BlueprintPackage, TEXT("BP_TriggerDoor"), BPTYPE_Normal, FName(TEXT("JevAutomation")));
        UEdGraph* Graph = BP->UbergraphPages[0];
        // Remove only automatically generated empty event nodes in this source-owned fixture.
        auto Initial = Graph->Nodes; for (UEdGraphNode* N : Initial) if (N->IsA<UK2Node_Event>()) FBlueprintEditorUtils::RemoveNode(BP, N, true);
        auto Make = [&](const TCHAR* Operation) { auto E = MakeShared<FJsonObject>(); E->SetStringField(TEXT("operation"), Operation); E->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString()); return E; };
        auto Apply = [&](const TSharedRef<FJsonObject>& E) -> UEdGraphNode* { FGuid Guid = FGuid::NewGuid(); FString Reason; if (!Test->TestTrue(TEXT("documented gameplay edit validates"), JevBlueprintGraph::Validate(BP, E, Reason, true)) || !Test->TestTrue(TEXT("documented gameplay edit applies"), JevBlueprintGraph::Apply(BP, E, Guid, true))) return nullptr; for (UEdGraphNode* N : Graph->Nodes) if (N->NodeGuid == Guid) return N; return nullptr; };
        auto V = Make(TEXT("add_variable")); V->SetStringField(TEXT("name"), TEXT("CanOpen")); V->SetStringField(TEXT("type"), TEXT("bool")); FString Reason;
        Test->TestFalse(TEXT("gameplay declaration denied without explicit approval"), JevBlueprintGraph::Validate(BP, V, Reason));
        if (!Test->TestTrue(TEXT("declares owned bool variable"), JevBlueprintGraph::Apply(BP, V, FGuid::NewGuid(), true))) return false;
        Test->TestFalse(TEXT("duplicate member refused"), JevBlueprintGraph::Validate(BP, V, Reason, true));
        auto Event = Make(TEXT("add_event")); Event->SetStringField(TEXT("event"), TEXT("ReceiveActorBeginOverlap")); Event->SetNumberField(TEXT("x"), 0); Event->SetNumberField(TEXT("y"), 0); auto* E = Apply(Event);
        auto Branch = Make(TEXT("add_branch")); Branch->SetNumberField(TEXT("x"), 240); Branch->SetNumberField(TEXT("y"), 0); auto* B = Apply(Branch);
        auto Get = Make(TEXT("add_variable_get")); Get->SetStringField(TEXT("name"), TEXT("CanOpen")); Get->SetNumberField(TEXT("x"), 0); Get->SetNumberField(TEXT("y"), 160); auto* G = Apply(Get);
        auto Call = Make(TEXT("add_actor_call")); Call->SetStringField(TEXT("function"), TEXT("K2_SetActorRelativeLocation")); Call->SetNumberField(TEXT("x"), 480); Call->SetNumberField(TEXT("y"), 0); Call->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(300)}); auto* C = Apply(Call);
        if (!E || !B || !G || !C) return false;
        Call->SetStringField(TEXT("function"), TEXT("ExecuteConsoleCommand")); Test->TestFalse(TEXT("arbitrary function denied"), JevBlueprintGraph::Validate(BP, Call, Reason, true));
        auto Link = [&](UEdGraphNode* Out, const TCHAR* OP, UEdGraphNode* In, const TCHAR* IP)
        {
            auto L = Make(TEXT("connect")); L->SetStringField(TEXT("output_node_id"), Out->NodeGuid.ToString()); L->SetStringField(TEXT("output_pin_id"), Out->FindPin(OP)->PinId.ToString()); L->SetStringField(TEXT("input_node_id"), In->NodeGuid.ToString()); L->SetStringField(TEXT("input_pin_id"), In->FindPin(IP)->PinId.ToString()); return Test->TestTrue(TEXT("reviewed typed gameplay link applies"), JevBlueprintGraph::Apply(BP, L, FGuid::NewGuid(), true));
        };
        if (!Link(E, TEXT("then"), B, TEXT("execute")) || !Link(G, TEXT("CanOpen"), B, TEXT("Condition")) || !Link(B, TEXT("then"), C, TEXT("execute"))) return false;
        FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
        if (!Test->TestTrue(TEXT("trigger-door graph compiles"), BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings)) return false;
        auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        BP->GeneratedClass->GetDefaultObject<AActor>()->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Cube);
        OpenDoor = World->SpawnActor<AActor>(BP->GeneratedClass, FVector(0,0,0), FRotator::ZeroRotator); LockedDoor = World->SpawnActor<AActor>(BP->GeneratedClass, FVector(1000,0,0), FRotator::ZeroRotator);
        auto* Property = FindFProperty<FBoolProperty>(BP->GeneratedClass, TEXT("CanOpen")); if (!OpenDoor.IsValid() || !LockedDoor.IsValid() || !Property) return false;
        Property->SetPropertyValue_InContainer(OpenDoor.Get(), true); Property->SetPropertyValue_InContainer(LockedDoor.Get(), false);
        auto* Mesh = World->SpawnActor<AStaticMeshActor>(FVector(0,500,0), FRotator::ZeroRotator); Probe = Mesh;
        Mesh->GetStaticMeshComponent()->SetStaticMesh(Cube); Mesh->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable); Mesh->GetStaticMeshComponent()->SetGenerateOverlapEvents(true); Mesh->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly); Mesh->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Overlap);
        if (bCapture)
        {
            auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(500,0,-140), FRotator::ZeroRotator); Floor->GetStaticMeshComponent()->SetStaticMesh(Cube); Floor->SetActorScale3D(FVector(18,10,.1)); Floor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            auto* Light = World->SpawnActor<ADirectionalLight>(FVector(500,-600,1000), FRotator(-45,-40,0)); Light->GetLightComponent()->SetMobility(EComponentMobility::Movable); Light->GetLightComponent()->SetIntensity(10);
            const FVector Eye(500,-1650,650), Target(500,0,150); auto* View = World->SpawnActor<ACameraActor>(Eye, (Target-Eye).Rotation()); Camera = View;
            View->GetCameraComponent()->SetFieldOfView(65); auto& PP = View->GetCameraComponent()->PostProcessSettings; PP.bOverride_AutoExposureMethod = true; PP.AutoExposureMethod = AEM_Manual; PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true; PP.AutoExposureApplyPhysicalCameraExposure = false; PP.bOverride_AutoExposureBias = true; PP.AutoExposureBias = 0;
        }
        return true;
    }
};
class FExercise : public IAutomationLatentCommand
{
    TSharedRef<FState> State;
public:
    explicit FExercise(TSharedRef<FState> In) : State(In) {}
    bool Update() override
    {
        auto& S = *State; auto Current = Identity(S.Bridge); S.Tools.Tick(Current);
        if (FPlatformTime::Seconds() - S.Began > 60) { S.Test->AddError(TEXT("Runtime source fixture exceeded 60 seconds.")); return true; }
        auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("plan_id"), S.PlanId); auto R = S.Tools.Execute(TEXT("runtime_receipt"), Params, Current); if (!R->GetBoolField(TEXT("ok"))) { S.Test->AddError(TEXT("Runtime receipt unavailable.")); return true; }
        const auto Receipt = R->GetObjectField(TEXT("result")); const FString Status = Receipt->GetStringField(TEXT("status"));
        if (S.Stage == 0)
        {
            if (Status == TEXT("starting")) return false;
            if (!S.Test->TestEqual(TEXT("approved runtime starts"), Status, FString(TEXT("running")))) return true;
            S.Owner = Receipt->GetStringField(TEXT("owned_session_id"));
            if (S.bCapture && GEditor->PlayWorld->GetFirstPlayerController()) GEditor->PlayWorld->GetFirstPlayerController()->SetViewTarget(EditorUtilities::GetSimWorldCounterpartActor(S.Camera.Get()));
            auto* Probe = EditorUtilities::GetSimWorldCounterpartActor(S.Probe.Get()); if (!Probe) return true;
            Probe->SetActorLocation(FVector(0,0,0)); ++S.Stage; return false;
        }
        if (S.Stage == 1)
        {
            auto* Open = EditorUtilities::GetSimWorldCounterpartActor(S.OpenDoor.Get()); auto* Locked = EditorUtilities::GetSimWorldCounterpartActor(S.LockedDoor.Get()); auto* Probe = EditorUtilities::GetSimWorldCounterpartActor(S.Probe.Get());
            if (!Open || !Locked || !Probe) { S.Test->AddError(TEXT("PIE fixture counterparts missing.")); return true; }
            S.Test->TestTrue(TEXT("real overlap opens permitted door through generated Blueprint"), Open->GetActorLocation().Equals(FVector(0,0,300), .01));
            Probe->SetActorLocation(FVector(1000,0,0)); ++S.Stage; return false;
        }
        if (S.Stage == 2)
        {
            auto* Locked = EditorUtilities::GetSimWorldCounterpartActor(S.LockedDoor.Get()); S.Test->TestTrue(TEXT("negative branch keeps locked door closed"), Locked && Locked->GetActorLocation().Equals(FVector(1000,0,0), .01));
            auto BadCapture = MakeShared<FJsonObject>(); BadCapture->SetStringField(TEXT("owned_session_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens)); S.Test->TestEqual(TEXT("other runtime cannot be captured"), ErrorCode(S.Tools.Execute(TEXT("runtime_capture"), BadCapture, Current)), FString(TEXT("session_not_owned")));
            FJevRuntimeGameplay Outsider; S.Test->TestEqual(TEXT("another bridge service cannot adopt or stop PIE"), ErrorCode(Outsider.Execute(TEXT("runtime_preview"), Preview(Current, TEXT("stop"), S.Owner), Current)), FString(TEXT("session_not_owned")));
            if (S.bCapture) { S.SettledAt = FPlatformTime::Seconds(); S.Stage = 3; return false; }
            if (!S.StartPlan(TEXT("stop"))) return true; S.Stage = 4; return false;
        }
        if (S.Stage == 3)
        {
            if (++S.SettledFrames < 8 || FPlatformTime::Seconds() - S.SettledAt < 1) return false;
            auto Capture = MakeShared<FJsonObject>(); Capture->SetStringField(TEXT("owned_session_id"), S.Owner); Capture->SetNumberField(TEXT("max_dimension"), 1024);
            const auto Response = S.Tools.Execute(TEXT("runtime_capture"), Capture, Current);
            if (S.Test->TestTrue(TEXT("owned PIE viewport frame captures"), Response->GetBoolField(TEXT("ok"))))
            {
                const auto Image = Response->GetObjectField(TEXT("result")); TArray<uint8> Png; FImage Decoded;
                S.Test->TestEqual(TEXT("capture identifies runtime instead of editor view"), Image->GetStringField(TEXT("source")), FString(TEXT("owned_pie_viewport")));
                S.Test->TestEqual(TEXT("capture binds exact owned world"), Image->GetStringField(TEXT("pie_world_path")), GEditor->PlayWorld->GetPathName());
                const bool Valid = FBase64::Decode(Image->GetStringField(TEXT("data")), Png) && FImageUtils::DecompressImage(Png.GetData(), Png.Num(), Decoded);
                if (S.Test->TestTrue(TEXT("runtime PNG decodes with native image library"), Valid))
                {
                    S.Test->TestTrue(TEXT("runtime PNG dimensions match receipt"), Decoded.SizeX == Image->GetIntegerField(TEXT("width")) && Decoded.SizeY == Image->GetIntegerField(TEXT("height")) && Decoded.SizeX >= 320 && Decoded.SizeY >= 180);
                    int32 Visible = 0; if (Decoded.Format == ERawImageFormat::BGRA8) { const auto* Pixels = reinterpret_cast<const FColor*>(Decoded.RawData.GetData()); for (int32 I = 0; I < Decoded.SizeX * Decoded.SizeY; ++I) if (Pixels[I].R + Pixels[I].G + Pixels[I].B > 48) ++Visible; }
                    S.Test->TestTrue(TEXT("runtime camera contains lit scene pixels"), Visible > 1000);
                    const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev")); IFileManager::Get().MakeDirectory(*Dir, true); S.Test->TestTrue(TEXT("runtime door evidence saved to fixed local path"), FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Dir, TEXT("RuntimeDoor.png"))));
                }
            }
            if (!S.StartPlan(TEXT("stop"))) return true; S.Stage = 4; return false;
        }
        if (Status == TEXT("stopping")) return false;
        S.Test->TestEqual(TEXT("reviewed stop completed"), Status, FString(TEXT("stopped"))); S.Test->TestNull(TEXT("owned PIE is gone"), GEditor->PlayWorld.Get()); return true;
    }
};
class FEnsureStopped : public IAutomationLatentCommand
{
    TSharedRef<FState> State; double Began = FPlatformTime::Seconds();
public:
    explicit FEnsureStopped(TSharedRef<FState> In) : State(In) {}
    bool Update() override
    {
        auto Current = Identity(State->Bridge); State->Tools.Tick(Current);
        if (FPlatformTime::Seconds() - Began > 60) { State->Test->AddError(TEXT("Owned runtime fixture cleanup exceeded 60 seconds; inspect editor.")); return true; }
        if (State->Tools.HasActiveJob()) return false;
        auto Status = State->Tools.Execute(TEXT("runtime_status"), MakeShared<FJsonObject>(), Current)->GetObjectField(TEXT("result"));
        if (!Status->GetBoolField(TEXT("owned_session"))) return true;
        State->Owner = Status->GetStringField(TEXT("owned_session_id"));
        return !State->StartPlan(TEXT("stop"));
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevRuntimeDoorWorkflow, "Jev.Editor.RuntimeDoorWorkflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevRuntimeDoorWorkflow::RunTest(const FString&)
{
    using namespace JevRuntimeTests;
    if (!TestFalse(TEXT("fixture never interrupts current PIE"), GEditor->IsPlaySessionInProgress())) return false;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap(); const FString Name = TEXT("JevRuntimeFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Package = CreatePackage(*(TEXT("/Game/") + Name)); World->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
    auto State = MakeShared<FState>(this); if (!State->DoorGraph(World)) return false;
    GConfig->SetArray(Section, TEXT("Maps"), {Package->GetName()}, GGameIni); GConfig->SetBool(Section, TEXT("bEnabled"), false, GGameIni);
    auto Current = Identity(State->Bridge); TestEqual(TEXT("runtime policy defaults closed"), ErrorCode(State->Tools.Execute(TEXT("runtime_preview"), Preview(Current, TEXT("start")), Current)), FString(TEXT("runtime_disabled")));
    GConfig->SetBool(Section, TEXT("bEnabled"), true, GGameIni);
    auto Stale = Preview(Current, TEXT("start")); Stale->GetObjectField(TEXT("expected_state"))->SetStringField(TEXT("revision"), TEXT("stale")); TestEqual(TEXT("stale runtime preview denied"), ErrorCode(State->Tools.Execute(TEXT("runtime_preview"), Stale, Current)), FString(TEXT("stale_plan")));
    if (!State->StartPlan(TEXT("start"))) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExercise>(State)); FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FEnsureStopped>(State)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevRuntimeDoorCapture, "Jev.Rendered.RuntimeDoorCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevRuntimeDoorCapture::RunTest(const FString&)
{
    using namespace JevRuntimeTests;
    if (!FApp::CanEverRender() || GEditor->IsPlaySessionInProgress()) { AddError(TEXT("Runtime door capture requires a rendered editor outside PIE.")); return false; }
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap(); const FString Name = TEXT("JevRuntimeCapture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits); UPackage* Package = CreatePackage(*(TEXT("/Game/") + Name)); World->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
    auto State = MakeShared<FState>(this); State->bCapture = true; if (!State->DoorGraph(World)) return false;
    GConfig->SetArray(Section, TEXT("Maps"), {Package->GetName()}, GGameIni); GConfig->SetBool(Section, TEXT("bEnabled"), true, GGameIni);
    if (!State->StartPlan(TEXT("start"))) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExercise>(State)); FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FEnsureStopped>(State)); return true;
}
#endif
