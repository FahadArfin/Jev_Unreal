#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Fixed, project-approved FBX imports. Requests select aliases, never filesystem paths. */
class FJevHandoffTools
{
public:
    explicit FJevHandoffTools(TFunction<double()> InClock = {});
    ~FJevHandoffTools();
    static bool HandlesAction(const FString& Action);
    static FString SourceSha256(const TArray<uint8>& Bytes);
    TSharedRef<FJsonObject> Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    bool HasActiveJob() const { return bImporting; }
    void Shutdown();
private:
    struct FPlan;
    TMap<FString, TSharedPtr<FPlan>> Plans;
    TFunction<double()> Clock;
    FDelegateHandle ModifiedHandle, PropertyHandle;
    uint64 Epoch = 0;
    bool bImporting = false;
};
