#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Project inspection and explicitly configured validation. Called on the game thread only. */
class FJevProjectTools
{
public:
    FJevProjectTools();
    ~FJevProjectTools();
    static bool HandlesAction(const FString& Action);
    TSharedRef<FJsonObject> Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    void Tick(const TSharedRef<FJsonObject>& Identity);
    void Shutdown();
    bool HasActiveJob() const { return !ActiveJob.IsEmpty(); }

private:
    struct FJob;
    TMap<FString, TSharedPtr<FJob>> Jobs;
    FString ActiveJob;
    bool bExecutingCallbacks = false;
    TSharedRef<FJsonObject> ValidationRules(const TSharedRef<FJsonObject>& Identity) const;
    TSharedRef<FJsonObject> StartValidation(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    TSharedRef<FJsonObject> JobSnapshot(const FJob& Job) const;
    void Prune();
};
