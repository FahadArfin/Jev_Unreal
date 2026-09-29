#include "JevEditorRuntimeGameplay.h"
#include "JevEditorBridge.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "PlayInEditorDataTypes.h"
#include "RenderingThread.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealClient.h"
#include "UObject/StrongObjectPtr.h"

namespace JevRuntime
{
const TCHAR* Section = TEXT("JevEditor.RuntimeGameplay");
bool Only(const TSharedPtr<FJsonObject>& P, const TArray<FString>& Keys)
{
    if (!P) return false;
    for (const auto& Pair : P->Values) if (!Keys.Contains(FString(*Pair.Key))) return false;
    return true;
}
bool Text(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, FString& Value, int32 Max = 2048)
{
    if (!P || !P->TryGetStringField(Key, Value) || Value.IsEmpty() || Value.Len() > Max) return false;
    for (TCHAR C : Value) if (FChar::IsControl(C)) return false;
    return true;
}
TSharedRef<FJsonObject> Base(const TSharedRef<FJsonObject>& Identity)
{
    auto R = MakeShared<FJsonObject>();
    for (const TCHAR* K : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) R->SetStringField(K, Identity->GetStringField(K));
    R->SetBoolField(TEXT("cloud_used"), false); return R;
}
TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& R)
{
    auto Response = MakeShared<FJsonObject>(); Response->SetBoolField(TEXT("ok"), true); Response->SetObjectField(TEXT("result"), R); return Response;
}
bool Same(const TSharedPtr<FJsonObject>& A, const TSharedRef<FJsonObject>& B, bool Revision)
{
    if (!A) return false;
    for (const TCHAR* K : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        if (!Revision && FString(K) == TEXT("revision")) continue;
        FString X, Y; if (!Text(A, K, X) || !Text(B, K, Y) || X != Y) return false;
    }
    return true;
}
bool Allowed(bool& Valid, bool& Enabled)
{
    Valid = true; Enabled = false; TArray<FString> Maps;
    if (GConfig) { GConfig->GetBool(Section, TEXT("bEnabled"), Enabled, GGameIni); GConfig->GetArray(Section, TEXT("Maps"), Maps, GGameIni); }
    Valid = Maps.Num() <= 64; TSet<FString> Seen;
    for (const FString& Map : Maps)
    {
        Valid &= Map.StartsWith(TEXT("/Game/")) && Map.Len() <= 1024 && !Map.Contains(TEXT("..")) && !Map.Contains(TEXT("\\")) && !Map.Contains(TEXT(".")) && !Seen.Contains(Map);
        for (TCHAR C : Map) Valid &= !FChar::IsWhitespace(C) && !FChar::IsControl(C);
        Seen.Add(Map);
    }
    UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    return Valid && Enabled && W && Maps.Contains(W->GetOutermost()->GetName());
}
UWorld* Standalone()
{
    if (!GEditor || !GEngine || GEditor->bIsSimulatingInEditor) return nullptr;
    UWorld* Found = nullptr; int32 Count = 0;
    for (const FWorldContext& C : GEngine->GetWorldContexts()) if (C.WorldType == EWorldType::PIE) { ++Count; Found = C.World(); }
    return Count == 1 && Found == GEditor->PlayWorld && Found && Found->HasBegunPlay() && Found->GetNetMode() == NM_Standalone ? Found : nullptr;
}
}

struct FJevRuntimeGameplay::FState
{
    struct FPlan { FString Operation, Owner; TSharedPtr<FJsonObject> Identity; TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>(); double Created = 0; bool Consumed = false; };
    TMap<FString, TSharedPtr<FPlan>> Plans;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<UWorld> World;
    TSharedPtr<FJsonObject> Identity;
    FString Owner, Pending;
    bool Owned() const
    {
        const auto Info = GEditor ? GEditor->GetPlayInEditorSessionInfo() : TOptional<FPlayInEditorSessionInfo>();
        return !Owner.IsEmpty() && Settings.IsValid() && World.IsValid() && JevRuntime::Standalone() == World.Get() && Info.IsSet() && Info->OriginalRequestParams.EditorPlaySettings == Settings.Get();
    }
};
FJevRuntimeGameplay::FJevRuntimeGameplay() : State(MakeUnique<FState>()) {}
FJevRuntimeGameplay::~FJevRuntimeGameplay() = default;
bool FJevRuntimeGameplay::HandlesAction(const FString& A) { return A == TEXT("runtime_status") || A == TEXT("runtime_preview") || A == TEXT("runtime_apply") || A == TEXT("runtime_receipt") || A == TEXT("runtime_capture"); }
bool FJevRuntimeGameplay::HasActiveJob() const { return !State->Pending.IsEmpty(); }
void FJevRuntimeGameplay::Shutdown() { State->Pending.Empty(); State->Plans.Reset(); /* Never stop a session as an unload side effect. */ }

void FJevRuntimeGameplay::Tick(const TSharedRef<FJsonObject>& Identity)
{
    check(IsInGameThread());
    auto Plan = State->Plans.FindRef(State->Pending); if (!Plan) return;
    FString Status;
    if (!JevRuntime::Same(Plan->Identity, Identity, false)) Status = TEXT("interrupted");
    else if (Plan->Operation == TEXT("start"))
    {
        const auto Info = GEditor ? GEditor->GetPlayInEditorSessionInfo() : TOptional<FPlayInEditorSessionInfo>();
        if (Info.IsSet() && Info->OriginalRequestParams.EditorPlaySettings != State->Settings.Get()) Status = TEXT("interrupted");
        else if (JevRuntime::Standalone() && Info.IsSet())
        {
            State->World = JevRuntime::Standalone(); State->Owner = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); State->Identity = JevRuntime::Base(Identity);
            Plan->Receipt->SetStringField(TEXT("owned_session_id"), State->Owner); Plan->Receipt->SetStringField(TEXT("pie_world_path"), State->World->GetPathName()); Status = TEXT("running");
        }
        else if (!GEditor || !GEditor->IsPlaySessionInProgress()) Status = TEXT("start_failed");
    }
    else if (!State->World.IsValid() || JevRuntime::Standalone() != State->World.Get()) Status = TEXT("stopped");
    if (Status.IsEmpty() && FPlatformTime::Seconds() - Plan->Created > 120) Status = TEXT("uncertain");
    if (!Status.IsEmpty()) { Plan->Receipt->SetStringField(TEXT("status"), Status); State->Pending.Empty(); }
}

TSharedRef<FJsonObject> FJevRuntimeGameplay::Execute(const FString& Action, const TSharedPtr<FJsonObject>& P, const TSharedRef<FJsonObject>& Identity)
{
    using namespace JevRuntime; check(IsInGameThread()); Tick(Identity);
    const double Now = FPlatformTime::Seconds();
    for (auto It = State->Plans.CreateIterator(); It; ++It) if (It.Key() != State->Pending && Now - It.Value()->Created > 900) It.RemoveCurrent();
    if (Action == TEXT("runtime_status"))
    {
        if (!Only(P, {})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("runtime_status takes no arguments."));
        bool Valid, Enabled; const bool Approved = Allowed(Valid, Enabled); auto R = Base(Identity);
        R->SetBoolField(TEXT("enabled"), Enabled && Valid); R->SetBoolField(TEXT("configuration_valid"), Valid); R->SetBoolField(TEXT("map_approved"), Approved);
        R->SetBoolField(TEXT("owned_session"), State->Owned()); R->SetBoolField(TEXT("transition_pending"), HasActiveJob());
        if (State->Owned()) { R->SetStringField(TEXT("owned_session_id"), State->Owner); R->SetStringField(TEXT("pie_world_path"), State->World->GetPathName()); }
        R->SetStringField(TEXT("scope"), TEXT("Reviewed standalone PIE for explicitly listed current maps. Project BeginPlay/EndPlay callbacks run trusted code. No map load, console, input injection, multiplayer or packaged launch.")); return Success(R);
    }
    if (Action == TEXT("runtime_preview"))
    {
        FString Project, Operation, Owner; const TSharedPtr<FJsonObject>* Expected = nullptr;
        if (!Only(P, {TEXT("operation"), TEXT("owned_session_id"), TEXT("expected_project"), TEXT("expected_state")}) || !Text(P, TEXT("expected_project"), Project) || !Text(P, TEXT("operation"), Operation, 5) || (Operation != TEXT("start") && Operation != TEXT("stop")) || !P->TryGetObjectField(TEXT("expected_state"), Expected) || !Only(*Expected, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply start/stop, exact expected project and inspected state."));
        auto ExpectedIdentity = MakeShared<FJsonObject>(); ExpectedIdentity->Values = (*Expected)->Values; ExpectedIdentity->SetStringField(TEXT("project_file"), Project);
        if (!Same(ExpectedIdentity, Identity, true)) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("Inspect this exact project/session/world/revision again."));
        bool Valid, Enabled; const bool Approved = Allowed(Valid, Enabled);
        if (Operation == TEXT("start") && !Approved) return FJevEditorBridge::Error(TEXT("runtime_disabled"), TEXT("Enable RuntimeGameplay and approve this current /Game map locally."));
        if (HasActiveJob()) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("A PIE transition is pending."));
        if (Operation == TEXT("start") && (P->HasField(TEXT("owned_session_id")) || !GEditor || GEditor->IsPlaySessionInProgress() || GEditor->PlayWorld)) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("Start requires no current or queued play session."));
        if (Operation == TEXT("stop") && (!Text(P, TEXT("owned_session_id"), Owner, 36) || Owner != State->Owner || !State->Owned() || !Same(State->Identity, Identity, false))) return FJevEditorBridge::Error(TEXT("session_not_owned"), TEXT("Only the exact surviving PIE session started by this bridge can be stopped."));
        if (State->Plans.Num() >= 64) return FJevEditorBridge::Error(TEXT("too_many_plans"), TEXT("Wait for old runtime receipts to expire."));
        auto Plan = MakeShared<FState::FPlan>(); Plan->Operation = Operation; Plan->Owner = Owner; Plan->Created = Now; Plan->Identity = Base(Identity); Plan->Receipt = Base(Identity);
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); Plan->Receipt->SetStringField(TEXT("plan_id"), Id); Plan->Receipt->SetStringField(TEXT("operation"), Operation); Plan->Receipt->SetStringField(TEXT("status"), TEXT("pending")); Plan->Receipt->SetNumberField(TEXT("expires_in_seconds"), 120);
        Plan->Receipt->SetStringField(TEXT("review"), TEXT("One-shot PIE lifecycle request. Project callbacks may change or save assets; no rollback or hard callback timeout. Read the receipt after uncertain transport failure; never blindly retry.")); State->Plans.Add(Id, Plan); return Success(Plan->Receipt);
    }
    if (Action == TEXT("runtime_apply") || Action == TEXT("runtime_receipt"))
    {
        FString Id, Project; const bool Apply = Action == TEXT("runtime_apply");
        if (!Only(P, Apply ? TArray<FString>{TEXT("plan_id"), TEXT("expected_project")} : TArray<FString>{TEXT("plan_id")}) || !Text(P, TEXT("plan_id"), Id, 36)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply a runtime plan ID."));
        const auto Plan = State->Plans.FindRef(Id); if (!Plan) return FJevEditorBridge::Error(TEXT("unknown_plan"), TEXT("Runtime plan is unknown or expired."));
        if (!Same(Plan->Identity, Identity, false)) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Runtime receipt belongs to another project/session/world."));
        if (!Apply) return Success(Plan->Receipt);
        if (Plan->Consumed) return FJevEditorBridge::Error(TEXT("plan_consumed"), TEXT("Read the existing receipt; this one-shot plan has already been consumed."));
        Plan->Consumed = true; Plan->Receipt->SetStringField(TEXT("status"), TEXT("rejected"));
        if (!Text(P, TEXT("expected_project"), Project) || Project != Identity->GetStringField(TEXT("project_file")) || !Same(Plan->Identity, Identity, true) || Now - Plan->Created > 120) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("The reviewed runtime state changed or expired."));
        if (HasActiveJob()) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("A PIE transition is pending."));
        if (Plan->Operation == TEXT("start"))
        {
            bool Valid, Enabled; if (!Allowed(Valid, Enabled) || !GEditor || GEditor->IsPlaySessionInProgress() || GEditor->PlayWorld) return FJevEditorBridge::Error(TEXT("runtime_disabled"), TEXT("Map policy or play state changed."));
            State->Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
            State->Settings->SetPlayNetMode(PIE_Standalone); State->Settings->SetPlayNumberOfClients(1); State->Settings->SetRunUnderOneProcess(true); State->Settings->bLaunchSeparateServer = false;
            FRequestPlaySessionParams Request; Request.EditorPlaySettings = State->Settings.Get(); Request.bAllowOnlineSubsystem = false; Request.SessionDestination = EPlaySessionDestinationType::InProcess; Request.WorldType = EPlaySessionWorldType::PlayInEditor;
            State->Owner.Empty(); State->World.Reset(); State->Pending = Id; Plan->Created = Now; Plan->Receipt->SetStringField(TEXT("status"), TEXT("starting")); GEditor->RequestPlaySession(Request);
            // UE copies play settings inside RequestPlaySession. Bind ownership to the
            // exact queued copy now, before a later user request can replace that request.
            const auto Queued = GEditor->GetPlaySessionRequest();
            if (!Queued.IsSet() || !Queued->EditorPlaySettings) { State->Pending.Empty(); Plan->Receipt->SetStringField(TEXT("status"), TEXT("start_failed")); }
            else State->Settings.Reset(Queued->EditorPlaySettings);
        }
        else
        {
            if (!State->Owned() || Plan->Owner != State->Owner) return FJevEditorBridge::Error(TEXT("session_not_owned"), TEXT("The original owned PIE session no longer exists."));
            State->Pending = Id; Plan->Created = Now; Plan->Receipt->SetStringField(TEXT("status"), TEXT("stopping")); GEditor->RequestEndPlayMap();
        }
        return Success(Plan->Receipt);
    }
    if (Action == TEXT("runtime_capture"))
    {
        FString Owner; double Max = 1024;
        if (!Only(P, {TEXT("owned_session_id"), TEXT("max_dimension")}) || !Text(P, TEXT("owned_session_id"), Owner, 36) || (P->HasField(TEXT("max_dimension")) && (!P->HasTypedField<EJson::Number>(TEXT("max_dimension")) || !P->TryGetNumberField(TEXT("max_dimension"), Max))) || !FMath::IsFinite(Max) || Max < 64 || Max > 1024 || Max != FMath::TruncToDouble(Max)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply owned_session_id and optional integer max_dimension 64..1024."));
        if (!State->Owned() || Owner != State->Owner || !Same(State->Identity, Identity, false)) return FJevEditorBridge::Error(TEXT("session_not_owned"), TEXT("Capture requires this bridge's exact running standalone PIE world."));
        UWorld* World = State->World.Get(); UGameViewportClient* Client = World->GetGameViewport(); FViewport* Viewport = Client ? Client->Viewport : nullptr;
        if (!Viewport || FScreenshotRequest::IsScreenshotRequested() || GIsHighResScreenshot || GIsDumpingMovie) return FJevEditorBridge::Error(TEXT("viewport_unavailable"), TEXT("Owned game viewport is unavailable or another capture is active."));
        const FIntPoint Size = Viewport->GetSizeXY(); if (Size.X <= 0 || Size.Y <= 0 || static_cast<int64>(Size.X) * Size.Y > 16 * 1024 * 1024) return FJevEditorBridge::Error(TEXT("capture_too_large"), TEXT("Use a nonempty viewport below 16 million pixels."));
        World->SendAllEndOfFrameUpdates(); Viewport->Draw(false); FlushRenderingCommands(); TArray<FColor> Pixels;
        if (!GetViewportScreenShot(Viewport, Pixels) || Pixels.Num() != Size.X * Size.Y) return FJevEditorBridge::Error(TEXT("capture_failed"), TEXT("Owned game viewport readback failed."));
        for (FColor& Pixel : Pixels) Pixel.A = 255;
        const double Ratio = FMath::Min(1.0, Max / FMath::Max(Size.X, Size.Y)); int32 Width = FMath::Max(1, FMath::RoundToInt(Size.X * Ratio)), Height = FMath::Max(1, FMath::RoundToInt(Size.Y * Ratio)); TArray64<uint8> Png;
        for (int32 Attempt = 0; Attempt < 6; ++Attempt) { TArray<FColor> Scaled; FImageUtils::ImageResize(Size.X, Size.Y, Pixels, Width, Height, Scaled, true); Png.Reset(); FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Scaled.GetData(), Scaled.Num()), Png); if (Png.Num() > 0 && Png.Num() <= 720 * 1024) break; Width = FMath::Max(1, Width * 3 / 4); Height = FMath::Max(1, Height * 3 / 4); }
        if (Png.IsEmpty() || Png.Num() > 720 * 1024) return FJevEditorBridge::Error(TEXT("capture_too_large"), TEXT("PNG exceeded bounded response."));
        auto R = Base(Identity); R->SetStringField(TEXT("owned_session_id"), Owner); R->SetStringField(TEXT("pie_world_path"), World->GetPathName()); R->SetStringField(TEXT("source"), TEXT("owned_pie_viewport")); R->SetStringField(TEXT("mime_type"), TEXT("image/png")); R->SetStringField(TEXT("data"), FBase64::Encode(Png.GetData(), static_cast<uint32>(Png.Num()))); R->SetNumberField(TEXT("width"), Width); R->SetNumberField(TEXT("height"), Height); R->SetNumberField(TEXT("world_time_seconds"), World->GetTimeSeconds()); return Success(R);
    }
    return FJevEditorBridge::Error(TEXT("unknown_action"), TEXT("Unknown runtime action."));
}
