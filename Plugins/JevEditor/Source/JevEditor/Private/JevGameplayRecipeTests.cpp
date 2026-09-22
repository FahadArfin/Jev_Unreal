#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "JevEditorFunctionalTools.h"
#include "ActorFactories/ActorFactory.h"
#include "AI/NavigationSystemBase.h"
#include "Builders/CubeBuilder.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FunctionalTest.h"
#include "FunctionalTestBase.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"

namespace JevGameplayRecipeTests
{
const TCHAR* Section = TEXT("JevEditor.FunctionalTesting");

TSharedRef<FJsonObject> Identity(FJevEditorBridge& Bridge)
{
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), TEXT("status"));
    Request->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
    return Bridge.Execute(Request)->GetObjectField(TEXT("result")).ToSharedRef();
}

struct FState
{
    FAutomationTestBase* Test;
    FJevEditorBridge Bridge;
    FJevFunctionalTools Tools;
    TArray<FString> Ids = {TEXT("door-pass"), TEXT("door-fail"), TEXT("interaction-pass"), TEXT("interaction-fail"),
        TEXT("combat-pass"), TEXT("combat-fail"), TEXT("navigation-pass"), TEXT("navigation-fail"), TEXT("door-pass")};
    TArray<FString> OldTests;
    TSharedPtr<FJsonObject> Job;
    bool bHadEnabled = false, bEnabled = false, bHadTimeout = false;
    double Timeout = 30, Begin = FPlatformTime::Seconds();
    EPlayNetMode OldMode = PIE_Standalone;
    int32 OldClients = 1, Index = 0;
    bool bOldProcess = true, bOldServer = false, bNavigationReady = false, bNavigationBuildStarted = false;
    TWeakObjectPtr<AActor> Sentinel;

    explicit FState(FAutomationTestBase* InTest) : Test(InTest)
    {
        Tools.PermitAutomationFixture();
        bHadEnabled = GConfig->GetBool(Section, TEXT("bEnabled"), bEnabled, GGameIni);
        bHadTimeout = GConfig->GetDouble(Section, TEXT("MaxJobSeconds"), Timeout, GGameIni);
        GConfig->GetArray(Section, TEXT("Tests"), OldTests, GGameIni);
        auto* Settings = GetMutableDefault<ULevelEditorPlaySettings>();
        Settings->GetPlayNetMode(OldMode); Settings->GetPlayNumberOfClients(OldClients);
        Settings->GetRunUnderOneProcess(bOldProcess); bOldServer = Settings->bLaunchSeparateServer;
        Settings->SetPlayNetMode(PIE_Standalone); Settings->SetPlayNumberOfClients(1);
        Settings->SetRunUnderOneProcess(true); Settings->bLaunchSeparateServer = false;
    }
    ~FState()
    {
        Tools.Shutdown();
        if (bHadEnabled) GConfig->SetBool(Section, TEXT("bEnabled"), bEnabled, GGameIni);
        else GConfig->RemoveKey(Section, TEXT("bEnabled"), GGameIni);
        if (bHadTimeout) GConfig->SetDouble(Section, TEXT("MaxJobSeconds"), Timeout, GGameIni);
        else GConfig->RemoveKey(Section, TEXT("MaxJobSeconds"), GGameIni);
        GConfig->SetArray(Section, TEXT("Tests"), OldTests, GGameIni);
        auto* Settings = GetMutableDefault<ULevelEditorPlaySettings>();
        Settings->SetPlayNetMode(OldMode); Settings->SetPlayNumberOfClients(OldClients);
        Settings->SetRunUnderOneProcess(bOldProcess); Settings->bLaunchSeparateServer = bOldServer;
    }
};

class FWaitForNavigation : public IAutomationLatentCommand
{
public:
    explicit FWaitForNavigation(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        auto* Navigation = UNavigationSystemV1::GetNavigationSystem(World);
        // New editor worlds initially hold AsyncLoadLock until asset compilation
        // has completed and the native delayed unlock has ticked. Do not remove
        // engine-owned locks: wait, then request our explicit fixture build once.
        if (Navigation && !State->bNavigationBuildStarted
            && !Navigation->IsNavigationBuildingLocked(static_cast<uint8>(~ENavigationBuildLock::NoUpdateInEditor)))
        {
            State->bNavigationBuildStarted = true;
            Navigation->Build();
        }
        if (Navigation && State->bNavigationBuildStarted && !Navigation->IsNavigationBuildInProgress())
        {
            FNavLocation Start, End;
            if (Navigation->ProjectPointToNavigation(FVector(0, 0, 25), Start, FVector(50))
                && Navigation->ProjectPointToNavigation(FVector(400, 0, 25), End, FVector(50)))
            {
                auto* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Start.Location, End.Location);
                if (Path && Path->IsValid() && !Path->IsPartial())
                {
                    State->bNavigationReady = true;
                    return true;
                }
            }
        }
        if (FPlatformTime::Seconds() - State->Begin < 60) return false;
        State->Test->AddError(FString::Printf(TEXT("Source-only recipe fixture did not build a usable Recast navigation path in 60 seconds (registered bounds: %d; nav data: %s)."),
            Navigation ? Navigation->GetNavigationBounds().Num() : 0,
            Navigation && Navigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) ? TEXT("present") : TEXT("absent")));
        return true;
    }
private:
    TSharedRef<FState> State;
};

class FExerciseRecipes : public IAutomationLatentCommand
{
public:
    explicit FExerciseRecipes(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        auto& S = *State;
        if (!S.bNavigationReady) return true;
        if (FPlatformTime::Seconds() - S.Begin > 100)
        {
            S.Test->AddError(TEXT("Gameplay recipe bridge fixture exceeded 100 seconds."));
            S.Tools.Shutdown(); return true;
        }
        if (!GEditor->PlayWorld || !GEditor->PlayWorld->HasBegunPlay()) return false;
        auto Current = Identity(S.Bridge);
        if (!S.Job)
        {
            if (S.Index == S.Ids.Num()) return true;
            auto Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("test_id"), S.Ids[S.Index]);
            auto Expected = MakeShared<FJsonObject>();
            for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
                Expected->SetStringField(Key, Current->GetStringField(Key));
            Params->SetObjectField(TEXT("expected_state"), Expected);
            const auto Response = S.Tools.Execute(TEXT("functional_start"), Params, Current);
            if (!S.Test->TestTrue(TEXT("exact project recipe alias starts"), Response->GetBoolField(TEXT("ok")))) return true;
            S.Job = MakeShared<FJsonObject>();
            S.Job->SetStringField(TEXT("job_id"), Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("job_id")));
            return false;
        }
        S.Tools.Tick(Current);
        const auto Response = S.Tools.Execute(TEXT("functional_job"), S.Job.ToSharedRef(), Current);
        if (!S.Test->TestTrue(TEXT("recipe receipt remains available"), Response->GetBoolField(TEXT("ok")))) return true;
        const auto Result = Response->GetObjectField(TEXT("result"));
        const FString Status = Result->GetStringField(TEXT("state"));
        if (Status == TEXT("queued") || Status == TEXT("running")) return false;
        const bool bNegative = S.Ids[S.Index].EndsWith(TEXT("-fail"));
        S.Test->TestEqual(TEXT("real recipe outcome matches the injected gameplay regression"), Status,
            FString(bNegative ? TEXT("failed") : TEXT("passed")));
        S.Test->TestTrue(TEXT("recipe cleanup callback returned"), Result->GetBoolField(TEXT("cleanup_attempted")));
        auto* Recipe = FindObject<AFunctionalTest>(nullptr, *Result->GetStringField(TEXT("pie_actor_path")));
        if (S.Test->TestNotNull(TEXT("original PIE recipe survives cleanup"), Recipe))
        {
            auto* Observations = FindFProperty<FIntProperty>(Recipe->GetClass(), TEXT("ObservationCount"));
            auto* Cleanups = FindFProperty<FIntProperty>(Recipe->GetClass(), TEXT("CleanupCount"));
            S.Test->TestTrue(TEXT("recipe evaluated actual gameplay observations"), Observations
                && Observations->GetPropertyValue_InContainer(Recipe) >= (bNegative ? 1 : 3));
            S.Test->TestTrue(TEXT("recipe cleanup called exactly once per run"), Cleanups
                && Cleanups->GetPropertyValue_InContainer(Recipe) == (S.Index == 8 ? 2 : 1));
            int32 OwnedAlive = 0;
            for (TActorIterator<AActor> It(GEditor->PlayWorld); It; ++It)
                if (IsValid(*It) && It->GetOwner() == Recipe) ++OwnedAlive;
            S.Test->TestEqual(TEXT("owned runtime subjects are gone after cleanup"), OwnedAlive, 0);
        }
        S.Test->TestTrue(TEXT("recipe cleanup leaves unrelated editor actors alone"), S.Sentinel.IsValid());
        ++S.Index; S.Job.Reset(); return false;
    }
private:
    TSharedRef<FState> State;
};

class FAfterRecipes : public IAutomationLatentCommand
{
public:
    explicit FAfterRecipes(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        // FEndPlayMapCommand requests teardown; it does not wait for the world
        // reference to disappear. Give the editor its own ticks to finish it.
        if (ShutdownStarted < 0) ShutdownStarted = FPlatformTime::Seconds();
        if (GEditor->PlayWorld && FPlatformTime::Seconds() - ShutdownStarted < 15) return false;
        State->Test->TestNull(TEXT("recipe automation ended only its own PIE session"), GEditor->PlayWorld.Get());
        State->Test->TestEqual(TEXT("all four positive, four negative, and repeated cleanup cases executed"), State->Index, State->Ids.Num());
        State->Tools.Shutdown(); return true;
    }
private:
    TSharedRef<FState> State;
    double ShutdownStarted = -1;
};
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FJevGameplayRecipes, FFunctionalTestBase,
    "Jev.Editor.GameplayRecipes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevGameplayRecipes::RunTest(const FString& Parameters)
{
    using namespace JevGameplayRecipeTests;
    TArray<UClass*> Classes;
    for (const TCHAR* Name : {TEXT("JevDoorRecipe"), TEXT("JevInteractionRecipe"), TEXT("JevCombatRecipe"), TEXT("JevNavigationRecipe")})
    {
        UClass* Class = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/JevSandbox.")) + Name));
        if (!Class)
        {
            AddWarning(TEXT("Project-owned recipe integration requires the JevSandbox source module; no recipe acceptance was run."));
            return true;
        }
        Classes.Add(Class);
    }
    if (!TestNull(TEXT("recipe fixture never interrupts an existing PIE session"), GEditor->PlayWorld.Get())) return false;
    auto State = MakeShared<FState>(this);
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -25), FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("unsaved navigation floor spawned"), Floor)) return false;
    if (!TestTrue(TEXT("public basic cube supplies navigation floor geometry"),
        Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"))))) return false;
    Floor->SetActorScale3D(FVector(20, 20, 0.5));
    Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
    State->Sentinel = Floor;
    auto* Bounds = World->SpawnActor<ANavMeshBoundsVolume>(FVector(0, 0, 100), FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("unsaved navigation bounds spawned"), Bounds)) return false;
    auto* Builder = NewObject<UCubeBuilder>(Bounds);
    Builder->X = 1800; Builder->Y = 1800; Builder->Z = 400;
    // SpawnActor alone does not create a volume's UModel/Polys. The editor's
    // supported factory initializes them before invoking the cube builder.
    UActorFactory::CreateBrushForVolumeActor(Bounds, Builder);
    const FBox NavigationBox = Bounds->GetComponentsBoundingBox(true);
    if (!TestTrue(TEXT("native volume factory creates finite navigation bounds around both endpoints"),
        NavigationBox.IsValid && NavigationBox.IsInsideOrOn(FVector(0, 0, 25))
        && NavigationBox.IsInsideOrOn(FVector(400, 0, 25)) && NavigationBox.GetSize().X >= 1800)) return false;
    FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
    auto* Navigation = UNavigationSystemV1::GetNavigationSystem(World);
    if (!TestNotNull(TEXT("fixture has native navigation system"), Navigation)) return false;
    Navigation->OnNavigationBoundsUpdated(Bounds);
    TArray<FString> Entries;
    for (int32 I = 0; I < 8; ++I)
    {
        const FVector Location = I >= 6 ? FVector(0, 0, 25) : FVector(3000 + I * 500, 0, 100);
        FActorSpawnParameters Spawn;
        auto* Actor = Cast<AFunctionalTest>(World->SpawnActor(Classes[I / 2], &Location, &FRotator::ZeroRotator, Spawn));
        if (!TestNotNull(TEXT("project-owned source recipe spawned"), Actor)) return false;
        if (I == 1 || I == 3)
        {
            const FName PropertyName = I == 1 ? TEXT("bJammedDoor") : TEXT("bDisableInteraction");
            auto* Property = FindFProperty<FBoolProperty>(Actor->GetClass(), PropertyName);
            if (!TestNotNull(TEXT("sample fault injection property exists"), Property)) return false;
            Property->SetPropertyValue_InContainer(Actor, true);
        }
        if (I == 5)
        {
            auto* Property = FindFProperty<FFloatProperty>(Actor->GetClass(), TEXT("DamageScale"));
            if (!TestNotNull(TEXT("sample damage scale property exists"), Property)) return false;
            Property->SetPropertyValue_InContainer(Actor, 0);
        }
        if (I == 7)
        {
            auto* Property = FindFProperty<FStructProperty>(Actor->GetClass(), TEXT("DestinationOffset"));
            if (!TestNotNull(TEXT("sample destination property exists"), Property)) return false;
            *Property->ContainerPtrToValuePtr<FVector>(Actor) = FVector(5000, 0, 0);
        }
        Entries.Add(State->Ids[I] + TEXT("|") + Actor->GetPathName());
    }
    GConfig->SetArray(Section, TEXT("Tests"), Entries, GGameIni);
    GConfig->SetBool(Section, TEXT("bEnabled"), true, GGameIni);
    GConfig->SetDouble(Section, TEXT("MaxJobSeconds"), 10, GGameIni);
    AddExpectedErrorPlain(TEXT("Jev recipe failed:"), EAutomationExpectedErrorFlags::Contains, 4);
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FWaitForNavigation>(State));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExerciseRecipes>(State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FAfterRecipes>(State));
    return true;
}

#endif
