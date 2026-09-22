#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace JevDomainRendered
{
class FScenario : public IAutomationLatentCommand
{
public:
    explicit FScenario(FAutomationTestBase* InTest) : Test(InTest), Began(FPlatformTime::Seconds())
    {
        World = FAutomationEditorCommonUtils::CreateNewMap();
        Package = CreatePackage(*(TEXT("/Game/JevRenderedDomain_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); Package->AddToRoot();
        Parent = NewObject<UMaterial>(Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
        auto* Color = NewObject<UMaterialExpressionVectorParameter>(Parent); Color->ParameterName = TEXT("Tint"); Color->DefaultValue = FLinearColor::Red; Parent->GetExpressionCollection().AddExpression(Color); Parent->GetEditorOnlyData()->EmissiveColor.Expression = Color; Parent->SetShadingModel(MSM_Unlit); Parent->PostEditChange(); FAssetRegistryModule::AssetCreated(Parent);
        Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Public | RF_Standalone | RF_Transactional); Instance->SetParentEditorOnly(Parent); Instance->PostEditChange(); FAssetRegistryModule::AssetCreated(Instance);
        Actor = World->SpawnActor<AStaticMeshActor>(); Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"))); Actor->GetStaticMeshComponent()->SetMaterial(0, Instance);
        bHadEnabled = GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), bOldEnabled, GGameIni); GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), OldPaths, GGameIni);
        GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), true, GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), {Instance->GetPathName()}, GGameIni);
        OriginalView = JevWorkflow::Camera();
    }
    ~FScenario()
    {
        if (bHadEnabled) GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), bOldEnabled, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), GGameIni);
        GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), OldPaths, GGameIni);
        if (World.IsValid() && Actor.IsValid()) World->EditorDestroyActor(Actor.Get(), true);
        for (UObject* O : {static_cast<UObject*>(Instance), static_cast<UObject*>(Parent)}) { FAssetRegistryModule::AssetDeleted(O); O->ClearFlags(RF_Public | RF_Standalone); }
        Package->SetDirtyFlag(false); Package->RemoveFromRoot();
    }
    TSharedRef<FJsonObject> BridgeCall(const TCHAR* Action, TSharedRef<FJsonObject> P = MakeShared<FJsonObject>())
    {
        auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("action"), Action); R->SetObjectField(TEXT("params"), P); return Bridge.Execute(R);
    }
    bool Edit(const TSharedRef<FJsonObject>& Change)
    {
        const auto I = BridgeCall(TEXT("status"))->GetObjectField(TEXT("result"));
        auto P = MakeShared<FJsonObject>(); auto State = MakeShared<FJsonObject>(); for (const TCHAR* K : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) State->SetStringField(K, I->GetStringField(K)); P->SetObjectField(TEXT("expected_state"), State); P->SetStringField(TEXT("expected_project"), I->GetStringField(TEXT("project_file"))); P->SetObjectField(TEXT("change"), Change);
        auto Plan = Tools.Execute(TEXT("workflow_preview"), P, I.ToSharedRef()); if (!Test->TestTrue(TEXT("rendered domain preview"), Plan->GetBoolField(TEXT("ok")))) return false;
        auto Apply = MakeShared<FJsonObject>(); Apply->SetStringField(TEXT("plan_id"), Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"))); Apply->SetStringField(TEXT("expected_project"), I->GetStringField(TEXT("project_file")));
        return Test->TestTrue(TEXT("rendered domain apply"), Tools.Execute(TEXT("workflow_apply"), Apply, I.ToSharedRef())->GetBoolField(TEXT("ok")));
    }
    bool Capture(const TCHAR* File, bool Green)
    {
        auto P = MakeShared<FJsonObject>(); P->SetNumberField(TEXT("max_dimension"), 768); auto R = BridgeCall(TEXT("capture"), P); if (!Test->TestTrue(TEXT("real viewport capture"), R->GetBoolField(TEXT("ok")))) return false;
        TArray<uint8> Bytes; FBase64::Decode(R->GetObjectField(TEXT("result"))->GetStringField(TEXT("data")), Bytes);
        const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev")); IFileManager::Get().MakeDirectory(*Dir, true); Test->TestTrue(TEXT("rendered evidence saved"), FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Dir, File)));
        TArray<FColor> Pixels; GetViewportScreenShot(GCurrentLevelEditingViewportClient->Viewport, Pixels);
        int32 Desired = 0; for (const auto& C : Pixels) if (Green ? (C.G > 80 && C.G > C.R * 1.5) : (C.R > 80 && C.R > C.G * 1.5)) ++Desired;
        return Test->TestTrue(Green ? TEXT("green material actually rendered") : TEXT("red material actually rendered"), Desired > 500);
    }
    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Began > 90) { Test->AddError(TEXT("Rendered domain fixture exceeded 90 seconds.")); return true; }
        if (FAssetCompilingManager::Get().GetNumRemainingAssets() > 0) return false;
        if (Now - LastStep < 1) return false;
        LastStep = Now;
        if (Step == 0)
        {
            auto C = MakeShared<FJsonObject>(); C->SetStringField(TEXT("kind"), TEXT("camera")); C->SetArrayField(TEXT("location"), JevWorkflow::Vector(FVector(-300, 0, 140))); C->SetArrayField(TEXT("rotation"), JevWorkflow::Vector(FVector(-25, 0, 0))); C->SetNumberField(TEXT("fov_degrees"), 60);
            if (!Edit(C)) return true;
            Test->TestTrue(TEXT("camera FOV readback"), FMath::IsNearlyEqual(JevWorkflow::Camera()->GetObjectField(TEXT("result"))->GetNumberField(TEXT("fov_degrees")), 60.0)); ++Step; return false;
        }
        if (Step == 1)
        {
            if (!Capture(TEXT("DomainMaterialRed.png"), false)) return true;
            auto C = MakeShared<FJsonObject>(); C->SetStringField(TEXT("kind"), TEXT("material_vector")); C->SetStringField(TEXT("target_path"), Instance->GetPathName()); C->SetStringField(TEXT("parameter"), TEXT("Tint")); C->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1)});
            if (!Edit(C)) return true; ++Step; return false;
        }
        Capture(TEXT("DomainMaterialGreen.png"), true);
        if (OriginalView && OriginalView->GetBoolField(TEXT("ok")))
        {
            const auto Before = OriginalView->GetObjectField(TEXT("result")); auto C = MakeShared<FJsonObject>(); C->SetStringField(TEXT("kind"), TEXT("camera")); for (const TCHAR* K : {TEXT("location"), TEXT("rotation"), TEXT("fov_degrees")}) C->SetField(K, Before->TryGetField(K)); Edit(C);
            const auto After = JevWorkflow::Camera()->GetObjectField(TEXT("result")); Test->TestEqual(TEXT("camera restore FOV"), After->GetNumberField(TEXT("fov_degrees")), Before->GetNumberField(TEXT("fov_degrees")));
            FVector A, B; JevWorkflow::Vector(After, TEXT("location"), A); JevWorkflow::Vector(Before, TEXT("location"), B); Test->TestTrue(TEXT("camera restore position"), A.Equals(B, 0.001));
        }
        return true;
    }
private:
    FAutomationTestBase* Test; FJevEditorBridge Bridge; FJevWorkflowTools Tools;
    TWeakObjectPtr<UWorld> World; TWeakObjectPtr<AStaticMeshActor> Actor;
    UPackage* Package = nullptr; UMaterial* Parent = nullptr; UMaterialInstanceConstant* Instance = nullptr;
    TSharedPtr<FJsonObject> OriginalView; TArray<FString> OldPaths; bool bHadEnabled = false, bOldEnabled = false;
    double Began = 0, LastStep = 0; int32 Step = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevDomainRendered, "Jev.Rendered.DomainMaterialsCamera", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevDomainRendered::RunTest(const FString&)
{
    if (!FApp::CanEverRender()) { AddError(TEXT("A rendered editor is required.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(JevDomainRendered::FScenario(this)); return true;
}

#endif
