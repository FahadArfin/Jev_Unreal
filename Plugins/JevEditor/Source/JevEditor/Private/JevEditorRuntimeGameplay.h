#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Reviewed lifecycle of one project-approved standalone PIE session. No console commands. */
class FJevRuntimeGameplay
{
public:
    FJevRuntimeGameplay();
    ~FJevRuntimeGameplay();
    static bool HandlesAction(const FString& Action);
    bool HasActiveJob() const;
    TSharedRef<FJsonObject> Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    void Tick(const TSharedRef<FJsonObject>& Identity);
    void Shutdown();
private:
    struct FState;
    TUniquePtr<FState> State;
};
