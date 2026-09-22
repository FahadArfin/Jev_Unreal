#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "JevEditorFunctionalTools.h"
#include "JevFunctionalTestFixture.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FunctionalTestBase.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"

namespace JevFunctionalTests
{
const TCHAR* Section = TEXT("JevEditor.FunctionalTesting");

TSharedRef<FJsonObject> Identity(FJevEditorBridge& Bridge)
{
    auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("action"), TEXT("status")); Request->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
    return Bridge.Execute(Request)->GetObjectField(TEXT("result")).ToSharedRef();
}

TSharedRef<FJsonObject> Start(const FString& Id, const TSharedRef<FJsonObject>& Current)
{
    auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("test_id"), Id);
    auto Expected = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) Expected->SetStringField(Key, Current->GetStringField(Key));
    Params->SetObjectField(TEXT("expected_state"), Expected); return Params;
}

FString ErrorCode(const TSharedRef<FJsonObject>& Response)
{
    return Response->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : Response->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
}

struct FState
{
    FAutomationTestBase* Test = nullptr;
    FJevEditorBridge Bridge;
    FJevFunctionalTools Tools;
    TSharedRef<FJsonObject> Job = MakeShared<FJsonObject>();
    bool HadEnabled = false, Enabled = false, HadTimeout = false;
    double Timeout = 30, Begin = FPlatformTime::Seconds();
    TArray<FString> OldTests;
    EPlayNetMode OldMode = PIE_Standalone;
    int32 OldClients = 1;
    bool OldProcess = true, OldServer = false;
    int32 Stage = 0;
    FState(FAutomationTestBase* InTest) : Test(InTest)
    {
        Tools.PermitAutomationFixture();
        HadEnabled = GConfig->GetBool(Section, TEXT("bEnabled"), Enabled, GGameIni);
        HadTimeout = GConfig->GetDouble(Section, TEXT("MaxJobSeconds"), Timeout, GGameIni);
        GConfig->GetArray(Section, TEXT("Tests"), OldTests, GGameIni);
        auto* Settings = GetMutableDefault<ULevelEditorPlaySettings>();
        Settings->GetPlayNetMode(OldMode); Settings->GetPlayNumberOfClients(OldClients); Settings->GetRunUnderOneProcess(OldProcess); OldServer = Settings->bLaunchSeparateServer;
        Settings->SetPlayNetMode(PIE_Standalone); Settings->SetPlayNumberOfClients(1); Settings->SetRunUnderOneProcess(true); Settings->bLaunchSeparateServer = false;
    }
    ~FState()
    {
        Tools.Shutdown();
        if (HadEnabled) GConfig->SetBool(Section, TEXT("bEnabled"), Enabled, GGameIni); else GConfig->RemoveKey(Section, TEXT("bEnabled"), GGameIni);
        if (HadTimeout) GConfig->SetDouble(Section, TEXT("MaxJobSeconds"), Timeout, GGameIni); else GConfig->RemoveKey(Section, TEXT("MaxJobSeconds"), GGameIni);
        GConfig->SetArray(Section, TEXT("Tests"), OldTests, GGameIni);
        auto* Settings = GetMutableDefault<ULevelEditorPlaySettings>();
        Settings->SetPlayNetMode(OldMode); Settings->SetPlayNumberOfClients(OldClients); Settings->SetRunUnderOneProcess(OldProcess); Settings->bLaunchSeparateServer = OldServer;
    }
    bool BeginJob(const TCHAR* Id)
    {
        auto Current = Identity(Bridge);
        const auto Response = Tools.Execute(TEXT("functional_start"), Start(Id, Current), Current);
        if (!Test->TestTrue(TEXT("approved project test starts"), Response->GetBoolField(TEXT("ok")))) return false;
        Job = MakeShared<FJsonObject>(); Job->SetStringField(TEXT("job_id"), Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("job_id")));
        return true;
    }
    TSharedPtr<FJsonObject> Poll()
    {
        const auto Current = Identity(Bridge); Tools.Tick(Current);
        const auto Response = Tools.Execute(TEXT("functional_job"), Job, Current);
        return Response->GetBoolField(TEXT("ok")) ? Response->GetObjectField(TEXT("result")) : nullptr;
    }
};

class FExercise : public IAutomationLatentCommand
{
public:
    explicit FExercise(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        auto& S = *State;
        if (FPlatformTime::Seconds() - S.Begin > 45) { S.Test->AddError(TEXT("Jev functional fixture exceeded 45 seconds.")); S.Tools.Shutdown(); return true; }
        if (!GEditor->PlayWorld || !GEditor->PlayWorld->HasBegunPlay()) return false;
        if (S.Stage == 0)
        {
            const auto Current = Identity(S.Bridge);
            S.Test->TestEqual(TEXT("unapproved test denied"), ErrorCode(S.Tools.Execute(TEXT("functional_start"), Start(TEXT("unapproved"), Current), Current)), FString(TEXT("test_not_allowed")));
            auto Stale = Start(TEXT("pass"), Current); Stale->GetObjectField(TEXT("expected_state"))->SetStringField(TEXT("revision"), TEXT("stale"));
            S.Test->TestEqual(TEXT("stale inspection cannot launch test"), ErrorCode(S.Tools.Execute(TEXT("functional_start"), Stale, Current)), FString(TEXT("stale_plan")));
            if (!S.BeginJob(TEXT("pass"))) return true;
            S.Test->TestEqual(TEXT("concurrent job refused"), ErrorCode(S.Tools.Execute(TEXT("functional_start"), Start(TEXT("pass"), Current), Current)), FString(TEXT("job_busy")));
            ++S.Stage; return false;
        }
        const auto Result = S.Poll();
        if (!Result) { S.Test->AddError(TEXT("Functional fixture lost its job receipt.")); return true; }
        const FString Status = Result->GetStringField(TEXT("state"));
        if (S.Stage == 1 || S.Stage == 2 || S.Stage == 5 || S.Stage == 7 || S.Stage == 8)
        {
            if (Status == TEXT("queued") || Status == TEXT("running")) return false;
            const FString Expected = S.Stage == 1 ? TEXT("passed") : S.Stage == 2 || S.Stage == 7 ? TEXT("failed") : S.Stage == 8 ? TEXT("interrupted") : TEXT("timed_out");
            S.Test->TestEqual(TEXT("authoritative native result"), Status, Expected);
            S.Test->TestTrue(TEXT("owned cleanup attempted"), Result->GetBoolField(TEXT("cleanup_attempted")));
            if (S.Stage == 1)
            {
                S.Test->TestTrue(TEXT("real PIE probe moved to expected coordinates"), AJevFunctionalTestFixture::SuccessfulMovementCount > 0);
                S.Test->TestNotNull(TEXT("adapter leaves PIE running"), GEditor->PlayWorld.Get());
                if (!S.BeginJob(TEXT("fail"))) return true; S.Stage = 2;
            }
            else if (S.Stage == 2) { if (!S.BeginJob(TEXT("assert"))) return true; S.Stage = 7; }
            else if (S.Stage == 7)
            {
                S.Test->TestEqual(TEXT("succeeded result cannot hide failed assertion"), Result->GetStringField(TEXT("native_result")), FString(TEXT("Succeeded")));
                S.Test->TestTrue(TEXT("failed assertion is observed"), Result->GetNumberField(TEXT("observed_error_count")) > 0);
                S.Test->TestTrue(TEXT("live functional log observer captures native errors"), Result->GetArrayField(TEXT("observed_errors")).Num() > 0);
                if (!S.BeginJob(TEXT("cleanup"))) return true; S.Stage = 8;
            }
            else if (S.Stage == 8) { if (!S.BeginJob(TEXT("finish"))) return true; S.Stage = 9; }
            else
            {
                GConfig->SetDouble(Section, TEXT("MaxJobSeconds"), 30, GGameIni);
                if (!S.BeginJob(TEXT("hang"))) return true;
                S.Tools.Tick(Identity(S.Bridge));
                S.Stage = 6; return true; // EndPIE latent command follows; next command verifies interruption.
            }
            return false;
        }
        if (S.Stage == 3)
        {
            const auto Cancelled = S.Tools.Execute(TEXT("functional_cancel"), S.Job, Identity(S.Bridge))->GetObjectField(TEXT("result"));
            S.Test->TestEqual(TEXT("cancelled test cannot pass"), Cancelled->GetStringField(TEXT("state")), FString(TEXT("cancelled")));
            S.Test->TestTrue(TEXT("cancellation runs owned cleanup"), Cancelled->GetBoolField(TEXT("cleanup_attempted")));
            if (!S.BeginJob(TEXT("hang"))) return true; S.Stage = 4; return false;
        }
        if (S.Stage == 9)
        {
            const auto Cancelled = S.Tools.Execute(TEXT("functional_cancel"), S.Job, Identity(S.Bridge))->GetObjectField(TEXT("result"));
            S.Test->TestEqual(TEXT("FinishTest callback destruction interrupts receipt"), Cancelled->GetStringField(TEXT("state")), FString(TEXT("interrupted")));
            S.Test->TestFalse(TEXT("destroyed test is not called again for cleanup"), Cancelled->GetBoolField(TEXT("cleanup_attempted")));
            if (!S.BeginJob(TEXT("hang"))) return true; S.Stage = 10; return false;
        }
        if (S.Stage == 10)
        {
            auto* Running = FindObject<AJevFunctionalTestFixture>(nullptr, *Result->GetStringField(TEXT("pie_actor_path")));
            if (!S.Test->TestNotNull(TEXT("replacement fixture resolves owned actor"), Running)) return true;
            if (Running->RunFrame == GFrameNumber) return false;
            Running->FinishTest(EFunctionalTestResult::Succeeded, TEXT("Source fixture replaces the old run."));
            Running->CleanUp(); Running->RunTest({});
            const auto Interrupted = S.Tools.Execute(TEXT("functional_cancel"), S.Job, Identity(S.Bridge))->GetObjectField(TEXT("result"));
            S.Test->TestEqual(TEXT("old job cannot cancel replacement run on same actor"), Interrupted->GetStringField(TEXT("state")), FString(TEXT("interrupted")));
            S.Test->TestTrue(TEXT("replacement run remains running"), Running->IsRunning());
            S.Test->TestFalse(TEXT("old job does not clean replacement run"), Interrupted->GetBoolField(TEXT("cleanup_attempted")));
            Running->FinishTest(EFunctionalTestResult::Succeeded, TEXT("Source fixture ends its replacement run.")); Running->CleanUp();
            if (!S.BeginJob(TEXT("hang"))) return true; S.Stage = 3; return false;
        }
        if (S.Stage == 4)
        {
            GConfig->SetBool(Section, TEXT("bEnabled"), false, GGameIni); const auto Revoked = S.Poll();
            S.Test->TestEqual(TEXT("revoked policy interrupts running test"), Revoked->GetStringField(TEXT("state")), FString(TEXT("interrupted")));
            GConfig->SetBool(Section, TEXT("bEnabled"), true, GGameIni); GConfig->SetDouble(Section, TEXT("MaxJobSeconds"), 1, GGameIni);
            if (!S.BeginJob(TEXT("hang"))) return true; S.Stage = 5; return false;
        }
        return true;
    }
private:
    TSharedRef<FState> State;
};

class FAfterPIE : public IAutomationLatentCommand
{
public:
    explicit FAfterPIE(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        if (GEditor->PlayWorld && FPlatformTime::Seconds() - State->Begin < 60) return false;
        State->Test->TestNull(TEXT("automation owns and ends only its own PIE fixture"), GEditor->PlayWorld.Get());
        if (State->Stage == 6)
        {
            const auto Result = State->Poll();
            if (State->Test->TestTrue(TEXT("receipt survives PIE ending"), Result.IsValid())) State->Test->TestEqual(TEXT("PIE loss interrupts job"), Result->GetStringField(TEXT("state")), FString(TEXT("interrupted")));
        }
        State->Tools.Shutdown(); return true;
    }
private:
    TSharedRef<FState> State;
};
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FJevFunctionalJobs, FFunctionalTestBase, "Jev.Editor.FunctionalJobs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevFunctionalJobs::RunTest(const FString& Parameters)
{
    using namespace JevFunctionalTests;
    if (!TestNull(TEXT("fixture never interrupts an existing PIE session"), GEditor->PlayWorld.Get())) return false;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    auto State = MakeShared<FState>(this);
    AddExpectedErrorPlain(TEXT("Intentional negative movement fixture."), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedErrorPlain(TEXT("Intentional assertion before Succeeded fixture."), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedErrorPlain(TEXT("Intentional native functional log error fixture."), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedErrorPlain(TEXT("Jev stopped its owned functional test:"), EAutomationExpectedErrorFlags::Contains, 4);
    AddExpectedErrorPlain(TEXT("Test was aborted"), EAutomationExpectedErrorFlags::Contains, 1);
    AJevFunctionalTestFixture::CleanupCount = 0; AJevFunctionalTestFixture::SuccessfulMovementCount = 0;
    TArray<FString> Entries;
    const TArray<FString> Ids = {TEXT("pass"), TEXT("fail"), TEXT("hang"), TEXT("assert"), TEXT("cleanup"), TEXT("finish")};
    for (int32 I = 0; I < Ids.Num(); ++I)
    {
        auto* Actor = World->SpawnActor<AJevFunctionalTestFixture>(FVector(I * 200, 0, 0), FRotator::ZeroRotator);
        if (!TestNotNull(TEXT("source fixture actor spawned"), Actor)) return false;
        Actor->ConfigureMode(I);
        const FString Id = Ids[I];
        Actor->TestLabel = TEXT("Jev ") + Id;
        Entries.Add(Id + TEXT("|") + Actor->GetPathName());
    }
    GConfig->SetArray(Section, TEXT("Tests"), Entries, GGameIni);
    GConfig->SetBool(Section, TEXT("bEnabled"), false, GGameIni);
    auto Current = Identity(State->Bridge);
    TestEqual(TEXT("functional execution disabled by default policy"), ErrorCode(State->Tools.Execute(TEXT("functional_start"), Start(TEXT("pass"), Current), Current)), FString(TEXT("functional_disabled")));
    GConfig->SetBool(Section, TEXT("bEnabled"), true, GGameIni); GConfig->SetDouble(Section, TEXT("MaxJobSeconds"), 30, GGameIni);
    TestEqual(TEXT("tool does not start PIE implicitly"), ErrorCode(State->Tools.Execute(TEXT("functional_start"), Start(TEXT("pass"), Current), Current)), FString(TEXT("pie_required")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExercise>(State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FAfterPIE>(State));
    return true;
}

#endif
