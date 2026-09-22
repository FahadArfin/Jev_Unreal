#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;
class UWorld;
class UStaticMesh;
class UMaterialInterface;

/** Game-thread-only, bounded editor operations. No model-generated code is evaluated. */
class FJevEditorBridge
{
public:
    explicit FJevEditorBridge(TFunction<double()> InClock = {});
    TSharedRef<FJsonObject> Execute(const TSharedPtr<FJsonObject>& Request);
    static TSharedRef<FJsonObject> Error(const FString& Code, const FString& Message);
    static FString BoundedResponseBody(const TSharedRef<FJsonObject>& Response);
#if WITH_DEV_AUTOMATION_TESTS
    void FailApplyAfterOperationsForTesting(int32 Count) { FailureAfterOperationsForTesting = Count; }
#endif

private:
    struct FOperation
    {
        FString Op;
        FString Shape;
        FString Label;
        FString ActorPath;
        FString AssetPath;
        FString MaterialPath;
        FString Folder;
        FString TargetBaseline;
        int32 Slot = INDEX_NONE;
        bool bSetLabel = false;
        bool bSetFolder = false;
        TWeakObjectPtr<AActor> Target;
        TWeakObjectPtr<UStaticMesh> MeshAsset;
        TWeakObjectPtr<UMaterialInterface> MaterialAsset;
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
#if WITH_DEV_AUTOMATION_TESTS
    int32 FailureAfterOperationsForTesting = INDEX_NONE;
#endif
    FString Revision(UWorld* World) const;
    TSharedRef<FJsonObject> ActorSnapshot(AActor* Actor) const;
    FString ActorEditFingerprint(AActor* Actor) const;
    TArray<FString> ActorEditBlockers(AActor* Actor) const;
    TSharedRef<FJsonObject> StatusSnapshot(UWorld* World) const;
    TSharedRef<FJsonObject> Context(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> AssetDetails(const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> ActorDetails(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> Validate(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> Capture(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedRef<FJsonObject> Frame(UWorld* World, const TSharedPtr<FJsonObject>& Params) const;
    TSharedPtr<FJsonObject> ResolveStaticMeshAsset(const FString& Path, UStaticMesh*& OutMesh) const;
    TSharedPtr<FJsonObject> ResolveMaterialAsset(const FString& Path, UMaterialInterface*& OutMaterial) const;
    TSharedRef<FJsonObject> Preview(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    TSharedRef<FJsonObject> Apply(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    AActor* FindActor(UWorld* World, const FString& Path) const;
};
