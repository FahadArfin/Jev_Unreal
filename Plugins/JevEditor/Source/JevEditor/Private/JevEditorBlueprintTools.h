#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

/** Explicit, project-approved Blueprint compilation. All calls run on the game thread. */
class FJevBlueprintTools
{
public:
    explicit FJevBlueprintTools(TFunction<double()> InClock = {});
    ~FJevBlueprintTools();
    static bool HandlesAction(const FString& Action);
    static bool IsSupportedClass(const UClass* Class);
    TSharedRef<FJsonObject> Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    bool HasActiveJob() const { return bCompiling; }
    void Shutdown();

private:
    struct FPlan;
    TMap<FString, TSharedPtr<FPlan>> Plans;
    TFunction<double()> Clock;
    FDelegateHandle ModifiedHandle;
    FDelegateHandle PropertyHandle;
    uint64 ChangeEpoch = 0;
    bool bCompiling = false;
    void Prune();
    TSharedRef<FJsonObject> Targets(const TSharedRef<FJsonObject>& Identity) const;
    TSharedRef<FJsonObject> Preview(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    TSharedRef<FJsonObject> Compile(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
};
