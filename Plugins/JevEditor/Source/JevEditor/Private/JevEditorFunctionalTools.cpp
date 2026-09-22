#include "JevEditorFunctionalTools.h"
#include "JevEditorBridge.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FunctionalTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

namespace JevFunctional
{
constexpr double Retention = 900;
const TCHAR* Section = TEXT("JevEditor.FunctionalTesting");
struct FTest { FString Id; FString Path; };

bool Only(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& Fields)
{
    if (!Params) return false;
    for (const auto& Pair : Params->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool String(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FString& Out, int32 Max)
{
    if (!Params || !Params->HasTypedField<EJson::String>(Key) || !Params->TryGetStringField(Key, Out) || Out.IsEmpty() || Out.Len() > Max) return false;
    for (const TCHAR C : Out) if (FChar::IsControl(C)) return false;
    return true;
}

bool Id(const FString& Text)
{
    if (Text.IsEmpty() || Text.Len() > 64) return false;
    for (const TCHAR C : Text) if (!((C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.'))) return false;
    return true;
}

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>(); Response->SetBoolField(TEXT("ok"), true); Response->SetObjectField(TEXT("result"), Result); return Response;
}

TSharedRef<FJsonObject> Base(const TSharedRef<FJsonObject>& Identity)
{
    auto Result = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        FString Value; if (Identity->TryGetStringField(Key, Value)) Result->SetStringField(Key, Value);
    }
    Result->SetBoolField(TEXT("cloud_used"), false); return Result;
}

bool Same(const TSharedRef<FJsonObject>& A, const TSharedRef<FJsonObject>& B, bool bRevision = false)
{
    for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        if (!bRevision && FString(Key) == TEXT("revision")) continue;
        FString AV, BV;
        if (!String(A, Key, AV, 2048) || !String(B, Key, BV, 2048) || AV != BV) return false;
    }
    return true;
}

TArray<FTest> Policy(bool& bEnabled, bool& bValid, double& Seconds)
{
    bEnabled = false; bValid = true; Seconds = 30;
    TArray<FString> Entries;
    if (GConfig)
    {
        GConfig->GetBool(Section, TEXT("bEnabled"), bEnabled, GGameIni);
        GConfig->GetDouble(Section, TEXT("MaxJobSeconds"), Seconds, GGameIni);
        GConfig->GetArray(Section, TEXT("Tests"), Entries, GGameIni);
    }
    bValid = Entries.Num() <= 64 && FMath::IsFinite(Seconds) && Seconds >= 1 && Seconds <= 120;
    TArray<FTest> Tests; TSet<FString> Seen;
    for (const auto& Entry : Entries)
    {
        FString Alias, Path;
        bool bEntryValid = Entry.Split(TEXT("|"), &Alias, &Path) && Id(Alias) && !Seen.Contains(Alias) && Path.StartsWith(TEXT("/")) && Path.Len() <= 1024 && !Path.Contains(TEXT("|")) && !Path.Contains(TEXT("..")) && !Path.Contains(TEXT("\\"));
        for (const TCHAR C : Path) bEntryValid &= !FChar::IsControl(C);
        if (!bEntryValid) { bValid = false; continue; }
        Seen.Add(Alias); if (Tests.Num() < 64) Tests.Add({Alias, Path});
    }
    return Tests;
}

UWorld* PIEWorld()
{
    if (!GEditor || !GEngine || !GEditor->PlayWorld || GEditor->bIsSimulatingInEditor) return nullptr;
    UWorld* Result = nullptr; int32 Count = 0;
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
        if (Context.WorldType == EWorldType::PIE) { ++Count; Result = Context.World(); }
    return Count == 1 && Result == GEditor->PlayWorld && Result->GetNetMode() == NM_Standalone && Result->HasBegunPlay() ? Result : nullptr;
}

AFunctionalTest* EditorTest(const FString& Path)
{
    AFunctionalTest* Test = FindObject<AFunctionalTest>(nullptr, *Path);
    return Test && GEditor && !Test->IsActorBeingDestroyed() && Test->GetWorld() == GEditor->GetEditorWorldContext().World() && Test->GetPathName() == Path ? Test : nullptr;
}

AFunctionalTest* PIETest(AFunctionalTest* EditorActor, UWorld* World)
{
    AFunctionalTest* Test = EditorActor ? Cast<AFunctionalTest>(EditorUtilities::GetSimWorldCounterpartActor(EditorActor)) : nullptr;
    return Test && !Test->IsActorBeingDestroyed() && Test->GetWorld() == World ? Test : nullptr;
}

bool OtherTestRunning(UWorld* World, AFunctionalTest* Exclude = nullptr)
{
    int32 Count = 0;
    for (TActorIterator<AFunctionalTest> It(World); It; ++It)
    {
        if (++Count > 1000) return true;
        if (*It != Exclude && It->IsRunning()) return true;
    }
    return false;
}

class FErrorObserver : public FOutputDevice
{
public:
    FCriticalSection Mutex;
    int32 Count = 0;
    bool bTruncated = false;
    TArray<FString> Messages;
    bool bRegistered = false;
    FErrorObserver() { if (GLog) { GLog->AddOutputDevice(this); bRegistered = true; } }
    void Stop() { if (bRegistered && GLog) GLog->RemoveOutputDevice(this); bRegistered = false; }
    virtual ~FErrorObserver() { Stop(); }
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    void Capture(int32& OutCount, TArray<FString>& OutMessages, bool& OutTruncated)
    {
        FScopeLock Lock(&Mutex); OutCount = Count; OutMessages = Messages; OutTruncated |= bTruncated;
    }
    virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (Category != FName(TEXT("LogFunctionalTest")) || (Verbosity & ELogVerbosity::VerbosityMask) > ELogVerbosity::Error) return;
        FScopeLock Lock(&Mutex);
        Count = Count < MAX_int32 ? Count + 1 : MAX_int32;
        if (Messages.Num() >= 32) { bTruncated = true; return; }
        const FString Message(Text); bTruncated |= Message.Len() > 512; Messages.Add(Message.Left(512));
    }
};

int32 AutomationDiagnostics(FAutomationTestBase* Test)
{
    if (!Test || FAutomationTestFramework::Get().GetCurrentTest() != Test) return 0;
    FAutomationTestExecutionInfo Info; Test->GetExecutionInfo(Info);
    TArray<FAutomationExpectedMessage> Expected; Test->GetExpectedMessages(Expected, true);
    int32 Total = Info.GetErrorTotal();
    // Expected fixture assertions are omitted from the framework's error total;
    // still treat their occurrence as diagnostic evidence for this owned job.
    for (const auto& Message : Expected) if (Message.Verbosity <= ELogVerbosity::Warning) Total += Message.ActualNumberOfOccurrences;
    return Total;
}
}

struct FJevFunctionalTools::FJob
{
    FString Id, TestId, EditorPath, PIEPath;
    FString State = TEXT("queued"), Reason, NativeResult = TEXT("not_started"), Message;
    TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AFunctionalTest> EditorActor, Test;
    double Started = 0, Finished = 0, Timeout = 30;
    bool bStarted = false, bCleanupAttempted = false, bTruncated = false;
    TUniquePtr<JevFunctional::FErrorObserver> Observer;
    FAutomationTestBase* Automation = nullptr;
    int32 InitialAutomationDiagnostics = 0, ObservedErrors = 0;
    TArray<FString> Errors;
    uint32 RunFrame = 0;
    float RunTime = 0;
    bool bRunIdentityCaptured = false;
};

FJevFunctionalTools::FJevFunctionalTools() = default;
FJevFunctionalTools::~FJevFunctionalTools() = default;

bool FJevFunctionalTools::HandlesAction(const FString& Action)
{
    return Action == TEXT("functional_tests") || Action == TEXT("functional_start") || Action == TEXT("functional_job") || Action == TEXT("functional_cancel");
}

TSharedRef<FJsonObject> FJevFunctionalTools::Snapshot(const FJob& Job) const
{
    auto Result = JevFunctional::Base(Job.Identity);
    Result->SetStringField(TEXT("job_id"), Job.Id); Result->SetStringField(TEXT("test_id"), Job.TestId);
    Result->SetStringField(TEXT("editor_actor_path"), Job.EditorPath); Result->SetStringField(TEXT("pie_actor_path"), Job.PIEPath);
    Result->SetStringField(TEXT("state"), Job.State); Result->SetStringField(TEXT("stop_reason"), Job.Reason);
    Result->SetStringField(TEXT("native_result"), Job.NativeResult); Result->SetStringField(TEXT("message"), Job.Message);
    Result->SetBoolField(TEXT("started"), Job.bStarted); Result->SetBoolField(TEXT("cleanup_attempted"), Job.bCleanupAttempted);
    Result->SetBoolField(TEXT("truncated"), Job.bTruncated);
    Result->SetBoolField(TEXT("save_requested"), false); Result->SetField(TEXT("saved"), MakeShared<FJsonValueNull>()); Result->SetBoolField(TEXT("callback_side_effects_tracked"), false);
    Result->SetNumberField(TEXT("observed_error_count"), Job.ObservedErrors);
    TArray<TSharedPtr<FJsonValue>> Errors; for (const auto& Message : Job.Errors) Errors.Add(MakeShared<FJsonValueString>(Message));
    Result->SetArrayField(TEXT("observed_errors"), Errors);
    Result->SetNumberField(TEXT("elapsed_seconds"), (Job.Finished > 0 ? Job.Finished : FPlatformTime::Seconds()) - Job.Started);
    Result->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, JevFunctional::Retention - (FPlatformTime::Seconds() - (Job.Finished > 0 ? Job.Finished : Job.Started))));
    Result->SetStringField(TEXT("scope"), TEXT("One explicitly approved project-owned test in an already-running standalone PIE world. No PIE launch/stop, reset/retry or save. Project test code and cleanup may have side effects."));
    Result->SetStringField(TEXT("cancellation"), TEXT("Cooperative between game-thread callbacks; running test code cannot be interrupted. Cleanup is attempted only for this job's surviving owned test in the original PIE world."));
    return JevFunctional::Success(Result);
}

void FJevFunctionalTools::Finish(FJob& Job, const FString& State, const FString& Reason, bool bMayCleanUp)
{
    Job.State = State; Job.Reason = Reason;
    if (const AFunctionalTest* Test = Job.Test.Get(); Job.bRunIdentityCaptured && Test && (Test->RunFrame != Job.RunFrame || Test->RunTime != Job.RunTime))
    {
        Job.State = TEXT("interrupted"); Job.Reason = TEXT("test_run_changed"); bMayCleanUp = false;
    }
    TGuardValue<bool> CallbackGuard(bExecutingCallbacks, true);
    auto OwnedTest = [&Job]() -> AFunctionalTest*
    {
        AFunctionalTest* Test = Job.Test.Get();
        UWorld* World = Job.World.Get();
        return World && JevFunctional::PIEWorld() == World && Test && !Test->IsActorBeingDestroyed() && Test->GetWorld() == World && (!Job.bRunIdentityCaptured || (Test->RunFrame == Job.RunFrame && Test->RunTime == Job.RunTime)) && JevFunctional::EditorTest(Job.EditorPath) == Job.EditorActor.Get() && JevFunctional::PIETest(Job.EditorActor.Get(), World) == Test ? Test : nullptr;
    };
    if (bMayCleanUp && Job.bStarted && !Job.bCleanupAttempted)
    {
        if (AFunctionalTest* Test = OwnedTest())
        {
            if (Test->IsRunning()) Test->FinishTest(EFunctionalTestResult::Error, TEXT("Jev stopped its owned functional test: ") + Reason);
            // Finish callbacks can end PIE, destroy/restart this test, or replace actors.
            Test = OwnedTest();
            if (Test && !Test->IsRunning() && ActiveJob == Job.Id)
            {
                Job.NativeResult = LexToString(Test->Result);
                Job.bTruncated |= Test->FailureMessage.Len() > 1024;
                Job.Message = Test->FailureMessage.Left(1024);
                Job.bCleanupAttempted = true;
                Test->CleanUp();
                Test = OwnedTest();
                if (!Test || Test->IsRunning()) { Job.State = TEXT("interrupted"); Job.Reason = TEXT("identity_changed_during_cleanup"); }
            }
            else if (ActiveJob == Job.Id) { Job.State = TEXT("interrupted"); Job.Reason = TEXT("identity_changed_during_finish"); }
        }
        else if (Job.State == TEXT("passed")) { Job.State = TEXT("interrupted"); Job.Reason = TEXT("cleanup_unavailable"); }
    }
    if (Job.Observer)
    {
        Job.Observer->Stop();
        Job.Observer->Capture(Job.ObservedErrors, Job.Errors, Job.bTruncated);
        Job.ObservedErrors += FMath::Max(0, JevFunctional::AutomationDiagnostics(Job.Automation) - Job.InitialAutomationDiagnostics);
        Job.Observer.Reset();
    }
    if (Job.State == TEXT("passed") && Job.ObservedErrors > 0) { Job.State = TEXT("failed"); Job.Reason = TEXT("observed_test_errors"); }
    Job.Finished = FPlatformTime::Seconds();
    if (ActiveJob == Job.Id) ActiveJob.Empty();
}

TSharedRef<FJsonObject> FJevFunctionalTools::Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    check(IsInGameThread());
    if (bExecutingCallbacks && (Action == TEXT("functional_start") || Action == TEXT("functional_cancel"))) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("A project test callback cannot reenter functional job mutation."));
    if (Action == TEXT("functional_tests"))
    {
        if (!JevFunctional::Only(Params, {})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("functional_tests accepts no fields."));
        bool bEnabled, bValid; double Timeout;
        const auto Tests = JevFunctional::Policy(bEnabled, bValid, Timeout);
        auto Result = JevFunctional::Base(Identity);
        Result->SetBoolField(TEXT("enabled"), bEnabled && bValid); Result->SetBoolField(TEXT("configuration_valid"), bValid);
        Result->SetBoolField(TEXT("standalone_pie_ready"), JevFunctional::PIEWorld() != nullptr);
        Result->SetNumberField(TEXT("max_job_seconds"), bValid ? Timeout : 0);
        Result->SetStringField(TEXT("configuration_section"), TEXT("JevEditor.FunctionalTesting in project DefaultGame.ini"));
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const auto& Entry : Tests)
        {
            auto Row = MakeShared<FJsonObject>(); AFunctionalTest* Test = JevFunctional::EditorTest(Entry.Path);
            Row->SetStringField(TEXT("id"), Entry.Id); Row->SetStringField(TEXT("editor_actor_path"), Entry.Path);
            Row->SetBoolField(TEXT("editor_actor_available"), Test != nullptr);
            Row->SetBoolField(TEXT("test_enabled"), Test && Test->IsEnabled());
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("tests"), Rows);
        return JevFunctional::Success(Result);
    }
    if (Action == TEXT("functional_start"))
    {
        if (FAutomationTestFramework::Get().GetCurrentTest() && !bAutomationFixture) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("MCP cannot start a functional test inside another automation test."));
        FString Id; const TSharedPtr<FJsonObject>* Expected = nullptr;
        if (!JevFunctional::Only(Params, {TEXT("test_id"), TEXT("expected_state")}) || !JevFunctional::String(Params, TEXT("test_id"), Id, 64) || !JevFunctional::Id(Id) || !Params->TryGetObjectField(TEXT("expected_state"), Expected) || !JevFunctional::Only(*Expected, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("A configured test ID and exact session/world/revision expected_state are required."));
        for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
        {
            FString A, B;
            if (!JevFunctional::String(*Expected, Key, A, 1024)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("expected_state requires nonempty bounded identity strings."));
            if (!Identity->TryGetStringField(Key, B) || A != B) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("Inspect the current editor state before starting this project test."));
        }
        if (!JevFunctional::Same(Identity, Identity, true)) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("A current explicit project identity is required."));
        bool bEnabled, bValid; double Timeout;
        const auto Tests = JevFunctional::Policy(bEnabled, bValid, Timeout);
        if (!bEnabled || !bValid) return FJevEditorBridge::Error(TEXT("functional_disabled"), TEXT("Project functional-test execution is disabled or configuration is invalid."));
        if (!ActiveJob.IsEmpty()) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("A functional test job is already active."));
        const auto* Entry = Tests.FindByPredicate([&Id](const JevFunctional::FTest& Test) { return Test.Id == Id; });
        if (!Entry) return FJevEditorBridge::Error(TEXT("test_not_allowed"), TEXT("The requested named test is not approved by this project."));
        UWorld* World = JevFunctional::PIEWorld();
        if (!World) return FJevEditorBridge::Error(TEXT("pie_required"), TEXT("Start exactly one standalone Play In Editor session first. Simulate and multiplayer worlds are unsupported."));
        AFunctionalTest* EditorActor = JevFunctional::EditorTest(Entry->Path);
        AFunctionalTest* Test = JevFunctional::PIETest(EditorActor, World);
        if (!Test || !Test->IsEnabledInWorld(World)) return FJevEditorBridge::Error(TEXT("test_unavailable"), TEXT("The exact configured test must have an enabled counterpart in the current PIE world."));
        if (JevFunctional::OtherTestRunning(World)) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("Another functional test is already running, or the bounded test scan cannot establish isolation."));
        const double Now = FPlatformTime::Seconds();
        for (auto It = Jobs.CreateIterator(); It; ++It) if (Now - It.Value()->Finished >= JevFunctional::Retention) It.RemoveCurrent();
        while (Jobs.Num() >= 64)
        {
            FString Oldest; double Time = DBL_MAX;
            for (const auto& Pair : Jobs) if (Pair.Value->Started < Time) { Time = Pair.Value->Started; Oldest = Pair.Key; }
            Jobs.Remove(Oldest);
        }
        auto Job = MakeShared<FJob>();
        Job->Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); Job->TestId = Id;
        Job->EditorPath = Entry->Path; Job->PIEPath = Test->GetPathName(); Job->Identity = JevFunctional::Base(Identity);
        Job->World = World; Job->EditorActor = EditorActor; Job->Test = Test; Job->Started = Now; Job->Timeout = Timeout;
        Jobs.Add(Job->Id, Job); ActiveJob = Job->Id;
        return Snapshot(*Job);
    }
    if (Action == TEXT("functional_job") || Action == TEXT("functional_cancel"))
    {
        FString Id; FGuid Guid;
        if (!JevFunctional::Only(Params, {TEXT("job_id")}) || !JevFunctional::String(Params, TEXT("job_id"), Id, 36) || !FGuid::ParseExact(Id, EGuidFormats::DigitsWithHyphens, Guid)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("A canonical functional job UUID is required."));
        const auto Job = Jobs.FindRef(Id);
        if (!Job || (Job->Finished > 0 && FPlatformTime::Seconds() - Job->Finished >= JevFunctional::Retention)) return FJevEditorBridge::Error(TEXT("unknown_job"), TEXT("The functional job is unknown or expired."));
        if (!JevFunctional::Same(Job->Identity, Identity)) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("This test receipt belongs to a different editor project, session or world."));
        if (Action == TEXT("functional_cancel") && ActiveJob == Id) Finish(*Job, TEXT("cancelled"), TEXT("requested"), true);
        return Snapshot(*Job);
    }
    return FJevEditorBridge::Error(TEXT("unknown_action"), TEXT("Unknown functional-testing action."));
}

void FJevFunctionalTools::Tick(const TSharedRef<FJsonObject>& Identity)
{
    check(IsInGameThread());
    const auto Job = Jobs.FindRef(ActiveJob); if (!Job) return;
    if (bExecutingCallbacks) return;
    if (!JevFunctional::Same(Job->Identity, Identity)) { Finish(*Job, TEXT("interrupted"), TEXT("identity_changed"), true); return; }
    UWorld* World = JevFunctional::PIEWorld();
    AFunctionalTest* Test = Job->Test.Get();
    if (!World || World != Job->World.Get() || !Test || Test->GetWorld() != World || Test->IsActorBeingDestroyed() || JevFunctional::EditorTest(Job->EditorPath) != Job->EditorActor.Get() || JevFunctional::PIETest(Job->EditorActor.Get(), World) != Test) { Finish(*Job, TEXT("interrupted"), TEXT("pie_or_test_changed"), false); return; }
    if (Job->bRunIdentityCaptured && (Test->RunFrame != Job->RunFrame || Test->RunTime != Job->RunTime)) { Finish(*Job, TEXT("interrupted"), TEXT("test_run_changed"), false); return; }
    bool bEnabled, bValid; double Timeout;
    const auto Tests = JevFunctional::Policy(bEnabled, bValid, Timeout);
    if (!bEnabled || !bValid || !Tests.ContainsByPredicate([&Job](const JevFunctional::FTest& Entry) { return Entry.Id == Job->TestId && Entry.Path == Job->EditorPath; })) { Finish(*Job, TEXT("interrupted"), TEXT("configuration_changed"), true); return; }
    if (FPlatformTime::Seconds() - Job->Started >= Job->Timeout) { Finish(*Job, TEXT("timed_out"), TEXT("time_budget"), true); return; }
    if (JevFunctional::OtherTestRunning(World, Job->bStarted ? Test : nullptr)) { Finish(*Job, TEXT("interrupted"), TEXT("another_test_running"), true); return; }
    if (!Job->bStarted)
    {
        if (!JevFunctional::Same(Job->Identity, Identity, true)) { Finish(*Job, TEXT("interrupted"), TEXT("stale_state"), false); return; }
        if (!Test->IsEnabledInWorld(World)) { Finish(*Job, TEXT("error"), TEXT("test_disabled"), false); return; }
        Job->bStarted = true; Job->State = TEXT("running");
        Job->Automation = FAutomationTestFramework::Get().GetCurrentTest();
        Job->InitialAutomationDiagnostics = JevFunctional::AutomationDiagnostics(Job->Automation);
        Job->Observer = MakeUnique<JevFunctional::FErrorObserver>();
        TGuardValue<bool> CallbackGuard(bExecutingCallbacks, true);
        const bool bStarted = Test->RunTest({});
        if (ActiveJob != Job->Id) { Job->Observer.Reset(); return; }
        Test = Job->Test.Get();
        if (!Test || Test->IsActorBeingDestroyed() || JevFunctional::PIEWorld() != Job->World.Get()) { Finish(*Job, TEXT("interrupted"), TEXT("identity_changed_during_start"), false); return; }
        Job->RunFrame = Test->RunFrame; Job->RunTime = Test->RunTime; Job->bRunIdentityCaptured = true;
        if (ActiveJob == Job->Id && Job->State == TEXT("running") && !bStarted) Finish(*Job, TEXT("error"), TEXT("start_failed"), true);
        return; // Observe results with a fresh identity on the following tick.
    }
    if (!Test->IsRunning())
    {
        Job->NativeResult = LexToString(Test->Result);
        const FString State = Test->Result == EFunctionalTestResult::Succeeded ? TEXT("passed") : Test->Result == EFunctionalTestResult::Failed ? TEXT("failed") : TEXT("error");
        Finish(*Job, State, TEXT("native_result"), true);
    }
}

void FJevFunctionalTools::Shutdown()
{
    if (bExecutingCallbacks)
    {
        if (const auto Job = Jobs.FindRef(ActiveJob)) { Job->State = TEXT("interrupted"); Job->Reason = TEXT("shutdown_during_callback"); Job->Finished = FPlatformTime::Seconds(); }
        ActiveJob.Empty(); return;
    }
    if (const auto Job = Jobs.FindRef(ActiveJob)) Finish(*Job, TEXT("interrupted"), TEXT("editor_shutdown"), true);
}
