#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorWorkflowTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace JevEditingTests
{
TSharedRef<FJsonObject> Identity()
{
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("project_file"), TEXT("/fixture/JevSandbox.uproject")); R->SetStringField(TEXT("session_id"), TEXT("editing-session")); R->SetStringField(TEXT("world_path"), TEXT("/fixture/World")); R->SetStringField(TEXT("revision"), TEXT("editing-revision")); R->SetBoolField(TEXT("play_in_editor"), false); R->SetBoolField(TEXT("simulating"), false); return R;
}
TSharedRef<FJsonObject> Preview(FJevWorkflowTools& Tools, const TSharedRef<FJsonObject>& Change)
{
    auto R = MakeShared<FJsonObject>(); auto I = Identity(); auto S = MakeShared<FJsonObject>(); for (const TCHAR* K : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) S->SetStringField(K, I->GetStringField(K)); R->SetObjectField(TEXT("expected_state"), S); R->SetStringField(TEXT("expected_project"), I->GetStringField(TEXT("project_file"))); R->SetObjectField(TEXT("change"), Change); return Tools.Execute(TEXT("workflow_preview"), R, I);
}
TSharedRef<FJsonObject> Apply(FJevWorkflowTools& Tools, const TSharedRef<FJsonObject>& Plan)
{
    auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("plan_id"), Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"))); P->SetStringField(TEXT("expected_project"), Identity()->GetStringField(TEXT("project_file"))); return Tools.Execute(TEXT("workflow_apply"), P, Identity());
}
FString Code(const TSharedRef<FJsonObject>& R) { return R->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : R->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMaterialTextureSwitch, "Jev.Editor.MaterialTextureSwitch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMaterialTextureSwitch::RunTest(const FString&)
{
    using namespace JevEditingTests;
    auto* Package = CreatePackage(*(TEXT("/Game/JevMaterialExtension_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); Package->AddToRoot();
    auto* M = NewObject<UMaterial>(Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional); auto* Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Public | RF_Standalone | RF_Transactional);
    FAssetRegistryModule::AssetCreated(M); FAssetRegistryModule::AssetCreated(Instance);
    bool MaterialEnabled = false, SwitchEnabled = false; const bool HadMaterial = GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), MaterialEnabled, GGameIni), HadSwitch = GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableStaticSwitchEdits"), SwitchEnabled, GGameIni); TArray<FString> Materials, Textures; GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Materials, GGameIni); GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("AllowedMaterialTextures"), Textures, GGameIni);
    ON_SCOPE_EXIT {
        if (HadMaterial) GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), MaterialEnabled, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), GGameIni);
        if (HadSwitch) GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableStaticSwitchEdits"), SwitchEnabled, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.Workflows"), TEXT("bEnableStaticSwitchEdits"), GGameIni);
        GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Materials, GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("AllowedMaterialTextures"), Textures, GGameIni);
        for (UObject* O : {static_cast<UObject*>(Instance), static_cast<UObject*>(M)}) { FAssetRegistryModule::AssetDeleted(O); O->ClearFlags(RF_Public | RF_Standalone); } Package->SetDirtyFlag(false); Package->RemoveFromRoot();
    };
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); Registry.ScanPathsSynchronous({TEXT("/Engine/EngineResources")});
    auto* First = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")); auto* Second = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture")); if (!TestNotNull(TEXT("first native texture"), First) || !TestNotNull(TEXT("second native texture"), Second)) return false;
    auto* Texture = NewObject<UMaterialExpressionTextureSampleParameter2D>(M); Texture->ParameterName = TEXT("Albedo"); Texture->Texture = First; M->GetExpressionCollection().AddExpression(Texture); M->GetEditorOnlyData()->BaseColor.Expression = Texture;
    auto* Zero = NewObject<UMaterialExpressionConstant>(M); Zero->R = 0; auto* One = NewObject<UMaterialExpressionConstant>(M); One->R = 1; M->GetExpressionCollection().AddExpression(Zero); M->GetExpressionCollection().AddExpression(One);
    auto* Switch = NewObject<UMaterialExpressionStaticSwitchParameter>(M); Switch->ParameterName = TEXT("UseRough"); Switch->DefaultValue = false; Switch->A.Expression = One; Switch->B.Expression = Zero; M->GetExpressionCollection().AddExpression(Switch); M->GetEditorOnlyData()->Roughness.Expression = Switch; M->PostEditChange(); Instance->SetParentEditorOnly(M); Instance->PostEditChange();
    GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), true, GGameIni); GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableStaticSwitchEdits"), true, GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), {Instance->GetPathName()}, GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("AllowedMaterialTextures"), {Second->GetPathName()}, GGameIni);
    FJevWorkflowTools Tools; auto Change = MakeShared<FJsonObject>(); Change->SetStringField(TEXT("kind"), TEXT("material_texture")); Change->SetStringField(TEXT("target_path"), Instance->GetPathName()); Change->SetStringField(TEXT("parameter"), TEXT("Albedo")); Change->SetStringField(TEXT("value"), Second->GetPathName());
    auto Plan = Preview(Tools, Change); if (!TestTrue(TEXT("approved texture previews"), Plan->GetBoolField(TEXT("ok")))) return false; UTexture* Value = nullptr; Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Albedo")), Value); TestEqual(TEXT("preview preserves inherited texture"), Value, static_cast<UTexture*>(First));
    auto Done = Apply(Tools, Plan); if (!TestTrue(TEXT("texture apply responds"), Done->GetBoolField(TEXT("ok")))) return false; TestTrue(TEXT("exact texture readback verified"), Done->GetObjectField(TEXT("result"))->GetBoolField(TEXT("readback_verified"))); GEditor->UndoTransaction(); Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Albedo")), Value); TestEqual(TEXT("Undo restores inherited texture"), Value, static_cast<UTexture*>(First));
    Plan = Preview(Tools, Change); if (!TestTrue(TEXT("texture revoke preview"), Plan->GetBoolField(TEXT("ok")))) return false; GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("AllowedMaterialTextures"), {}, GGameIni); TestEqual(TEXT("texture permission rechecked"), Code(Apply(Tools, Plan)), FString(TEXT("policy_invalid")));
    Change->SetStringField(TEXT("kind"), TEXT("material_static_switch")); Change->SetStringField(TEXT("parameter"), TEXT("UseRough")); Change->SetBoolField(TEXT("value"), true); Plan = Preview(Tools, Change); if (!TestTrue(TEXT("static switch previews"), Plan->GetBoolField(TEXT("ok")))) return false;
    Done = Apply(Tools, Plan); if (!TestTrue(TEXT("static switch apply responds"), Done->GetBoolField(TEXT("ok")))) return false; TestTrue(TEXT("static switch readback verified"), Done->GetObjectField(TEXT("result"))->GetBoolField(TEXT("readback_verified"))); GEditor->UndoTransaction(); bool BoolValue = true; FGuid Guid; Instance->GetStaticSwitchParameterValue(FMaterialParameterInfo(TEXT("UseRough")), BoolValue, Guid); TestFalse(TEXT("Undo restores inherited static switch"), BoolValue);
    Change->SetStringField(TEXT("association"), TEXT("layer")); Change->SetNumberField(TEXT("index"), 0); TestEqual(TEXT("association cannot alias global parameter"), Code(Preview(Tools, Change)), FString(TEXT("bad_request"))); Change->SetStringField(TEXT("association"), TEXT("global")); Change->SetNumberField(TEXT("index"), -1);
    Plan = Preview(Tools, Change); if (!TestTrue(TEXT("switch revoke preview"), Plan->GetBoolField(TEXT("ok")))) return false; GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableStaticSwitchEdits"), false, GGameIni); TestEqual(TEXT("static switch permission rechecked"), Code(Apply(Tools, Plan)), FString(TEXT("policy_invalid")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevMaterialLayerParameter, "Jev.Editor.MaterialLayerParameter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevMaterialLayerParameter::RunTest(const FString&)
{
    using namespace JevEditingTests;
    auto* Package = CreatePackage(*(TEXT("/Game/JevMaterialLayer_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); Package->AddToRoot();
    auto* M = NewObject<UMaterial>(Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional); auto* Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Public | RF_Standalone | RF_Transactional); auto* Layer = NewObject<UMaterialFunctionMaterialLayer>(Package, TEXT("Layer"), RF_Public | RF_Standalone | RF_Transactional);
    for (UObject* O : {static_cast<UObject*>(M), static_cast<UObject*>(Instance), static_cast<UObject*>(Layer)}) FAssetRegistryModule::AssetCreated(O);
    bool Enabled = false; const bool Had = GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), Enabled, GGameIni); TArray<FString> Paths; GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Paths, GGameIni);
    ON_SCOPE_EXIT { if (Had) GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), Enabled, GGameIni); else GConfig->RemoveKey(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Paths, GGameIni); for (UObject* O : {static_cast<UObject*>(M), static_cast<UObject*>(Instance), static_cast<UObject*>(Layer)}) { FAssetRegistryModule::AssetDeleted(O); O->ClearFlags(RF_Public | RF_Standalone); } Package->SetDirtyFlag(false); Package->RemoveFromRoot(); };
    auto* Scalar = NewObject<UMaterialExpressionScalarParameter>(Layer); Scalar->ParameterName = TEXT("LayerRoughness"); Scalar->DefaultValue = 0.25f;
    auto* Attributes = NewObject<UMaterialExpressionMakeMaterialAttributes>(Layer); Attributes->Roughness.Expression = Scalar;
    auto* Output = NewObject<UMaterialExpressionFunctionOutput>(Layer); Output->OutputName = TEXT("Attributes"); Output->A.Expression = Attributes;
    for (UMaterialExpression* E : {static_cast<UMaterialExpression*>(Scalar), static_cast<UMaterialExpression*>(Attributes), static_cast<UMaterialExpression*>(Output)}) { E->Function = Layer; Layer->GetExpressionCollection().AddExpression(E); } Layer->PostEditChange();
    auto* Stack = NewObject<UMaterialExpressionMaterialAttributeLayers>(M); Stack->DefaultLayers.Layers[0] = Layer; M->GetExpressionCollection().AddExpression(Stack); M->bUseMaterialAttributes = true; M->GetEditorOnlyData()->MaterialAttributes.Expression = Stack; M->PostEditChange(); Instance->SetParentEditorOnly(M); Instance->PostEditChange();
    const FMaterialParameterInfo Info(TEXT("LayerRoughness"), EMaterialParameterAssociation::LayerParameter, 0); TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids; Instance->GetAllScalarParameterInfo(Infos, Ids); if (!TestTrue(TEXT("real layer parameter exposed"), Infos.Contains(Info))) return false;
    GConfig->SetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), true, GGameIni); GConfig->SetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), {Instance->GetPathName()}, GGameIni);
    FJevWorkflowTools Tools; auto Change = MakeShared<FJsonObject>(); Change->SetStringField(TEXT("kind"), TEXT("material_scalar")); Change->SetStringField(TEXT("target_path"), Instance->GetPathName()); Change->SetStringField(TEXT("parameter"), TEXT("LayerRoughness")); Change->SetStringField(TEXT("association"), TEXT("layer")); Change->SetNumberField(TEXT("index"), 0); Change->SetNumberField(TEXT("value"), 0.75);
    auto Plan = Preview(Tools, Change); if (!TestTrue(TEXT("existing layer parameter previews"), Plan->GetBoolField(TEXT("ok")))) return false; auto Done = Apply(Tools, Plan); if (!TestTrue(TEXT("layer parameter applied"), Done->GetBoolField(TEXT("ok")))) return false;
    TestTrue(TEXT("layer value verified"), Done->GetObjectField(TEXT("result"))->GetBoolField(TEXT("readback_verified"))); float Value = 0; Instance->GetScalarParameterValue(Info, Value); TestEqual(TEXT("actual layer scalar"), Value, 0.75f); GEditor->UndoTransaction(); Instance->GetScalarParameterValue(Info, Value); TestEqual(TEXT("Undo restores layer inherited value"), Value, 0.25f);
    Change->SetNumberField(TEXT("index"), 1); TestEqual(TEXT("another layer cannot be targeted implicitly"), Code(Preview(Tools, Change)), FString(TEXT("bad_request")));
    return true;
}

namespace JevEditingTests
{
class FCameraRenderScenario : public IAutomationLatentCommand
{
public:
    explicit FCameraRenderScenario(FAutomationTestBase* InTest) : Test(InTest), Began(FPlatformTime::Seconds()), StartedFrame(GFrameCounter)
    {
        auto* Settings = GetMutableDefault<UEditorPerformanceSettings>();
        PreviousThrottle = Settings->bThrottleCPUWhenNotForeground;
        Settings->bThrottleCPUWhenNotForeground = false;
    }
    virtual ~FCameraRenderScenario() override
    {
        GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = PreviousThrottle;
    }
    virtual bool Update() override
    {
    // Let the editor remove its own Background Process realtime override after
    // disabling fixture throttling. Never pop another subsystem's override.
    if (GFrameCounter <= StartedFrame + 1 && FPlatformTime::Seconds() - Began < 5) return false;
    auto* V = GCurrentLevelEditingViewportClient;
    if (!Test->TestNotNull(TEXT("rendered level viewport"), V) || !Ok(TEXT("camera inspect available"), JevWorkflow::Camera())) return true;
    if (V->IsRealtimeOverrideSet())
    {
        Test->AddError(TEXT("Camera fixture still has a foreign realtime override: ") + V->GetRealtimeOverrideMessage().ToString());
        return true;
    }
    const auto Exposure = V->ExposureSettings; const EViewModeIndex Mode = V->GetViewMode(); const auto Flags = V->EngineShowFlags; bool Realtime = false; V->SaveRealtimeStateToConfig(Realtime);
    const FText OverrideOwner = FText::FromString(TEXT("Jev camera automation"));
    ON_SCOPE_EXIT { if (V->HasRealtimeOverride(OverrideOwner)) V->RemoveRealtimeOverride(OverrideOwner); V->ExposureSettings = Exposure; V->SetViewMode(Mode); V->EngineShowFlags = Flags; V->SetRealtime(Realtime); V->Invalidate(); };
    V->SetViewMode(VMI_Lit); V->ExposureSettings.bFixed = false; V->ExposureSettings.FixedEV100 = 4;
    FJevWorkflowTools Tools; auto Change = MakeShared<FJsonObject>(); Change->SetStringField(TEXT("kind"), TEXT("camera_render")); Change->SetStringField(TEXT("exposure_mode"), TEXT("fixed")); Change->SetNumberField(TEXT("fixed_ev100"), 7.5); Change->SetStringField(TEXT("view_mode"), TEXT("unlit")); Change->SetBoolField(TEXT("realtime"), false); Change->SetBoolField(TEXT("motion_blur"), false);
    V->SetViewMode(VMI_Wireframe); Test->TestEqual(TEXT("unsupported initial mode remains protected"), Code(Preview(Tools, Change)), FString(TEXT("viewport_unavailable"))); V->SetViewMode(VMI_Lit);
    auto Plan = Preview(Tools, Change); if (!Ok(TEXT("reviewed camera render settings"), Plan)) return true;
    auto Done = Apply(Tools, Plan); if (!Ok(TEXT("camera render settings applied"), Done)) return true; Test->TestTrue(TEXT("camera native readback verified"), Done->GetObjectField(TEXT("result"))->GetBoolField(TEXT("readback_verified"))); Test->TestEqual(TEXT("fixed EV100 applied"), V->ExposureSettings.FixedEV100, 7.5f); Test->TestTrue(TEXT("fixed exposure enabled"), V->ExposureSettings.bFixed); Test->TestEqual(TEXT("unlit mode applied"), V->GetViewMode(), VMI_Unlit); Test->TestFalse(TEXT("motion blur disabled"), static_cast<bool>(V->EngineShowFlags.MotionBlur));
    Test->TestEqual(TEXT("camera settings are one-shot"), Code(Apply(Tools, Plan)), FString(TEXT("plan_consumed")));
    Plan = Preview(Tools, Change); if (!Ok(TEXT("camera stale plan"), Plan)) return true; V->ExposureSettings.FixedEV100 = 8; Test->TestEqual(TEXT("unnotified exposure change detected"), Code(Apply(Tools, Plan)), FString(TEXT("stale_plan")));
    V->AddRealtimeOverride(true, OverrideOwner); Test->TestEqual(TEXT("foreign realtime ownership protected"), Code(Preview(Tools, Change)), FString(TEXT("editor_busy")));
    return true;
    }
private:
    bool Ok(const TCHAR* Step, const TSharedRef<FJsonObject>& Response)
    {
        if (Response->GetBoolField(TEXT("ok"))) return true;
        const auto Error = Response->GetObjectField(TEXT("error"));
        Test->AddError(FString(Step) + TEXT(": ") + Error->GetStringField(TEXT("code")) + TEXT(": ") + Error->GetStringField(TEXT("message")));
        return false;
    }
    FAutomationTestBase* Test;
    double Began;
    uint64 StartedFrame;
    bool PreviousThrottle = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevCameraRenderSettings, "Jev.Rendered.CameraRenderSettings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevCameraRenderSettings::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(JevEditingTests::FCameraRenderScenario(this));
    return true;
}

#endif
