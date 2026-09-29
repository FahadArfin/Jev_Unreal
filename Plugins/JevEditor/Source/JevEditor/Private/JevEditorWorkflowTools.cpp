#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Editor.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "Engine/Level.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "LevelUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace JevWorkflow
{
bool Only(const TSharedPtr<FJsonObject>& P, const TArray<FString>& Keys)
{
    if (!P) return false;
    for (const auto& Pair : P->Values) if (!Keys.Contains(FString(*Pair.Key))) return false;
    return true;
}
bool Text(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, FString& Value, int32 Max)
{
    if (!P || !P->HasTypedField<EJson::String>(Key) || !P->TryGetStringField(Key, Value) || Value.IsEmpty() || Value.Len() > Max) return false;
    for (TCHAR C : Value) if (FChar::IsControl(C)) return false;
    return true;
}
bool Number(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, double& Value, double Min, double Max)
{
    return P && P->HasTypedField<EJson::Number>(Key) && P->TryGetNumberField(Key, Value) && FMath::IsFinite(Value) && Value >= Min && Value <= Max;
}
bool Vector(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, FVector& Value, double Bound)
{
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!P || !P->TryGetArrayField(Key, Items) || Items->Num() != 3) return false;
    for (int32 I = 0; I < 3; ++I)
    {
        double V = 0;
        if (!(*Items)[I] || (*Items)[I]->Type != EJson::Number || !(*Items)[I]->TryGetNumber(V) || !FMath::IsFinite(V) || FMath::Abs(V) > Bound) return false;
        Value[I] = V;
    }
    return true;
}
TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V) { return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)}; }
TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& R)
{
    auto Out = MakeShared<FJsonObject>(); Out->SetBoolField(TEXT("ok"), true); Out->SetObjectField(TEXT("result"), R); return Out;
}
TSharedRef<FJsonObject> Base(const TSharedRef<FJsonObject>& Identity)
{
    auto R = MakeShared<FJsonObject>();
    for (const TCHAR* K : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) R->SetStringField(K, Identity->GetStringField(K));
    R->SetBoolField(TEXT("cloud_used"), false); return R;
}
bool Same(const TSharedPtr<FJsonObject>& Before, const TSharedRef<FJsonObject>& Now)
{
    if (!Before) return false;
    for (const TCHAR* K : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        FString A, B;
        if (!Text(Before, K, A, 4096) || !Text(Now, K, B, 4096) || A != B) return false;
    }
    return true;
}
bool Editing(const TSharedRef<FJsonObject>& I)
{
    bool Pie = true, Sim = true;
    return GEditor && I->TryGetBoolField(TEXT("play_in_editor"), Pie) && I->TryGetBoolField(TEXT("simulating"), Sim) && !Pie && !Sim;
}
UObject* Loaded(const FString& Path)
{
    if (Path.Len() > 1024 || (!Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Engine/"))) || Path.Contains(TEXT(":")) || Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")) || !FPackageName::IsValidObjectPath(Path)) return nullptr;
    UObject* O = FindObject<UObject>(nullptr, *Path);
    const auto D = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(Path));
    return IsValid(O) && D.IsValid() && O->GetPathName() == Path && D.AssetClassPath == O->GetClass()->GetClassPathName() ? O : nullptr;
}
bool ApprovedMaterial(const FString& Path)
{
    bool Enabled = false; TArray<FString> Paths;
    GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableMaterialEdits"), Enabled, GGameIni);
    GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("EditableMaterials"), Paths, GGameIni);
    if (!Enabled || Paths.Num() > 64 || !Path.StartsWith(TEXT("/Game/"))) return false;
    TSet<FString> Seen;
    for (const FString& P : Paths)
    {
        if (!P.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidObjectPath(P) || P.Contains(TEXT(":")) || P.Contains(TEXT("..")) || P.Len() > 1024 || Seen.Contains(P)) return false;
        Seen.Add(P);
    }
    return Seen.Contains(Path);
}
bool ApprovedTexture(const FString& Path)
{
    TArray<FString> Paths; GConfig->GetArray(TEXT("JevEditor.Workflows"), TEXT("AllowedMaterialTextures"), Paths, GGameIni);
    if (Paths.Num() > 64) return false; TSet<FString> Seen;
    for (const FString& P : Paths)
    {
        if ((!P.StartsWith(TEXT("/Game/")) && !P.StartsWith(TEXT("/Engine/"))) || !FPackageName::IsValidObjectPath(P) || P.Contains(TEXT(":")) || P.Contains(TEXT("..")) || P.Len() > 1024 || Seen.Contains(P)) return false;
        Seen.Add(P);
    }
    return Seen.Contains(Path);
}
bool StaticSwitchEdits()
{
    bool Enabled = false; GConfig->GetBool(TEXT("JevEditor.Workflows"), TEXT("bEnableStaticSwitchEdits"), Enabled, GGameIni); return Enabled;
}
bool ParameterInfo(const TSharedPtr<FJsonObject>& Change, FMaterialParameterInfo& Info)
{
    FString Name, Association = TEXT("global"); double Index = -1;
    if (!Text(Change, TEXT("parameter"), Name, 128)) return false;
    if (Change->HasField(TEXT("association")) && !Text(Change, TEXT("association"), Association, 16)) return false;
    if (Change->HasField(TEXT("index")) && (!Number(Change, TEXT("index"), Index, -1, 63) || Index != FMath::TruncToDouble(Index))) return false;
    if (Association == TEXT("global") && Index == -1) Info = FMaterialParameterInfo(FName(*Name));
    else if ((Association == TEXT("layer") || Association == TEXT("blend")) && Index >= 0) Info = FMaterialParameterInfo(FName(*Name), Association == TEXT("layer") ? EMaterialParameterAssociation::LayerParameter : EMaterialParameterAssociation::BlendParameter, static_cast<int32>(Index));
    else return false;
    return true;
}
ULightComponent* NativeLight(UObject* Object)
{
    ALight* A = Cast<ALight>(Object);
    if (!IsValid(A) || !(A->GetClass() == APointLight::StaticClass() || A->GetClass() == ASpotLight::StaticClass() || A->GetClass() == ADirectionalLight::StaticClass() || A->GetClass() == ARectLight::StaticClass())) return nullptr;
    return A->GetLightComponent();
}
bool EditableLight(UObject* Object)
{
    auto* A = Cast<AActor>(Object);
    return NativeLight(Object) && GEditor && A->GetWorld() == GEditor->GetEditorWorldContext().World() && A->GetLevel() == A->GetWorld()->GetCurrentLevel() && !FLevelUtils::IsLevelLocked(A->GetLevel()) && !A->IsLockLocation() && !A->IsTemplate();
}
FLevelEditorViewportClient* View()
{
    return GCurrentLevelEditingViewportClient && GEditor && GEditor->GetLevelViewportClients().Contains(GCurrentLevelEditingViewportClient) && GCurrentLevelEditingViewportClient->GetWorld() == GEditor->GetEditorWorldContext().World() && GCurrentLevelEditingViewportClient->IsPerspective() && !GCurrentLevelEditingViewportClient->IsAnyActorLocked() ? GCurrentLevelEditingViewportClient : nullptr;
}
FString Json(const TSharedRef<FJsonObject>& R)
{
    FString S; FJsonSerializer::Serialize(R, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&S)); return S;
}
TSharedRef<FJsonObject> Material(UObject* Object)
{
    auto* M = Cast<UMaterialInstanceConstant>(Object);
    if (!M || M->GetClass() != UMaterialInstanceConstant::StaticClass()) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("Open an exact native MaterialInstanceConstant first."));
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), M->GetPathName()); R->SetStringField(TEXT("parent_path"), GetPathNameSafe(M->Parent));
    R->SetNumberField(TEXT("parent_instance_id"), M->Parent ? M->Parent->GetUniqueID() : 0);
    R->SetNumberField(TEXT("instance_id"), M->GetUniqueID()); R->SetBoolField(TEXT("package_dirty"), M->GetOutermost()->IsDirty());
    R->SetBoolField(TEXT("editing_approved"), ApprovedMaterial(M->GetPathName()));
    TArray<TSharedPtr<FJsonValue>> Rows;
    bool Truncated = false;
    for (int32 Kind = 0; Kind < 4; ++Kind)
    {
        TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids;
        if (Kind == 0) M->GetAllScalarParameterInfo(Infos, Ids); else if (Kind == 1) M->GetAllVectorParameterInfo(Infos, Ids); else if (Kind == 2) M->GetAllTextureParameterInfo(Infos, Ids); else M->GetAllStaticSwitchParameterInfo(Infos, Ids);
        for (const auto& Info : Infos)
        {
            if (Rows.Num() >= 128) { Truncated = true; break; }
            auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), Info.Name.ToString().Left(128));
            Row->SetNumberField(TEXT("association"), static_cast<int32>(Info.Association)); Row->SetNumberField(TEXT("index"), Info.Index);
            Row->SetStringField(TEXT("kind"), Kind == 0 ? TEXT("scalar") : Kind == 1 ? TEXT("vector") : Kind == 2 ? TEXT("texture") : TEXT("static_switch"));
            Row->SetStringField(TEXT("association_name"), Info.Association == EMaterialParameterAssociation::GlobalParameter ? TEXT("global") : Info.Association == EMaterialParameterAssociation::LayerParameter ? TEXT("layer") : TEXT("blend"));
            Row->SetBoolField(TEXT("editable"), Info.Name.ToString().Len() <= 128 && Info.Index >= -1 && Info.Index <= 63 && (Kind != 3 || StaticSwitchEdits()));
            if (Kind == 0) { float V = 0; Row->SetBoolField(TEXT("resolved"), M->GetScalarParameterValue(Info, V)); Row->SetNumberField(TEXT("value"), V); }
            else if (Kind == 1) { FLinearColor V; Row->SetBoolField(TEXT("resolved"), M->GetVectorParameterValue(Info, V)); Row->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(V.R), MakeShared<FJsonValueNumber>(V.G), MakeShared<FJsonValueNumber>(V.B), MakeShared<FJsonValueNumber>(V.A)}); }
            else if (Kind == 2) { UTexture* V = nullptr; Row->SetBoolField(TEXT("resolved"), M->GetTextureParameterValue(Info, V)); Row->SetStringField(TEXT("value"), GetPathNameSafe(V)); Row->SetNumberField(TEXT("texture_instance_id"), V ? V->GetUniqueID() : 0); }
            else { bool V = false; FGuid Guid; Row->SetBoolField(TEXT("resolved"), M->GetStaticSwitchParameterValue(Info, V, Guid)); Row->SetBoolField(TEXT("value"), V); Row->SetStringField(TEXT("expression_id"), Guid.ToString()); }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    R->SetArrayField(TEXT("parameters"), Rows); R->SetBoolField(TEXT("truncated"), Truncated);
    R->SetStringField(TEXT("scope"), TEXT("Effective exposed scalar/vector/texture/static-switch values including existing layer and blend parameters. Texture assignment requires exact loaded native Texture2D identity and project approval. Static switches may enqueue shader compilation. No layer stack restructuring, implicit loading or shader/visual acceptance."));
    return Success(R);
}
TSharedRef<FJsonObject> Light(UObject* Object)
{
    auto* L = NativeLight(Object);
    if (!L) return FJevEditorBridge::Error(TEXT("actor_unsupported"), TEXT("Select a native Point, Spot, Rect or Directional light."));
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("actor_path"), Object->GetPathName()); R->SetNumberField(TEXT("instance_id"), Object->GetUniqueID());
    R->SetNumberField(TEXT("intensity"), L->Intensity); const auto C = L->GetLightColor(); R->SetArrayField(TEXT("color_rgb"), Vector(FVector(C.R, C.G, C.B)));
    R->SetStringField(TEXT("component_class"), L->GetClass()->GetPathName()); R->SetNumberField(TEXT("mobility"), L->Mobility); R->SetBoolField(TEXT("cast_shadows"), L->CastShadows);
    if (const auto* Local = Cast<ULocalLightComponent>(L))
    {
        R->SetStringField(TEXT("intensity_units"), StaticEnum<ELightUnits>()->GetNameStringByValue(static_cast<int64>(Local->GetLightUnits())));
        R->SetNumberField(TEXT("attenuation_radius_cm"), Local->AttenuationRadius);
    }
    else R->SetStringField(TEXT("intensity_units"), TEXT("directional_illuminance"));
    R->SetStringField(TEXT("units"), TEXT("Native intensity units for this light and project; no conversion or exposure normalization."));
    return Success(R);
}
TSharedRef<FJsonObject> Camera()
{
    auto* V = View(); if (!V) return FJevEditorBridge::Error(TEXT("viewport_unavailable"), TEXT("An unlocked perspective level viewport is required."));
    auto R = MakeShared<FJsonObject>(); R->SetArrayField(TEXT("location"), Vector(V->GetViewLocation())); const auto Rot = V->GetViewRotation(); R->SetArrayField(TEXT("rotation"), Vector(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll)));
    R->SetNumberField(TEXT("fov_degrees"), V->ViewFOV); R->SetBoolField(TEXT("realtime"), V->IsRealtime());
    R->SetBoolField(TEXT("realtime_override"), V->IsRealtimeOverrideSet()); R->SetBoolField(TEXT("fixed_exposure"), V->ExposureSettings.bFixed); R->SetNumberField(TEXT("fixed_ev100"), V->ExposureSettings.FixedEV100); R->SetNumberField(TEXT("view_mode_index"), V->GetViewMode()); R->SetBoolField(TEXT("motion_blur"), V->EngineShowFlags.MotionBlur);
    R->SetStringField(TEXT("scope"), TEXT("Editor viewport pose, FOV, exposure, lit/unlit mode, realtime and motion blur settings. A realtime toggle is an editor preference, not a world-time lock. Temporal history, simulation, shader completion and time of day are not frozen. Capture separately after settling.")); return Success(R);
}
}

struct FJevWorkflowTools::FPlan
{
    FString Kind, TargetPath, Baseline;
    TWeakObjectPtr<UObject> Target;
    TWeakObjectPtr<UTexture2D> Texture;
    FString TexturePath;
    bool bTextureVirtual = false;
    TSharedPtr<FJsonObject> Change, Identity, Receipt;
    uint64 Epoch = 0; double CreatedAt = 0; bool Consumed = false;
    FLevelEditorViewportClient* Viewport = nullptr;
};

FJevWorkflowTools::FJevWorkflowTools(TFunction<double()> InClock)
    : Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
    ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject*) { ++ChangeEpoch; });
    PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject*, FPropertyChangedEvent&) { ++ChangeEpoch; });
}
FJevWorkflowTools::~FJevWorkflowTools()
{
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle); FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
}
bool FJevWorkflowTools::HandlesAction(const FString& A)
{
    return A == TEXT("workflow_inspect") || A == TEXT("workflow_preview") || A == TEXT("workflow_apply") || A == TEXT("workflow_receipt") || A == TEXT("performance_start") || A == TEXT("performance_job") || A == TEXT("performance_cancel");
}
TSharedRef<FJsonObject> FJevWorkflowTools::Inspect(const TSharedPtr<FJsonObject>& P, const TSharedRef<FJsonObject>& Identity)
{
    using namespace JevWorkflow;
    FString Kind, Path; if (!Text(P, TEXT("kind"), Kind, 32)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a known workflow kind."));
    TSharedPtr<FJsonObject> Response;
    if (Kind == TEXT("camera") && Only(P, {TEXT("kind")})) Response = Camera();
    else if ((Kind == TEXT("material") || Kind == TEXT("light")) && Only(P, {TEXT("kind"), TEXT("target_path")}) && Text(P, TEXT("target_path"), Path))
    {
        UObject* O = Kind == TEXT("material") ? Loaded(Path) : FindObject<UObject>(nullptr, *Path);
        if (Kind == TEXT("light") && (!Cast<AActor>(O) || Cast<AActor>(O)->GetWorld() != GEditor->GetEditorWorldContext().World())) return FJevEditorBridge::Error(TEXT("actor_not_found"), TEXT("Light is not in the connected editor world."));
        Response = Kind == TEXT("material") ? Material(O) : Light(O);
    }
    else if (Kind == TEXT("asset_diagnosis")) Response = AssetDiagnosis(P);
    else if (Kind == TEXT("rig")) Response = Rig(P);
    else if (Kind == TEXT("widgets")) Response = Widgets(P);
    else if (Kind == TEXT("surface")) Response = Surface(P);
    else if (Kind == TEXT("navigation")) Response = Navigation(P);
    else return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Unsupported workflow or fields."));
    const TSharedPtr<FJsonObject>* R = nullptr;
    if (Response->TryGetObjectField(TEXT("result"), R))
    {
        const auto IdentityFields = Base(Identity);
        for (const auto& Pair : IdentityFields->Values) (*R)->SetField(FString(*Pair.Key), Pair.Value);
        (*R)->SetStringField(TEXT("kind"), Kind);
    }
    return Response.ToSharedRef();
}
TSharedRef<FJsonObject> FJevWorkflowTools::Preview(const TSharedPtr<FJsonObject>& P, const TSharedRef<FJsonObject>& I)
{
    using namespace JevWorkflow;
    FString Project, Kind, Path; const TSharedPtr<FJsonObject>* State = nullptr; const TSharedPtr<FJsonObject>* Change = nullptr;
    if (!Only(P, {TEXT("expected_project"), TEXT("expected_state"), TEXT("change")}) || !Text(P, TEXT("expected_project"), Project, 4096) || !P->TryGetObjectField(TEXT("expected_state"), State) || !Only(*State, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) || !P->TryGetObjectField(TEXT("change"), Change) || !Text(*Change, TEXT("kind"), Kind, 32)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply exact project, inspected state and one typed change."));
    if (Project != I->GetStringField(TEXT("project_file"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Project identity differs."));
    if (!Same(*State, I)) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("Editor state changed."));
    if (!Editing(I)) return FJevEditorBridge::Error(TEXT("play_mode"), TEXT("Stop PIE/simulation before editing."));
    if (Plans.Num() >= 64) return FJevEditorBridge::Error(TEXT("too_many_plans"), TEXT("Receipt store is full; retention is 15 minutes."));
    auto Plan = MakeShared<FPlan>(); Plan->Kind = Kind;
    TSharedPtr<FJsonObject> Snapshot;
    if (Kind == TEXT("material_scalar") || Kind == TEXT("material_vector") || Kind == TEXT("material_texture") || Kind == TEXT("material_static_switch"))
    {
        FMaterialParameterInfo Parameter;
        if (!Only(*Change, {TEXT("kind"), TEXT("target_path"), TEXT("parameter"), TEXT("value"), TEXT("association"), TEXT("index")}) || !Text(*Change, TEXT("target_path"), Path) || !ParameterInfo(*Change, Parameter)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a parameter name and valid global/layer/blend association and index."));
        if (!ApprovedMaterial(Path)) return FJevEditorBridge::Error(TEXT("target_not_allowed"), TEXT("Approve this exact material in JevEditor.Workflows first."));
        UObject* O = Loaded(Path); Snapshot = Material(O); Plan->Target = O;
        const auto* M = Cast<UMaterialInstanceConstant>(O); if (!M) return Snapshot.ToSharedRef();
        const TSharedPtr<FJsonObject>* MaterialResult = nullptr;
        if (!Snapshot->TryGetObjectField(TEXT("result"), MaterialResult)) return Snapshot.ToSharedRef();
        if ((*MaterialResult)->GetBoolField(TEXT("truncated"))) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("The material exceeds the 128-parameter review limit."));
        TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids;
        if (Kind == TEXT("material_scalar")) M->GetAllScalarParameterInfo(Infos, Ids); else if (Kind == TEXT("material_vector")) M->GetAllVectorParameterInfo(Infos, Ids); else if (Kind == TEXT("material_texture")) M->GetAllTextureParameterInfo(Infos, Ids); else M->GetAllStaticSwitchParameterInfo(Infos, Ids);
        if (!Infos.Contains(Parameter)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("The exact exposed parameter name, association and index do not exist."));
        double Value = 0;
        if (Kind == TEXT("material_scalar")) { if (!Number(*Change, TEXT("value"), Value, -1000000, 1000000)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Scalar must be finite and bounded.")); }
        else if (Kind == TEXT("material_vector"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!(*Change)->TryGetArrayField(TEXT("value"), Values) || Values->Num() != 4) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Vector requires four channels."));
            for (const auto& V : *Values) if (!V || V->Type != EJson::Number || !V->TryGetNumber(Value) || !FMath::IsFinite(Value) || Value < 0 || Value > 16) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("RGBA channels must be finite in 0..16."));
        }
        else if (Kind == TEXT("material_texture"))
        {
            FString TexturePath; if (!Text(*Change, TEXT("value"), TexturePath) || !ApprovedTexture(TexturePath)) return FJevEditorBridge::Error(TEXT("target_not_allowed"), TEXT("Approve the exact texture in AllowedMaterialTextures."));
            auto* Texture = Cast<UTexture2D>(Loaded(TexturePath)); UTexture* Previous = nullptr;
            if (!Texture || Texture->GetClass() != UTexture2D::StaticClass() || !M->GetTextureParameterValue(Parameter, Previous) || !Previous || Previous->GetClass() != UTexture2D::StaticClass() || Previous->VirtualTextureStreaming != Texture->VirtualTextureStreaming) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("Texture assignment requires loaded native Texture2D values with matching virtual-texture sampling mode."));
            Plan->Texture = Texture; Plan->TexturePath = TexturePath; Plan->bTextureVirtual = Texture->VirtualTextureStreaming;
        }
        else
        {
            if (!StaticSwitchEdits()) return FJevEditorBridge::Error(TEXT("target_not_allowed"), TEXT("Enable bEnableStaticSwitchEdits explicitly; static permutations may enqueue shader compilation."));
            if (!(*Change)->HasTypedField<EJson::Boolean>(TEXT("value"))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Static switch requires a boolean."));
        }
    }
    else if (Kind == TEXT("light"))
    {
        double Intensity = 0; FVector Color;
        if (!Only(*Change, {TEXT("kind"), TEXT("target_path"), TEXT("intensity"), TEXT("color_rgb")}) || !Text(*Change, TEXT("target_path"), Path) || !Number(*Change, TEXT("intensity"), Intensity, 0, 1000000) || !Vector(*Change, TEXT("color_rgb"), Color, 1) || Color.GetMin() < 0) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Light requires intensity 0..1e6 and linear RGB 0..1."));
        UObject* O = FindObject<UObject>(nullptr, *Path); if (!EditableLight(O)) return FJevEditorBridge::Error(TEXT("actor_locked"), TEXT("Light must be an editable native actor in the current level."));
        Plan->Target = O; Snapshot = Light(O);
    }
    else if (Kind == TEXT("camera"))
    {
        FVector Location, Rotation; double Fov = 0;
        if (!Only(*Change, {TEXT("kind"), TEXT("location"), TEXT("rotation"), TEXT("fov_degrees")}) || !Vector(*Change, TEXT("location"), Location) || !Vector(*Change, TEXT("rotation"), Rotation, 360) || !Number(*Change, TEXT("fov_degrees"), Fov, 5, 170)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Camera requires bounded pose and FOV 5..170."));
        Plan->Viewport = View(); Snapshot = Camera();
    }
    else if (Kind == TEXT("camera_render"))
    {
        FString Exposure, Mode; double EV = 0;
        if (!Only(*Change, {TEXT("kind"), TEXT("exposure_mode"), TEXT("fixed_ev100"), TEXT("view_mode"), TEXT("realtime"), TEXT("motion_blur")}) || !Text(*Change, TEXT("exposure_mode"), Exposure, 16) || (Exposure != TEXT("fixed") && Exposure != TEXT("automatic")) || !Number(*Change, TEXT("fixed_ev100"), EV, -16, 32) || !Text(*Change, TEXT("view_mode"), Mode, 16) || (Mode != TEXT("lit") && Mode != TEXT("unlit")) || !(*Change)->HasTypedField<EJson::Boolean>(TEXT("realtime")) || !(*Change)->HasTypedField<EJson::Boolean>(TEXT("motion_blur"))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply fixed/automatic exposure, EV100 -16..32, lit/unlit view mode, realtime and motion_blur booleans."));
        Plan->Viewport = View(); Snapshot = Camera();
        if (Plan->Viewport && (Plan->Viewport->IsRealtimeOverrideSet() || Plan->Viewport->IsEngineShowFlagsOverrideEnabled())) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("Another subsystem owns a realtime or show-flags override."));
        if (Plan->Viewport && ((Plan->Viewport->GetViewMode() != VMI_Lit && Plan->Viewport->GetViewMode() != VMI_Unlit) || !FMath::IsFinite(Plan->Viewport->ExposureSettings.FixedEV100) || Plan->Viewport->ExposureSettings.FixedEV100 < -16 || Plan->Viewport->ExposureSettings.FixedEV100 > 32)) return FJevEditorBridge::Error(TEXT("viewport_unavailable"), TEXT("Start in lit/unlit mode with a bounded EV100 so the captured settings can be restored through this workflow."));
    }
    else return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Unsupported edit kind."));
    const TSharedPtr<FJsonObject>* Before = nullptr;
    if (!Snapshot->TryGetObjectField(TEXT("result"), Before)) return Snapshot.ToSharedRef();
    Plan->TargetPath = Path; Plan->Baseline = Json(Snapshot.ToSharedRef()); Plan->Change = *Change; Plan->Identity = Base(I); Plan->Epoch = ChangeEpoch; Plan->CreatedAt = Clock();
    auto R = Base(I); const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    R->SetStringField(TEXT("plan_id"), Id); R->SetStringField(TEXT("status"), TEXT("pending")); R->SetObjectField(TEXT("before"), *Before); R->SetObjectField(TEXT("requested"), *Change); R->SetNumberField(TEXT("expires_in_seconds"), 120);
    R->SetBoolField(TEXT("save_requested"), false); R->SetStringField(TEXT("scope"), TEXT("One native edit; readback is recorded. Asset/light edits use Undo. Viewport changes are not Undo transactions. No render, gameplay or compiler side-effect guarantee. Receipts survive MCP reconnect only."));
    Plan->Receipt = R; Plans.Add(Id, Plan); return Success(R);
}
TSharedRef<FJsonObject> FJevWorkflowTools::Apply(const TSharedPtr<FJsonObject>& P, const TSharedRef<FJsonObject>& I)
{
    using namespace JevWorkflow;
    FString Id, Project;
    if (!Only(P, {TEXT("plan_id"), TEXT("expected_project")}) || !Text(P, TEXT("plan_id"), Id, 64) || !Text(P, TEXT("expected_project"), Project, 4096)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply reviewed plan_id and exact project."));
    if (Project != I->GetStringField(TEXT("project_file"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Project differs."));
    auto* Found = Plans.Find(Id); if (!Found) return FJevEditorBridge::Error(TEXT("unknown_plan"), TEXT("Unknown workflow plan."));
    const auto Plan = *Found;
    if (Plan->Consumed) return FJevEditorBridge::Error(TEXT("plan_consumed"), TEXT("Read the receipt; the plan was already attempted."));
    Plan->Consumed = true;
    auto Reject = [&Plan](const TCHAR* Code) { Plan->Receipt->SetStringField(TEXT("status"), Plan->Receipt->GetStringField(TEXT("status")) == TEXT("applying") ? TEXT("failed_after_attempt") : TEXT("rejected")); Plan->Receipt->SetStringField(TEXT("outcome_code"), Code); return FJevEditorBridge::Error(Code, TEXT("Inspect again before a new preview.")); };
    if (Clock() - Plan->CreatedAt >= 120) return Reject(TEXT("expired_plan"));
    if (Plan->Identity->GetStringField(TEXT("project_file")) != Project || !Same(Plan->Identity, I) || Plan->Epoch != ChangeEpoch) return Reject(TEXT("stale_plan"));
    if (!Editing(I)) return Reject(TEXT("play_mode"));
    TSharedPtr<FJsonObject> Current;
    UObject* O = Plan->Target.Get();
    if (Plan->Kind.StartsWith(TEXT("material_")))
    {
        if (!ApprovedMaterial(Plan->TargetPath)) return Reject(TEXT("policy_invalid"));
        if (!O || Loaded(Plan->TargetPath) != O) return Reject(TEXT("stale_plan"));
        if (Plan->Kind == TEXT("material_static_switch") && !StaticSwitchEdits()) return Reject(TEXT("policy_invalid"));
        if (Plan->Kind == TEXT("material_texture"))
        {
            if (!ApprovedTexture(Plan->TexturePath)) return Reject(TEXT("policy_invalid"));
            if (!Plan->Texture.IsValid() || Loaded(Plan->TexturePath) != Plan->Texture.Get() || Plan->Texture->VirtualTextureStreaming != Plan->bTextureVirtual) return Reject(TEXT("stale_plan"));
        }
        Current = Material(O);
    }
    else if (Plan->Kind == TEXT("light"))
    {
        if (!EditableLight(O) || O->GetPathName() != Plan->TargetPath) return Reject(TEXT("stale_plan"));
        Current = Light(O);
    }
    else { if (View() != Plan->Viewport) return Reject(TEXT("stale_plan")); if (Plan->Kind == TEXT("camera_render") && (Plan->Viewport->IsRealtimeOverrideSet() || Plan->Viewport->IsEngineShowFlagsOverrideEnabled())) return Reject(TEXT("editor_busy")); Current = Camera(); }
    if (Json(Current.ToSharedRef()) != Plan->Baseline) return Reject(TEXT("stale_plan"));
    if (!GEditor->CanTransact() || GEditor->IsTransactionActive() || GIsTransacting) return Reject(TEXT("editor_busy"));
    TGuardValue<bool> Guard(bApplying, true); Plan->Receipt->SetStringField(TEXT("status"), TEXT("applying"));
    TStrongObjectPtr<UObject> KeepAlive(O);
    bool Verified = false;
    if (Plan->Kind.StartsWith(TEXT("material_")))
    {
        FScopedTransaction Transaction(NSLOCTEXT("JevEditor", "MaterialParameter", "Edit Jev material parameter"));
        auto* M = CastChecked<UMaterialInstanceConstant>(O); FMaterialParameterInfo Info;
        if (!ParameterInfo(Plan->Change, Info)) return Reject(TEXT("apply_failed"));
        M->Modify();
        if (Plan->Kind == TEXT("material_scalar")) M->SetScalarParameterValueEditorOnly(Info, static_cast<float>(Plan->Change->GetNumberField(TEXT("value"))));
        else if (Plan->Kind == TEXT("material_vector")) { const auto& V = Plan->Change->GetArrayField(TEXT("value")); M->SetVectorParameterValueEditorOnly(Info, FLinearColor(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber(), V[3]->AsNumber())); }
        else if (Plan->Kind == TEXT("material_texture")) M->SetTextureParameterValueEditorOnly(Info, Plan->Texture.Get());
        else { M->SetStaticSwitchParameterValueEditorOnly(Info, Plan->Change->GetBoolField(TEXT("value"))); M->UpdateStaticPermutation(); }
        M->PostEditChange();
        if (!IsValid(M) || Loaded(Plan->TargetPath) != M) return Reject(TEXT("apply_failed"));
        if (Plan->Kind == TEXT("material_scalar")) { float Value = 0; Verified = M->GetScalarParameterValue(Info, Value) && FMath::IsNearlyEqual(Value, static_cast<float>(Plan->Change->GetNumberField(TEXT("value"))), 0.0001f); }
        else if (Plan->Kind == TEXT("material_vector")) { FLinearColor Value; const auto& V = Plan->Change->GetArrayField(TEXT("value")); Verified = M->GetVectorParameterValue(Info, Value) && Value.Equals(FLinearColor(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber(), V[3]->AsNumber()), 0.0001f); }
        else if (Plan->Kind == TEXT("material_texture")) { UTexture* Value = nullptr; Verified = M->GetTextureParameterValue(Info, Value) && Value == Plan->Texture.Get() && Loaded(Plan->TexturePath) == Value; }
        else { bool Value = false; FGuid Guid; Verified = M->GetStaticSwitchParameterValue(Info, Value, Guid) && Value == Plan->Change->GetBoolField(TEXT("value")); }
        Current = Material(M);
    }
    else if (Plan->Kind == TEXT("light"))
    {
        FScopedTransaction Transaction(NSLOCTEXT("JevEditor", "LightEdit", "Edit Jev light"));
        auto* L = NativeLight(O); O->Modify(); L->Modify(); FVector C; Vector(Plan->Change, TEXT("color_rgb"), C, 1);
        L->SetIntensity(Plan->Change->GetNumberField(TEXT("intensity"))); L->SetLightColor(FLinearColor(C.X, C.Y, C.Z), true); L->PostEditChange();
        if (!IsValid(O) || NativeLight(O) != L) return Reject(TEXT("apply_failed"));
        Verified = FMath::IsNearlyEqual(L->Intensity, static_cast<float>(Plan->Change->GetNumberField(TEXT("intensity"))), 0.0001f) && L->GetLightColor().Equals(FLinearColor(C.X, C.Y, C.Z), 0.01f);
        Current = Light(O);
    }
    else if (Plan->Kind == TEXT("camera_render"))
    {
        auto* V = View(); V->ExposureSettings.bFixed = Plan->Change->GetStringField(TEXT("exposure_mode")) == TEXT("fixed"); V->ExposureSettings.FixedEV100 = Plan->Change->GetNumberField(TEXT("fixed_ev100"));
        const EViewModeIndex Mode = Plan->Change->GetStringField(TEXT("view_mode")) == TEXT("lit") ? VMI_Lit : VMI_Unlit;
        V->SetViewMode(Mode); V->SetRealtime(Plan->Change->GetBoolField(TEXT("realtime"))); V->EngineShowFlags.SetMotionBlur(Plan->Change->GetBoolField(TEXT("motion_blur"))); V->Invalidate();
        Verified = V->ExposureSettings.bFixed == (Plan->Change->GetStringField(TEXT("exposure_mode")) == TEXT("fixed")) && FMath::IsNearlyEqual(V->ExposureSettings.FixedEV100, static_cast<float>(Plan->Change->GetNumberField(TEXT("fixed_ev100")))) && V->GetViewMode() == Mode && V->DoRealtimeAndOverridesMatch(Plan->Change->GetBoolField(TEXT("realtime"))) && V->EngineShowFlags.MotionBlur == Plan->Change->GetBoolField(TEXT("motion_blur"));
        Current = Camera();
    }
    else
    {
        auto* V = View(); FVector Location, Rotation; Vector(Plan->Change, TEXT("location"), Location); Vector(Plan->Change, TEXT("rotation"), Rotation, 360);
        V->SetViewLocation(Location); V->SetViewRotation(FRotator(Rotation.X, Rotation.Y, Rotation.Z)); V->ViewFOV = Plan->Change->GetNumberField(TEXT("fov_degrees")); V->Invalidate(); Current = Camera();
        Verified = V->GetViewLocation().Equals(Location, 0.001) && V->GetViewRotation().Equals(FRotator(Rotation.X, Rotation.Y, Rotation.Z), 0.001) && FMath::IsNearlyEqual(V->ViewFOV, static_cast<float>(Plan->Change->GetNumberField(TEXT("fov_degrees"))), 0.001f);
    }
    const TSharedPtr<FJsonObject>* After = nullptr;
    if (!Current->TryGetObjectField(TEXT("result"), After)) return Reject(TEXT("apply_failed"));
    Plan->Receipt->SetObjectField(TEXT("after"), *After); Plan->Receipt->SetBoolField(TEXT("readback_verified"), Verified); Plan->Receipt->SetStringField(TEXT("status"), Verified ? TEXT("applied") : TEXT("readback_failed"));
    Plan->Receipt->SetBoolField(TEXT("visual_acceptance"), false); return Success(Plan->Receipt.ToSharedRef());
}
TSharedRef<FJsonObject> FJevWorkflowTools::Execute(const FString& A, const TSharedPtr<FJsonObject>& P, const TSharedRef<FJsonObject>& I)
{
    using namespace JevWorkflow;
    for (auto It = Plans.CreateIterator(); It; ++It) if (Clock() - It->Value->CreatedAt > 900) It.RemoveCurrent();
    if (A == TEXT("workflow_receipt"))
    {
        FString Id; if (!Only(P, {TEXT("plan_id")}) || !Text(P, TEXT("plan_id"), Id, 64)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply one plan id."));
        const auto* Found = Plans.Find(Id); if (!Found) return FJevEditorBridge::Error(TEXT("unknown_plan"), TEXT("Unknown receipt."));
        const auto Plan = *Found;
        if (Plan->Identity->GetStringField(TEXT("project_file")) != I->GetStringField(TEXT("project_file")) || Plan->Identity->GetStringField(TEXT("session_id")) != I->GetStringField(TEXT("session_id"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Receipt belongs to a different project/session."));
        Plan->Receipt->SetNumberField(TEXT("expires_in_seconds"), Plan->Consumed ? 0 : FMath::Max(0.0, 120 - (Clock() - Plan->CreatedAt)));
        if (!Plan->Consumed && Clock() - Plan->CreatedAt >= 120) Plan->Receipt->SetStringField(TEXT("status"), TEXT("expired"));
        return Success(Plan->Receipt.ToSharedRef());
    }
    if (bApplying) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("A workflow edit is applying."));
    if (A == TEXT("workflow_inspect")) return Inspect(P, I);
    if (A == TEXT("workflow_preview")) return Preview(P, I);
    if (A == TEXT("workflow_apply")) return Apply(P, I);
    if (A.StartsWith(TEXT("performance_"))) return Performance(A, P, I);
    return FJevEditorBridge::Error(TEXT("unknown_action"), TEXT("Unknown workflow."));
}
