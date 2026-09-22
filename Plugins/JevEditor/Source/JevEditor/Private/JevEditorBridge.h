#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;
class UWorld;

/** Game-thread-only, bounded editor operations. No model-generated code is evaluated. */
class FJevEditorBridge
{
public:
    explicit FJevEditorBridge(TFunction<double()> InClock = {});
    TSharedRef<FJsonObject> Execute(const TSharedPtr<FJsonObject>& Request);
    static TSharedRef<FJsonObject> Error(const FString& Code, const FString& Message);

private:
    struct FOperation
    {
        FString Op;
        FString Shape;
        FString Label;
        FString ActorPath;
        TWeakObjectPtr<AActor> Target;
        FTransform Transform = FTransform::Identity;
    };
    struct FPlan
    {
        FString Project;
        FString World;
        TWeakObjectPtr<UWorld> WorldInstance;
        TArray<TWeakObjectPtr<AActor>> SceneActors;
        FString Revision;
        double ExpiresAt = 0;
        TArray<FOperation> Operations;
    };

    FString SessionId;
    TFunction<double()> Clock;
    TMap<FString, FPlan> Plans;
    FString Revision(UWorld* World) const;
    TSharedRef<FJsonObject> ActorSnapshot(AActor* Actor) const;
    TSharedRef<FJsonObject> Preview(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    TSharedRef<FJsonObject> Apply(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    AActor* FindActor(UWorld* World, const FString& Path) const;
};
