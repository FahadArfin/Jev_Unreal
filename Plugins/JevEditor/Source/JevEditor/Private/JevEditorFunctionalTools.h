#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Named project-owned functional tests in one already-running standalone PIE world. */
class FJevFunctionalTools
{
public:
    FJevFunctionalTools();
    ~FJevFunctionalTools();
    static bool HandlesAction(const FString& Action);
    TSharedRef<FJsonObject> Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    bool HasActiveJob() const { return !ActiveJob.IsEmpty(); }
    void Tick(const TSharedRef<FJsonObject>& Identity);
    void Shutdown();
#if WITH_DEV_AUTOMATION_TESTS
    void PermitAutomationFixture() { bAutomationFixture = true; }
#endif
private:
    struct FJob;
    TMap<FString, TSharedPtr<FJob>> Jobs;
    FString ActiveJob;
    bool bExecutingCallbacks = false;
    bool bAutomationFixture = false;
    TSharedRef<FJsonObject> Snapshot(const FJob& Job) const;
    void Finish(FJob& Job, const FString& State, const FString& Reason, bool bMayCleanUp);
};
