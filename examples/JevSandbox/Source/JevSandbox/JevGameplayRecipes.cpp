#include "JevGameplayRecipes.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"

AJevRecipeDoor::AJevRecipeDoor()
{
    Panel = CreateDefaultSubobject<UBoxComponent>(TEXT("DoorPanel"));
    SetRootComponent(Panel);
    Panel->SetBoxExtent(FVector(5, 50, 100));
    Panel->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    Panel->SetCanEverAffectNavigation(false);
}

bool AJevRecipeDoor::SetOpen(bool bRequestedOpen)
{
    if (bRequestedOpen && (bLocked || bJammed)) return false;
    bOpen = bRequestedOpen;
    SetActorRotation(FRotator(0, bOpen ? 90 : 0, 0));
    Panel->SetCollisionEnabled(bOpen ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
    return true;
}

bool AJevRecipeDoor::BlocksPassage() const
{
    return Panel && Panel->GetCollisionEnabled() != ECollisionEnabled::NoCollision
        && Panel->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block;
}

AJevRecipeInteraction::AJevRecipeInteraction()
{
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("InteractionRoot")));
}

bool AJevRecipeInteraction::TryInteract(AActor* Interactor)
{
    if (!bEnabled || !IsValid(Interactor) || Interactor->GetWorld() != GetWorld()
        || InteractionCount != 0 || FVector::DistSquared(GetActorLocation(), Interactor->GetActorLocation()) > FMath::Square(200.0)) return false;
    ++InteractionCount;
    return true;
}

AJevRecipeCombatTarget::AJevRecipeCombatTarget()
{
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CombatRoot")));
    SetCanBeDamaged(true);
}

float AJevRecipeCombatTarget::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
    AController* EventInstigator, AActor* DamageCauser)
{
    if (!FMath::IsFinite(DamageAmount) || DamageAmount <= 0 || IsDead()) return 0;
    const float Accepted = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
    const float Applied = FMath::Min(Health, FMath::Max(0.0f, Accepted * DamageScale * (1 - ArmorFraction)));
    Health -= Applied;
    return Applied;
}

AJevGameplayRecipe::AJevGameplayRecipe()
{
    TimeLimit = 8;
    TimesUpMessage = FText::FromString(TEXT("Project gameplay recipe did not complete within eight seconds."));
    LogErrorHandling = EFunctionalTestLogHandling::OutputIgnored;
    LogWarningHandling = EFunctionalTestLogHandling::OutputIgnored;
}

void AJevGameplayRecipe::StartTest()
{
    TeardownOwnedActors();
    ObservationCount = 0;
    LastObservation.Reset();
    Super::StartTest();
    if (!GetWorld() || GetWorld()->WorldType != EWorldType::PIE)
    {
        FinishTest(EFunctionalTestResult::Error, TEXT("Jev recipe requires its isolated PIE world."));
        return;
    }
    RunScenario();
}

AActor* AJevGameplayRecipe::SpawnOwned(UClass* ActorClass, const FVector& Location)
{
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.ObjectFlags |= RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor = GetWorld()->SpawnActor(ActorClass, &Location, &FRotator::ZeroRotator, Params);
    if (Actor) OwnedActors.Add(Actor);
    else FinishTest(EFunctionalTestResult::Error, TEXT("Jev recipe could not spawn an owned subject."));
    return Actor;
}

bool AJevGameplayRecipe::Observe(bool bCondition, const TCHAR* Evidence)
{
    LastObservation = Evidence;
    ++ObservationCount;
    if (!bCondition) FinishTest(EFunctionalTestResult::Failed, FString(TEXT("Jev recipe failed: ")) + Evidence);
    return bCondition;
}

void AJevGameplayRecipe::Succeed(const TCHAR* Evidence)
{
    LastObservation = Evidence;
    FinishTest(EFunctionalTestResult::Succeeded, Evidence);
}

void AJevGameplayRecipe::TeardownOwnedActors()
{
    for (AActor* Actor : OwnedActors)
    {
        // Never destroy a subject that project code transferred to another owner.
        if (IsValid(Actor) && Actor->GetOwner() == this && Actor->GetWorld() == GetWorld()) Actor->Destroy();
    }
    OwnedActors.Reset();
}

int32 AJevGameplayRecipe::GetOwnedActorCount() const
{
    int32 Count = 0;
    for (AActor* Actor : OwnedActors) if (IsValid(Actor)) ++Count;
    return Count;
}

void AJevGameplayRecipe::CleanUp()
{
    TeardownOwnedActors();
    ++CleanupCount;
    Super::CleanUp();
}

void AJevGameplayRecipe::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    TeardownOwnedActors();
    Super::EndPlay(EndPlayReason);
}

AJevDoorRecipe::AJevDoorRecipe() { TestLabel = TEXT("Project recipe: door lock, opening and closing"); }
void AJevDoorRecipe::RunScenario()
{
    auto* Door = Cast<AJevRecipeDoor>(SpawnOwned(AJevRecipeDoor::StaticClass(), GetActorLocation()));
    if (!Door) return;
    Door->bJammed = bJammedDoor;
    if (!Observe(!Door->SetOpen(true) && !Door->IsOpen() && Door->BlocksPassage(), TEXT("locked door refuses opening and still blocks passage"))) return;
    Door->bLocked = false;
    if (!Observe(Door->SetOpen(true) && Door->IsOpen() && !Door->BlocksPassage()
        && FMath::IsNearlyEqual(Door->GetActorRotation().Yaw, 90.0), TEXT("unlocked door opens physically and clears blocking collision"))) return;
    if (!Observe(Door->SetOpen(false) && !Door->IsOpen() && Door->BlocksPassage()
        && FMath::IsNearlyZero(Door->GetActorRotation().Yaw), TEXT("closing restores orientation and blocking collision"))) return;
    Succeed(TEXT("Door recipe observed locked denial, open orientation/collision, and closed restoration."));
}

AJevInteractionRecipe::AJevInteractionRecipe() { TestLabel = TEXT("Project recipe: interaction range and single use"); }
void AJevInteractionRecipe::RunScenario()
{
    auto* Target = Cast<AJevRecipeInteraction>(SpawnOwned(AJevRecipeInteraction::StaticClass(), GetActorLocation()));
    auto* User = SpawnOwned(AJevRecipeInteraction::StaticClass(), GetActorLocation() + FVector(500, 0, 0));
    if (!Target || !User) return;
    Target->bEnabled = !bDisableInteraction;
    IJevRecipeInteractable* Contract = Cast<IJevRecipeInteractable>(Target);
    if (!Observe(Contract && !Contract->TryInteract(User) && Target->InteractionCount == 0, TEXT("out-of-range interaction has no side effect"))) return;
    User->SetActorLocation(Target->GetActorLocation() + FVector(100, 0, 0));
    if (!Observe(Contract->TryInteract(User) && Target->InteractionCount == 1, TEXT("in-range interaction consumes the target exactly once"))) return;
    if (!Observe(!Contract->TryInteract(User) && Target->InteractionCount == 1, TEXT("consumed interaction refuses duplicate activation"))) return;
    Succeed(TEXT("Interaction recipe observed range rejection, one accepted use, and duplicate rejection."));
}

AJevCombatRecipe::AJevCombatRecipe() { TestLabel = TEXT("Project recipe: damage, armor and death"); }
void AJevCombatRecipe::RunScenario()
{
    auto* Target = Cast<AJevRecipeCombatTarget>(SpawnOwned(AJevRecipeCombatTarget::StaticClass(), GetActorLocation()));
    if (!Target) return;
    Target->DamageScale = DamageScale;
    const float Applied = UGameplayStatics::ApplyDamage(Target, 40, nullptr, this, nullptr);
    if (!Observe(FMath::IsNearlyEqual(Applied, 30.0f) && FMath::IsNearlyEqual(Target->Health, 70.0f)
        && !Target->IsDead(), TEXT("40 damage with 25 percent armor removes 30 health through ApplyDamage"))) return;
    const float Overkill = UGameplayStatics::ApplyDamage(Target, 1000, nullptr, this, nullptr);
    if (!Observe(FMath::IsNearlyEqual(Overkill, 70.0f) && Target->Health == 0 && Target->IsDead(), TEXT("overkill clamps health to zero and enters the dead state"))) return;
    if (!Observe(UGameplayStatics::ApplyDamage(Target, 20, nullptr, this, nullptr) == 0 && Target->Health == 0,
        TEXT("a dead target cannot take further damage"))) return;
    Succeed(TEXT("Combat recipe observed armor-adjusted damage, clamped lethal damage, and dead-state rejection."));
}

AJevNavigationRecipe::AJevNavigationRecipe() { TestLabel = TEXT("Project recipe: complete navigation path"); }
void AJevNavigationRecipe::RunScenario()
{
    ObservedPathLength = 0;
    UNavigationSystemV1* Navigation = UNavigationSystemV1::GetNavigationSystem(GetWorld());
    if (!Navigation || !Navigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate))
    {
        FinishTest(EFunctionalTestResult::Error, TEXT("Jev navigation recipe requires existing built navigation data; no navmesh was created."));
        return;
    }
    if (!FMath::IsFinite(EndpointTolerance) || EndpointTolerance < 1 || EndpointTolerance > 100
        || DestinationOffset.ContainsNaN() || DestinationOffset.SizeSquared() < 100
        || DestinationOffset.SizeSquared() > FMath::Square(100000.0))
    {
        FinishTest(EFunctionalTestResult::Error, TEXT("Jev navigation recipe has invalid endpoints or tolerance."));
        return;
    }
    FNavLocation Start, End;
    const FVector ProjectionExtent(EndpointTolerance, EndpointTolerance, EndpointTolerance);
    if (!Observe(Navigation->ProjectPointToNavigation(GetActorLocation(), Start, ProjectionExtent)
        && Navigation->ProjectPointToNavigation(GetActorLocation() + DestinationOffset, End, ProjectionExtent),
        TEXT("both configured navigation endpoints project onto existing navigation data"))) return;
    UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(), Start.Location, End.Location);
    if (!Observe(IsValid(Path) && Path->IsValid() && !Path->IsPartial() && Path->PathPoints.Num() >= 2,
        TEXT("navigation returns a valid complete path instead of a partial route"))) return;
    ObservedPathLength = Path->GetPathLength();
    if (!Observe(FVector::Dist(Path->PathPoints[0], Start.Location) <= EndpointTolerance
        && FVector::Dist(Path->PathPoints.Last(), End.Location) <= EndpointTolerance
        && FMath::IsFinite(ObservedPathLength) && ObservedPathLength > 0,
        TEXT("complete path connects both projected endpoints with positive finite length"))) return;
    Succeed(TEXT("Navigation recipe observed a complete route between the configured projected endpoints."));
}
