#include "JevEditorBlueprintTools.h"
#include "JevEditorBridge.h"

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace JevBlueprint
{
const TCHAR* Section = TEXT("JevEditor.BlueprintCompilation");
constexpr int32 MaximumPlans = 64;
constexpr double PreviewSeconds = 120;
constexpr double RetentionSeconds = 900;

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), true);
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

TSharedRef<FJsonObject> Base(const TSharedRef<FJsonObject>& Identity)
{
    auto Result = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        FString Value;
        if (Identity->TryGetStringField(Key, Value)) Result->SetStringField(Key, Value);
    }
    Result->SetBoolField(TEXT("cloud_used"), false);
    return Result;
}

bool Only(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& Keys)
{
    if (!Params) return false;
    for (const auto& Pair : Params->Values) if (!Keys.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool Text(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FString& Value, int32 Maximum)
{
    return Params && Params->HasTypedField<EJson::String>(Key) && Params->TryGetStringField(Key, Value) && !Value.IsEmpty() && Value.Len() <= Maximum;
}

bool Alias(const FString& Value)
{
    if (Value.IsEmpty() || Value.Len() > 64) return false;
    for (TCHAR C : Value) if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.')) return false;
    return true;
}

bool Path(const FString& Value)
{
    if (Value.Len() > 1024 || !Value.StartsWith(TEXT("/Game/")) || Value.Contains(TEXT(":")) || Value.Contains(TEXT("..")) || Value.Contains(TEXT("\\"))) return false;
    for (TCHAR C : Value) if (FChar::IsWhitespace(C) || FChar::IsControl(C)) return false;
    return FPackageName::IsValidObjectPath(Value) && !FPackageName::ObjectPathToObjectName(Value).IsEmpty();
}

struct FPolicy
{
    bool bEnabled = false;
    bool bValid = true;
    TMap<FString, FString> Targets;
};

FPolicy Policy()
{
    FPolicy Result;
    GConfig->GetBool(Section, TEXT("bEnabled"), Result.bEnabled, GGameIni);
    TArray<FString> Entries;
    GConfig->GetArray(Section, TEXT("Targets"), Entries, GGameIni);
    if (Entries.Num() > 64) { Result.bValid = false; return Result; }
    TSet<FString> Paths;
    for (const FString& Entry : Entries)
    {
        FString Id, Asset;
        if (!Entry.Split(TEXT("|"), &Id, &Asset) || !Alias(Id) || !Path(Asset) || Result.Targets.Contains(Id) || Paths.Contains(Asset))
        {
            Result.bValid = false; Result.Targets.Reset(); return Result;
        }
        Result.Targets.Add(Id, Asset); Paths.Add(Asset);
    }
    return Result;
}

UBlueprint* Loaded(const FString& Asset)
{
    const FAssetData Data = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(Asset));
    UBlueprint* BP = FindObject<UBlueprint>(nullptr, *Asset);
    return Data.IsValid() && BP && FJevBlueprintTools::IsSupportedClass(BP->GetClass()) && Data.AssetClassPath == BP->GetClass()->GetClassPathName() && BP->GetPathName() == Asset ? BP : nullptr;
}

bool SameState(const TSharedPtr<FJsonObject>& State, const TSharedRef<FJsonObject>& Identity)
{
    if (!Only(State, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})) return false;
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        FString Expected, Current;
        if (!Text(State, Key, Expected, 4096) || !Identity->TryGetStringField(Key, Current) || Expected != Current) return false;
    }
    return true;
}

bool Editing(const TSharedRef<FJsonObject>& Identity)
{
    bool bPie = true, bSimulating = true;
    return Identity->TryGetBoolField(TEXT("play_in_editor"), bPie) && Identity->TryGetBoolField(TEXT("simulating"), bSimulating) && !bPie && !bSimulating;
}

void SideEffects(const TSharedRef<FJsonObject>& Result)
{
    Result->SetBoolField(TEXT("save_requested"), false);
    Result->SetField(TEXT("saved"), MakeShared<FJsonValueNull>());
    Result->SetBoolField(TEXT("callback_side_effects_tracked"), false);
    Result->SetBoolField(TEXT("rollback_available"), false);
    Result->SetBoolField(TEXT("editor_state_after_known"), false);
    Result->SetStringField(TEXT("identity_scope"), TEXT("Project, session, world and revision describe the reviewed state before compilation; callbacks may change editor state."));
    Result->SetStringField(TEXT("scope"), TEXT("Compile one project-approved loaded Blueprint using SkipSave. Trusted compiler extensions, default-object validation and reinstancing may run project code, affect dependent assets or instances, or save through their own callbacks. No hard timeout, cancellation, rollback or complete side-effect tracking."));
}
}

struct FJevBlueprintTools::FPlan
{
    FString TargetId;
    FString AssetPath;
    TWeakObjectPtr<UBlueprint> Target;
    TSharedPtr<FJsonObject> Identity;
    TSharedPtr<FJsonObject> Receipt;
    uint64 Epoch = 0;
    double CreatedAt = 0;
    bool bDirty = false;
    EBlueprintStatus Status = BS_Unknown;
    bool bConsumed = false;
};

FJevBlueprintTools::FJevBlueprintTools(TFunction<double()> InClock)
    : Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
    ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject*) { ++ChangeEpoch; });
    PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject*, FPropertyChangedEvent&) { ++ChangeEpoch; });
}

FJevBlueprintTools::~FJevBlueprintTools()
{
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
    Shutdown();
}

bool FJevBlueprintTools::IsSupportedClass(const UClass* Class)
{
    return Class == UBlueprint::StaticClass() || Class == UWidgetBlueprint::StaticClass() || Class == UAnimBlueprint::StaticClass();
}

bool FJevBlueprintTools::HandlesAction(const FString& Action)
{
    return Action == TEXT("blueprint_compile_targets") || Action == TEXT("blueprint_compile_preview") || Action == TEXT("blueprint_compile") || Action == TEXT("blueprint_compile_receipt");
}

void FJevBlueprintTools::Prune()
{
    const double Now = Clock();
    for (auto It = Plans.CreateIterator(); It; ++It) if (Now - It->Value->CreatedAt > JevBlueprint::RetentionSeconds) It.RemoveCurrent();
}

void FJevBlueprintTools::Shutdown() { Plans.Reset(); }

TSharedRef<FJsonObject> FJevBlueprintTools::Targets(const TSharedRef<FJsonObject>& Identity) const
{
    using namespace JevBlueprint;
    const FPolicy Config = Policy();
    auto Result = Base(Identity);
    Result->SetBoolField(TEXT("enabled"), Config.bEnabled && Config.bValid);
    Result->SetBoolField(TEXT("policy_valid"), Config.bValid);
    TArray<FString> Ids; Config.Targets.GetKeys(Ids); Ids.Sort();
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FString& Id : Ids)
    {
        const FString& Asset = Config.Targets[Id];
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("target_id"), Id);
        Row->SetStringField(TEXT("asset_path"), Asset);
        Row->SetBoolField(TEXT("loaded_supported_asset"), Loaded(Asset) != nullptr);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("targets"), Rows);
    Result->SetBoolField(TEXT("compiled"), false);
    SideEffects(Result);
    return Success(Result);
}

TSharedRef<FJsonObject> FJevBlueprintTools::Preview(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    using namespace JevBlueprint;
    FString TargetId, Project;
    const TSharedPtr<FJsonObject>* State = nullptr;
    if (!Only(Params, {TEXT("target_id"), TEXT("expected_project"), TEXT("expected_state")}) || !Text(Params, TEXT("target_id"), TargetId, 64) || !Alias(TargetId) || !Text(Params, TEXT("expected_project"), Project, 4096) || !Params->TryGetObjectField(TEXT("expected_state"), State))
        return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply one target_id, exact expected_project and expected_state."));
    if (Project != Identity->GetStringField(TEXT("project_file"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Connected project differs from the requested project."));
    if (!SameState(*State, Identity)) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("Inspect the current editor identity before creating a compile preview."));
    if (!Editing(Identity)) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("Blueprint compilation is unavailable during PIE or simulation."));
    const FPolicy Config = Policy();
    if (!Config.bValid || !Config.bEnabled) return FJevEditorBridge::Error(TEXT("blueprint_compile_disabled"), TEXT("Project Blueprint compilation policy is disabled or invalid."));
    const FString* Asset = Config.Targets.Find(TargetId);
    if (!Asset) return FJevEditorBridge::Error(TEXT("target_not_allowed"), TEXT("The target alias is not approved by this project."));
    UBlueprint* BP = Loaded(*Asset);
    if (!BP) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open the exact approved native Blueprint, Widget Blueprint or Animation Blueprint in Unreal first."));
    if (GCompilingBlueprint || BP->bBeingCompiled || BP->bQueuedForCompilation) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("Unreal is compiling a Blueprint, or this target is already compiling or queued."));
    if (Plans.Num() >= MaximumPlans) return FJevEditorBridge::Error(TEXT("too_many_plans"), TEXT("The bounded compile receipt store is full; wait for older receipts to expire."));
    const FString PlanId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    auto Plan = MakeShared<FPlan>();
    Plan->TargetId = TargetId; Plan->AssetPath = *Asset; Plan->Target = BP;
    Plan->Identity = Base(Identity); Plan->Epoch = ChangeEpoch; Plan->CreatedAt = Clock();
    Plan->bDirty = BP->GetOutermost()->IsDirty(); Plan->Status = BP->Status;
    auto Result = Base(Identity);
    Result->SetStringField(TEXT("plan_id"), PlanId);
    Result->SetStringField(TEXT("target_id"), TargetId);
    Result->SetStringField(TEXT("asset_path"), *Asset);
    Result->SetStringField(TEXT("asset_class"), BP->GetClass()->GetPathName());
    Result->SetStringField(TEXT("status"), TEXT("pending"));
    Result->SetNumberField(TEXT("asset_instance_id_before"), BP->GetUniqueID());
    Result->SetStringField(TEXT("generated_class_before"), GetPathNameSafe(BP->GeneratedClass));
    Result->SetNumberField(TEXT("blueprint_status_before"), static_cast<int32>(BP->Status));
    Result->SetBoolField(TEXT("compiled"), false);
    Result->SetBoolField(TEXT("package_dirty_before"), Plan->bDirty);
    Result->SetNumberField(TEXT("expires_in_seconds"), PreviewSeconds);
    Result->SetStringField(TEXT("review"), TEXT("Compile this exact loaded asset once. Review the asset and trusted project compiler code before committing. A stale or failed attempt consumes the plan; never retry a timed-out commit without reading its receipt."));
    SideEffects(Result);
    Plan->Receipt = Result; Plans.Add(PlanId, Plan);
    return Success(Result);
}

TSharedRef<FJsonObject> FJevBlueprintTools::Compile(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    using namespace JevBlueprint;
    FString PlanId, Project;
    if (!Only(Params, {TEXT("plan_id"), TEXT("expected_project")}) || !Text(Params, TEXT("plan_id"), PlanId, 64) || !Text(Params, TEXT("expected_project"), Project, 4096))
        return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply one reviewed plan_id and exact expected_project."));
    if (Project != Identity->GetStringField(TEXT("project_file"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Connected project differs from the requested project."));
    const auto* Found = Plans.Find(PlanId);
    if (!Found) return FJevEditorBridge::Error(TEXT("unknown_plan"), TEXT("Compile preview is unknown or no longer retained."));
    const auto Plan = *Found;
    if (Plan->bConsumed) return FJevEditorBridge::Error(TEXT("plan_consumed"), TEXT("This compile plan was already attempted; inspect its receipt."));
    Plan->bConsumed = true;
    Plan->Receipt->SetNumberField(TEXT("expires_in_seconds"), 0);
    auto Reject = [&Plan](const TCHAR* Code, const TCHAR* Message)
    {
        Plan->Receipt->SetStringField(TEXT("status"), TEXT("rejected"));
        Plan->Receipt->SetStringField(TEXT("outcome_code"), Code);
        return FJevEditorBridge::Error(Code, Message);
    };
    if (Clock() - Plan->CreatedAt >= PreviewSeconds) return Reject(TEXT("expired_plan"), TEXT("The compile preview expired. Inspect and preview again."));
    for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
        if (Plan->Identity->GetStringField(Key) != Identity->GetStringField(Key)) return Reject(TEXT("stale_plan"), TEXT("The editor identity or scene changed after preview."));
    if (!Editing(Identity)) return Reject(TEXT("editor_busy"), TEXT("Compilation is unavailable during PIE or simulation."));
    const FPolicy Config = Policy();
    const FString* CurrentAsset = Config.Targets.Find(Plan->TargetId);
    if (!Config.bValid || !Config.bEnabled || !CurrentAsset || *CurrentAsset != Plan->AssetPath) return Reject(TEXT("policy_invalid"), TEXT("Project compilation approval changed after preview."));
    UBlueprint* BP = Plan->Target.Get();
    if (!BP || Loaded(Plan->AssetPath) != BP || Plan->Epoch != ChangeEpoch || BP->GetOutermost()->IsDirty() != Plan->bDirty || BP->Status != Plan->Status)
        return Reject(TEXT("stale_plan"), TEXT("An editor object changed after preview, or the loaded Blueprint identity changed. Inspect and preview again."));
    if (GCompilingBlueprint || BP->bBeingCompiled || BP->bQueuedForCompilation) return Reject(TEXT("editor_busy"), TEXT("Unreal is compiling a Blueprint, or this target is already compiling or queued."));
    TGuardValue<bool> Guard(bCompiling, true);
    TStrongObjectPtr<UBlueprint> KeepAlive(BP);
    Plan->Receipt->SetStringField(TEXT("status"), TEXT("running"));
    Plan->Receipt->SetBoolField(TEXT("compiled"), true);
    FCompilerResultsLog Log;
    Log.bSilentMode = true;
    const double Started = Clock();
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Log);
    const bool bPassed = Log.NumErrors == 0 && (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
    Plan->Receipt->SetStringField(TEXT("status"), bPassed ? TEXT("passed") : TEXT("failed"));
    Plan->Receipt->SetNumberField(TEXT("error_count"), Log.NumErrors);
    Plan->Receipt->SetNumberField(TEXT("warning_count"), Log.NumWarnings);
    Plan->Receipt->SetNumberField(TEXT("elapsed_seconds"), FMath::Max(0.0, Clock() - Started));
    Plan->Receipt->SetBoolField(TEXT("package_dirty_after"), BP->GetOutermost()->IsDirty());
    Plan->Receipt->SetNumberField(TEXT("asset_instance_id_after"), BP->GetUniqueID());
    Plan->Receipt->SetStringField(TEXT("asset_path_after"), BP->GetPathName());
    Plan->Receipt->SetStringField(TEXT("asset_class_after"), BP->GetClass()->GetPathName());
    Plan->Receipt->SetStringField(TEXT("generated_class_after"), GetPathNameSafe(BP->GeneratedClass));
    Plan->Receipt->SetNumberField(TEXT("blueprint_status_after"), static_cast<int32>(BP->Status));
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    bool bTruncated = Log.Messages.Num() > 128;
    for (const auto& Message : Log.Messages)
    {
        if (Diagnostics.Num() == 128) break;
        auto Row = MakeShared<FJsonObject>();
        const FString TextValue = Message->ToText().ToString();
        bTruncated |= TextValue.Len() > 1024;
        Row->SetStringField(TEXT("message"), TextValue.Left(1024));
        const auto Severity = Message->GetSeverity();
        Row->SetStringField(TEXT("severity"), Severity <= EMessageSeverity::Error ? TEXT("error") : Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning ? TEXT("warning") : TEXT("info"));
        Diagnostics.Add(MakeShared<FJsonValueObject>(Row));
    }
    Plan->Receipt->SetArrayField(TEXT("diagnostics"), Diagnostics);
    Plan->Receipt->SetBoolField(TEXT("diagnostics_truncated"), bTruncated);
    Plan->Receipt->SetStringField(TEXT("diagnostic_scope"), TEXT("Fresh compiler results for this explicit compile. Diagnostics may include private project text; external callbacks and unrelated logs are not comprehensively captured."));
    return Success(Plan->Receipt.ToSharedRef());
}

TSharedRef<FJsonObject> FJevBlueprintTools::Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    using namespace JevBlueprint;
    Prune();
    if (Action == TEXT("blueprint_compile_targets"))
        return Only(Params, {}) ? Targets(Identity) : FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Compile target discovery takes no arguments."));
    if (Action == TEXT("blueprint_compile_receipt"))
    {
        FString Id;
        if (!Only(Params, {TEXT("plan_id")}) || !Text(Params, TEXT("plan_id"), Id, 64)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply one compile plan_id."));
        const auto* Found = Plans.Find(Id);
        if (!Found) return FJevEditorBridge::Error(TEXT("unknown_plan"), TEXT("Compile receipt is unknown or no longer retained."));
        for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id")})
            if ((*Found)->Identity->GetStringField(Key) != Identity->GetStringField(Key)) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Compile receipt belongs to a different project or editor session."));
        (*Found)->Receipt->SetNumberField(TEXT("expires_in_seconds"), (*Found)->bConsumed ? 0 : FMath::Max(0.0, PreviewSeconds - (Clock() - (*Found)->CreatedAt)));
        if (!(*Found)->bConsumed && Clock() - (*Found)->CreatedAt >= PreviewSeconds) (*Found)->Receipt->SetStringField(TEXT("status"), TEXT("expired"));
        return Success((*Found)->Receipt.ToSharedRef());
    }
    if (bCompiling) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("A Blueprint compilation callback is already running."));
    if (Action == TEXT("blueprint_compile_preview")) return Preview(Params, Identity);
    if (Action == TEXT("blueprint_compile")) return Compile(Params, Identity);
    return FJevEditorBridge::Error(TEXT("unknown_action"), TEXT("Unsupported Blueprint action."));
}
