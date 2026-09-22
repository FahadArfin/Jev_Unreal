#pragma once

#include "CoreMinimal.h"
#include "FunctionalTest.h"
#include "UObject/Interface.h"
#include "JevGameplayRecipes.generated.h"

class UBoxComponent;

/** Example project contract. Replace this with the interaction interface your game owns. */
UINTERFACE(MinimalAPI)
class UJevRecipeInteractable : public UInterface
{
    GENERATED_BODY()
};

class IJevRecipeInteractable
{
    GENERATED_BODY()
public:
    virtual bool TryInteract(AActor* Interactor) = 0;
};

/** Small real subject: opening changes both orientation and blocking collision. */
UCLASS(NotBlueprintable)
class AJevRecipeDoor : public AActor
{
    GENERATED_BODY()
public:
    AJevRecipeDoor();
    bool SetOpen(bool bOpen);
    bool IsOpen() const { return bOpen; }
    bool BlocksPassage() const;
    bool bLocked = true;
    bool bJammed = false;
private:
    UPROPERTY() TObjectPtr<UBoxComponent> Panel;
    bool bOpen = false;
};

/** One-use interaction with distance and consumed-state checks. */
UCLASS(NotBlueprintable)
class AJevRecipeInteraction : public AActor, public IJevRecipeInteractable
{
    GENERATED_BODY()
public:
    AJevRecipeInteraction();
    virtual bool TryInteract(AActor* Interactor) override;
    bool bEnabled = true;
    int32 InteractionCount = 0;
};

/** Damage is applied through Unreal's TakeDamage route, including armor and death. */
UCLASS(NotBlueprintable)
class AJevRecipeCombatTarget : public AActor
{
    GENERATED_BODY()
public:
    AJevRecipeCombatTarget();
    virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
        AController* EventInstigator, AActor* DamageCauser) override;
    float Health = 100;
    float ArmorFraction = 0.25f;
    float DamageScale = 1;
    bool IsDead() const { return Health <= 0; }
};

/** All owned actors are transient and are torn down on cleanup, restart, or PIE ending. */
UCLASS(Abstract, NotBlueprintable)
class AJevGameplayRecipe : public AFunctionalTest
{
    GENERATED_BODY()
public:
    AJevGameplayRecipe();
    virtual void CleanUp() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    UPROPERTY(VisibleInstanceOnly, Transient, Category="Recipe evidence") int32 CleanupCount = 0;
    UPROPERTY(VisibleInstanceOnly, Transient, Category="Recipe evidence") int32 ObservationCount = 0;
    UPROPERTY(VisibleInstanceOnly, Transient, Category="Recipe evidence") FString LastObservation;
    int32 GetOwnedActorCount() const;
protected:
    virtual void StartTest() override;
    virtual void RunScenario() PURE_VIRTUAL(AJevGameplayRecipe::RunScenario, );
    AActor* SpawnOwned(UClass* ActorClass, const FVector& Location);
    bool Observe(bool bCondition, const TCHAR* Evidence);
    void Succeed(const TCHAR* Evidence);
private:
    void TeardownOwnedActors();
    UPROPERTY(Transient) TArray<TObjectPtr<AActor>> OwnedActors;
};

UCLASS(NotBlueprintable)
class AJevDoorRecipe : public AJevGameplayRecipe
{
    GENERATED_BODY()
public:
    AJevDoorRecipe();
    /** Fault injection lets automation prove that a broken door is detected. */
    UPROPERTY(EditInstanceOnly, Category="Recipe regression") bool bJammedDoor = false;
protected:
    virtual void RunScenario() override;
};

UCLASS(NotBlueprintable)
class AJevInteractionRecipe : public AJevGameplayRecipe
{
    GENERATED_BODY()
public:
    AJevInteractionRecipe();
    UPROPERTY(EditInstanceOnly, Category="Recipe regression") bool bDisableInteraction = false;
protected:
    virtual void RunScenario() override;
};

UCLASS(NotBlueprintable)
class AJevCombatRecipe : public AJevGameplayRecipe
{
    GENERATED_BODY()
public:
    AJevCombatRecipe();
    UPROPERTY(EditInstanceOnly, Category="Recipe regression", meta=(ClampMin="0", ClampMax="2")) float DamageScale = 1;
protected:
    virtual void RunScenario() override;
};

/** Queries existing navigation only; never builds a navmesh or moves a user's pawn. */
UCLASS(NotBlueprintable)
class AJevNavigationRecipe : public AJevGameplayRecipe
{
    GENERATED_BODY()
public:
    AJevNavigationRecipe();
    UPROPERTY(EditInstanceOnly, Category="Recipe navigation") FVector DestinationOffset = FVector(400, 0, 0);
    UPROPERTY(EditInstanceOnly, Category="Recipe navigation", meta=(ClampMin="1", ClampMax="100")) float EndpointTolerance = 50;
    UPROPERTY(VisibleInstanceOnly, Transient, Category="Recipe evidence") double ObservedPathLength = 0;
protected:
    virtual void RunScenario() override;
};
