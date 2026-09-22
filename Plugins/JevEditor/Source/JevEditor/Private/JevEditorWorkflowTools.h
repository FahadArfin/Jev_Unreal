#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Bounded native domain workflows. No names from a request are executed as code. */
class FJevWorkflowTools
{
public:
    explicit FJevWorkflowTools(TFunction<double()> InClock = {});
    ~FJevWorkflowTools();
    static bool HandlesAction(const FString& Action);
    TSharedRef<FJsonObject> Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    void Tick(const TSharedRef<FJsonObject>& Identity, double DeltaSeconds);
    bool HasActiveJob() const;
private:
    struct FPlan;
    struct FSample;
    TMap<FString, TSharedPtr<FPlan>> Plans;
    TMap<FString, TSharedPtr<FSample>> Samples;
    TFunction<double()> Clock;
    FDelegateHandle ModifiedHandle, PropertyHandle;
    uint64 ChangeEpoch = 0;
    bool bApplying = false;
    TSharedRef<FJsonObject> Inspect(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    TSharedRef<FJsonObject> Preview(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    TSharedRef<FJsonObject> Apply(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
    TSharedRef<FJsonObject> Performance(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity);
};

namespace JevWorkflow
{
bool Only(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& Keys);
bool Text(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FString& Value, int32 Max = 1024);
bool Number(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, double& Value, double Min, double Max);
bool Vector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FVector& Value, double Bound = 10000000);
TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value);
TSharedRef<FJsonObject> Base(const TSharedRef<FJsonObject>& Identity);
TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result);
bool Same(const TSharedPtr<FJsonObject>& Before, const TSharedRef<FJsonObject>& Now);
bool Editing(const TSharedRef<FJsonObject>& Identity);
UObject* Loaded(const FString& Path);
TSharedRef<FJsonObject> Material(UObject* Object);
TSharedRef<FJsonObject> Light(UObject* Object);
TSharedRef<FJsonObject> Camera();
TSharedRef<FJsonObject> AssetDiagnosis(const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> Rig(const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> Widgets(const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> Surface(const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> Navigation(const TSharedPtr<FJsonObject>& Params);
}
