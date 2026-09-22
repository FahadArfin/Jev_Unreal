#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;
class UWorld;
class UStaticMesh;

/** Game-thread-only, bounded editor operations. No model-generated code is evaluated. */
class FJevEditorBridge
{
public:
    explicit FJevEditorBridge(TFunction<double()> InClock = {});
    TSharedRef<FJsonObject> Execute(const TSharedPtr<FJsonObject>& Request);
    static TSharedRef<FJsonObject> Error(const FString& Code, const FString& Message);
    static FString BoundedResponseBody(const TSharedRef<FJsonObject>& Response);

private:
    struct FOperation
    {
        FString Op;
        FString Shape;
        FString Label;
        FString ActorPath;
        FString AssetPath;
        TWeakObjectPtr<AActor> Target;
        TWeakObjectPtr<UStaticMesh> MeshAsset;
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
    TSharedRef<FJsonObject> StatusSnapshot(UWorld* World) const;
    TSharedRef<FJsonObject> Context(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> AssetDetails(const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> Validate(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> Capture(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> Frame(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedPtr<FJsonObject> ResolveStaticMeshAsset(const FString& Path, UStaticMesh*& OutMesh) const;
    TSharedRef<FJsonObject> Preview(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    TSharedRef<FJsonObject> Apply(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    AActor* FindActor(UWorld* World, const FString& Path) const;
};
