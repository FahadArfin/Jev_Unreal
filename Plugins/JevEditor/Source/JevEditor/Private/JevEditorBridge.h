#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;
class UWorld;
class UStaticMesh;
class UMaterialInterface;
class AStaticMeshActor;
class UStaticMeshComponent;

/** Game-thread-only, bounded editor operations. No model-generated code is evaluated. */
class FJevEditorBridge
{
public:
    explicit FJevEditorBridge(TFunction<double()> InClock = {});
    ~FJevEditorBridge();
    FJevEditorBridge(const FJevEditorBridge&) = delete;
    FJevEditorBridge& operator=(const FJevEditorBridge&) = delete;
    TSharedRef<FJsonObject> Execute(const TSharedPtr<FJsonObject>& Request);
    static TSharedRef<FJsonObject> Error(const FString& Code, const FString& Message);
    static FString BoundedResponseBody(const TSharedRef<FJsonObject>& Response);
#if WITH_DEV_AUTOMATION_TESTS
    void FailApplyAfterOperationsForTesting(int32 Count) { FailureAfterOperationsForTesting = Count; }
    void OnApplyConsumedForTesting(TFunction<void()> Callback) { ApplyConsumedCallbackForTesting = MoveTemp(Callback); }
    void OnMeshOperationForTesting(TFunction<void()> Callback) { MeshOperationCallbackForTesting = MoveTemp(Callback); }
#endif

private:
    struct FMeshSettings
    {
        uint8 Mobility = 0;
        uint8 CollisionMode = 0;
        uint8 CollisionObjectType = 0;
        FName CollisionProfile;
        bool bUseMeshDefaultCollision = false;
        TArray<uint8> CollisionResponses;
        bool bActorCollisionEnabled = true;
        bool bCastShadow = true;
        bool bVisible = true;
        bool bHiddenInGame = false;
        bool bActorHiddenInGame = false;
        bool bActorHiddenInEditor = false;
        TArray<FName> ActorTags;
        TArray<FName> ComponentTags;
    };
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
        FString MaterialPolicy;
        FString MeshBaseline;
        TArray<FString> MaterialBaselines;
        TArray<TWeakObjectPtr<UMaterialInterface>> MeshMaterials;
        TArray<TWeakObjectPtr<UMaterialInterface>> MeshOverrides;
        FMeshSettings MeshSettings;
        int32 Slot = INDEX_NONE;
        bool bSetLabel = false;
        bool bSetFolder = false;
        TWeakObjectPtr<AActor> Target;
        TWeakObjectPtr<UStaticMeshComponent> SourceComponent;
        TWeakObjectPtr<UStaticMesh> MeshAsset;
        TWeakObjectPtr<UMaterialInterface> MaterialAsset;
        FTransform Transform = FTransform::Identity;
        FVector SourcePivotOffset = FVector::ZeroVector;
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
    struct FPlanRecord
    {
        TSharedPtr<FJsonObject> Review;
        FString Status = TEXT("pending");
        FString OutcomeCode;
        FString RevisionAfter;
        TArray<FString> ActorPaths;
        double CreatedAt = 0;
        double UpdatedAt = 0;
        double ExpiresAt = 0;
    };

    FString SessionId;
    TFunction<double()> Clock;
    TMap<FString, FPlan> Plans;
    TMap<FString, FPlanRecord> PlanRecords;
    TArray<FString> PlanRecordOrder;
    bool bApplyingPlan = false;
    uint64 AssetChangeEpoch = 0;
    FDelegateHandle AssetChangeHandle;
#if WITH_DEV_AUTOMATION_TESTS
    int32 FailureAfterOperationsForTesting = INDEX_NONE;
    TFunction<void()> ApplyConsumedCallbackForTesting;
    TFunction<void()> MeshOperationCallbackForTesting;
#endif
    FString Revision(UWorld* World) const;
    TSharedRef<FJsonObject> ActorSnapshot(AActor* Actor) const;
    FString ActorEditFingerprint(AActor* Actor) const;
    FString MeshAssetFingerprint(UStaticMesh* Mesh) const;
    FString MaterialAssetFingerprint(UMaterialInterface* Material) const;
    FMeshSettings CaptureMeshSettings(AStaticMeshActor* Actor) const;
    TSharedRef<FJsonObject> MeshSettingsSnapshot(const FMeshSettings& Settings) const;
    TSharedRef<FJsonObject> MeshAssetSnapshot(UStaticMesh* Mesh) const;
    bool SetMeshSettings(AStaticMeshActor* Actor, const FMeshSettings& Settings) const;
    bool SupportsMeshOperation(AStaticMeshActor* Actor, bool bDuplicate, FString* UnsupportedProperty = nullptr) const;
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
    TSharedRef<FJsonObject> ApplyTracked(UWorld* World, const TSharedPtr<FJsonObject>& Params);
    TSharedRef<FJsonObject> PlanStatus(const TSharedPtr<FJsonObject>& Params);
    TSharedRef<FJsonObject> PendingPlans(const TSharedPtr<FJsonObject>& Params);
    TSharedRef<FJsonObject> PlanRecordSnapshot(const FString& PlanId, const FPlanRecord& Record) const;
    void PrunePlanRecords();
    AActor* FindActor(UWorld* World, const FString& Path) const;
};
