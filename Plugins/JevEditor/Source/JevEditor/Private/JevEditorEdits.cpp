#include "JevEditorBridge.h"

#include "ActorEditorUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "LevelUtils.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshComponentLODInfo.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace JevEdits
{
constexpr int32 MaxOperations = 20;
constexpr double PlanLifetime = 120.0;

bool OnlyFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Fields)
{
    for (const auto& Pair : Object->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& Out)
{
    return Object->HasTypedField<EJson::String>(Key) && Object->TryGetStringField(Key, Out);
}

bool Vector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FVector& Out, double Bound, bool bScale = false)
{
    if (!Object->HasField(Key)) return true;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object->TryGetArrayField(Key, Values) || Values->Num() != 3) return false;
    for (int32 I = 0; I < 3; ++I)
    {
        double Value = 0;
        if (!(*Values)[I].IsValid() || (*Values)[I]->Type != EJson::Number || !(*Values)[I]->TryGetNumber(Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > Bound || (bScale && Value < 0.001)) return false;
        Out[I] = Value;
    }
    return true;
}

TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

bool Label(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
    if (!String(Object, TEXT("label"), Out)) return false;
    Out.TrimStartAndEndInline();
    if (Out.IsEmpty() || Out.Len() > 80) return false;
    for (TCHAR Character : Out) if (Character < 32 || Character == 127) return false;
    FText Reason;
    return FActorEditorUtils::ValidateActorName(FText::FromString(Out), Reason);
}

bool Folder(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
    if (!String(Object, TEXT("folder"), Out) || Out.Len() > 256 || Out != Out.TrimStartAndEnd() || Out.Contains(TEXT("\\")) || Out.Contains(TEXT(":"))) return false;
    if (Out.IsEmpty()) return true;
    if (FName(*Out).IsNone() || Out.StartsWith(TEXT("/")) || Out.EndsWith(TEXT("/"))) return false;
    for (TCHAR Character : Out) if (Character < 32 || Character == 127) return false;
    TArray<FString> Parts;
    Out.ParseIntoArray(Parts, TEXT("/"), false);
    for (const FString& Part : Parts) if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part != Part.TrimStartAndEnd()) return false;
    return true;
}

FString MeshPath(const FString& Shape)
{
    if (Shape != TEXT("Cube") && Shape != TEXT("Sphere") && Shape != TEXT("Cylinder") && Shape != TEXT("Plane")) return {};
    return FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), *Shape, *Shape);
}

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), true);
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

bool HasOnlySupportedProperties(const UStruct* Type, const void* Source, const void* Defaults, const TSet<FName>& Copied, FString* UnsupportedProperty)
{
    int32 Scanned = 0;
    for (TFieldIterator<FProperty> It(Type); It; ++It)
    {
        if (++Scanned > 1024) return false;
        const FProperty* Property = *It;
        if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient) &&
            !Copied.Contains(Property->GetFName()) && !Property->Identical_InContainer(Source, Defaults))
        {
            if (UnsupportedProperty) *UnsupportedProperty = (Type->GetName() + TEXT(".") + Property->GetName()).Left(128);
            return false;
        }
    }
    return true;
}

bool ValidTags(const TArray<FName>& Tags)
{
    if (Tags.Num() > 32) return false;
    for (FName Tag : Tags)
    {
        const FString Text = Tag.ToString();
        if (Text.Len() > 128) return false;
        for (TCHAR Character : Text) if (Character < 32 || Character == 127) return false;
    }
    return true;
}

bool EqualJson(const TSharedRef<FJsonObject>& First, const TSharedRef<FJsonObject>& Second)
{
    FString A, B;
    auto AWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&A);
    auto BWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&B);
    return FJsonSerializer::Serialize(First, AWriter) && FJsonSerializer::Serialize(Second, BWriter) && A == B;
}

bool SameCollision(const FBodyInstance& First, const FBodyInstance& Second)
{
    return First.GetCollisionProfileName() == Second.GetCollisionProfileName() && First.GetCollisionEnabled(false) == Second.GetCollisionEnabled(false) &&
        First.GetObjectType() == Second.GetObjectType() && First.GetResponseToChannels() == Second.GetResponseToChannels();
}
}

bool FJevEditorBridge::SupportsMeshOperation(AStaticMeshActor* Actor, bool bDuplicate, FString* UnsupportedProperty) const
{
    if (!IsValid(Actor) || Actor->GetClass() != AStaticMeshActor::StaticClass()) return false;
    UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
    if (!IsValid(Component) || Component->GetClass() != UStaticMeshComponent::StaticClass() || Actor->GetRootComponent() != Component ||
        Actor->GetComponents().Num() != 1 || !IsValid(Component->GetStaticMesh()) || Component->BodyInstance.bSimulatePhysics ||
        Component->GetNumMaterials() > 64 || Component->GetNumOverrideMaterials() > Component->GetNumMaterials() ||
        Component->GetStaticMesh()->GetStaticMaterials().Num() != Component->GetNumMaterials() || Component->LODData.Num() > 8 ||
        !JevEdits::ValidTags(Actor->Tags) || !JevEdits::ValidTags(Component->ComponentTags) ||
        Component->GetCollisionProfileName().ToString().Len() > 128 || Component->BodyInstance.GetMaskFilter() != 0) return false;
    if (Component->Mobility > EComponentMobility::Movable || Component->BodyInstance.GetCollisionEnabled(false) > ECollisionEnabled::QueryAndProbe || Component->GetCollisionObjectType() >= 32) return false;
    if (Component->CustomDepthStencilValue < 0 || Component->CustomDepthStencilValue > 255 || FMath::Abs(static_cast<int64>(Component->TranslucencySortPriority)) > 32767) return false;
    for (int32 Channel = 0; Channel < 32; ++Channel)
        if (Component->GetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel)) > ECR_Block) return false;
    if (Component->bUseDefaultCollision && (!IsValid(Component->GetStaticMesh()->GetBodySetup()) || !JevEdits::SameCollision(Component->BodyInstance, Component->GetStaticMesh()->GetBodySetup()->DefaultInstance)))
    {
        if (UnsupportedProperty) *UnsupportedProperty = TEXT("StaticMeshComponent.bUseDefaultCollision: inconsistent inherited settings");
        return false;
    }
    if (const UBodySetup* Body = Component->BodyInstance.GetBodySetup())
    {
        if (Body->AggGeom.GetElementCount() > 256) return false;
        for (int32 Shape = 0; Shape < Body->AggGeom.GetElementCount(); ++Shape)
            if (Component->BodyInstance.GetShapeCollisionEnabled(Shape) != Body->AggGeom.GetElement(Shape)->GetCollisionEnabled() ||
                Component->BodyInstance.GetShapeResponseToChannels(Shape) != Component->BodyInstance.GetResponseToChannels()) return false;
    }
    for (const FStaticMeshComponentLODInfo& LOD : Component->LODData)
        if (LOD.OverrideVertexColors || !LOD.PaintedVertices.IsEmpty() || LOD.OverrideMapBuildData) return false;
    if (!bDuplicate) return true;
    if (Actor->GetOwner() || !Actor->GetPivotOffset().IsNearlyZero() || Actor->IsHiddenEdAtStartup()) return false;
    const AStaticMeshActor* Defaults = GetDefault<AStaticMeshActor>();
    const UStaticMeshComponent* DefaultComponent = Defaults->GetStaticMeshComponent();
    const TSet<FName> ActorFields = {TEXT("ActorLabel"), TEXT("FolderPath"), TEXT("FolderGuid"), TEXT("Tags"), TEXT("bHidden"), TEXT("bHiddenEdTemporary"), TEXT("bActorEnableCollision")};
    const TSet<FName> ComponentFields = {TEXT("StaticMesh"), TEXT("OverrideMaterials"), TEXT("RelativeLocation"), TEXT("RelativeRotation"), TEXT("RelativeScale3D"), TEXT("Mobility"), TEXT("BodyInstance"), TEXT("bUseDefaultCollision"), TEXT("CastShadow"), TEXT("bVisible"), TEXT("bHiddenInGame"), TEXT("ComponentTags"), TEXT("bReceivesDecals"), TEXT("bRenderCustomDepth"), TEXT("CustomDepthStencilValue"), TEXT("TranslucencySortPriority")};
    TSet<FName> BodyFields = {TEXT("CollisionProfileName"), TEXT("CollisionEnabled"), TEXT("ObjectType"), TEXT("CollisionResponses")};
    // Body construction/registration fills these cached values from project settings.
    // They are not copied and are irrelevant while their explicit override is off.
    // Enabled overrides themselves still fail the unsupported-property comparison.
    const TPair<const TCHAR*, const TCHAR*> InactiveValues[] = {
        {TEXT("bOverrideMaxAngularVelocity"), TEXT("MaxAngularVelocity")},
        {TEXT("bOverrideMaxDepenetrationVelocity"), TEXT("MaxDepenetrationVelocity")},
        {TEXT("bOverrideMass"), TEXT("MassInKgOverride")},
        {TEXT("bOverrideSolverAsyncDeltaTime"), TEXT("SolverAsyncDeltaTime")},
        {TEXT("bOverrideWalkableSlopeOnInstance"), TEXT("WalkableSlopeOverride")},
        {TEXT("bOverrideIterationCounts"), TEXT("PositionSolverIterationCount")},
        {TEXT("bOverrideIterationCounts"), TEXT("VelocitySolverIterationCount")},
        {TEXT("bOverrideIterationCounts"), TEXT("ProjectionSolverIterationCount")}
    };
    for (const auto& Value : InactiveValues)
    {
        const FBoolProperty* Flag = FindFProperty<FBoolProperty>(FBodyInstance::StaticStruct(), Value.Key);
        if (!Flag) return false;
        if (!Flag->GetPropertyValue_InContainer(&Component->BodyInstance)) BodyFields.Add(Value.Value);
    }
    return JevEdits::HasOnlySupportedProperties(Actor->GetClass(), Actor, Defaults, ActorFields, UnsupportedProperty) &&
        JevEdits::HasOnlySupportedProperties(Component->GetClass(), Component, DefaultComponent, ComponentFields, UnsupportedProperty) &&
        JevEdits::HasOnlySupportedProperties(FBodyInstance::StaticStruct(), &Component->BodyInstance, &DefaultComponent->BodyInstance, BodyFields, UnsupportedProperty);
}

bool FJevEditorBridge::SetMeshSettings(AStaticMeshActor* Actor, const FMeshSettings& Settings) const
{
    TWeakObjectPtr<AStaticMeshActor> WeakActor = Actor;
    TWeakObjectPtr<UStaticMeshComponent> WeakComponent = Actor->GetStaticMeshComponent();
    TWeakObjectPtr<UWorld> WeakWorld = Actor->GetWorld();
    TWeakObjectPtr<ULevel> WeakLevel = Actor->GetLevel();
    UStaticMeshComponent* Component = WeakComponent.Get();
    const auto Valid = [&]
    {
        return WeakActor.IsValid() && WeakComponent.IsValid() && WeakWorld.IsValid() && WeakLevel.IsValid() && GEditor &&
            GEditor->GetEditorWorldContext().World() == WeakWorld.Get() && !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor &&
            WeakActor->GetWorld() == WeakWorld.Get() && WeakActor->GetLevel() == WeakLevel.Get() && WeakWorld->GetCurrentLevel() == WeakLevel.Get() &&
            WeakActor->GetStaticMeshComponent() == WeakComponent.Get();
    };
    Component->SetMobility(static_cast<EComponentMobility::Type>(Settings.Mobility));
    if (!Valid()) return false;
    if (Component->GetCollisionProfileName() != Settings.CollisionProfile || (!Settings.bUseMeshDefaultCollision && Component->bUseDefaultCollision))
        Component->SetCollisionProfileName(Settings.CollisionProfile, false);
    if (!Valid()) return false;
    if (Component->BodyInstance.GetCollisionEnabled(false) != Settings.CollisionMode) Component->SetCollisionEnabled(static_cast<ECollisionEnabled::Type>(Settings.CollisionMode));
    if (!Valid()) return false;
    if (Component->GetCollisionObjectType() != Settings.CollisionObjectType) Component->SetCollisionObjectType(static_cast<ECollisionChannel>(Settings.CollisionObjectType));
    if (!Valid()) return false;
    for (int32 Channel = 0; Channel < 32; ++Channel)
    {
        if (Component->GetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel)) != Settings.CollisionResponses[Channel])
            Component->SetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel), static_cast<ECollisionResponse>(Settings.CollisionResponses[Channel]));
        if (!Valid()) return false;
    }
    Component->bUseDefaultCollision = Settings.bUseMeshDefaultCollision;
    Actor->SetActorEnableCollision(Settings.bActorCollisionEnabled);
    if (!Valid()) return false;
    Component->SetCastShadow(Settings.bCastShadow);
    if (!Valid()) return false;
    Component->SetReceivesDecals(Settings.bReceivesDecals);
    if (!Valid()) return false;
    Component->SetRenderCustomDepth(Settings.bRenderCustomDepth);
    if (!Valid()) return false;
    Component->SetCustomDepthStencilValue(Settings.CustomDepthStencilValue);
    if (!Valid()) return false;
    Component->SetTranslucentSortPriority(Settings.TranslucencySortPriority);
    if (!Valid()) return false;
    Component->SetVisibility(Settings.bVisible, false);
    if (!Valid()) return false;
    Component->SetHiddenInGame(Settings.bHiddenInGame, false);
    if (!Valid()) return false;
    Actor->SetActorHiddenInGame(Settings.bActorHiddenInGame);
    if (!Valid()) return false;
    Actor->SetIsTemporarilyHiddenInEditor(Settings.bActorHiddenInEditor);
    if (!Valid()) return false;
    Actor->Tags = Settings.ActorTags;
    Component->ComponentTags = Settings.ComponentTags;
    return true;
}

TSharedRef<FJsonObject> FJevEditorBridge::Preview(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    if (bApplyingPlan) return Error(TEXT("editor_busy"), TEXT("A Jev plan is applying. Inspect its outcome before another preview."));
    if (!World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel()))
        return Error(TEXT("level_locked"), TEXT("The current editor level must be editable."));
    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    if (!JevEdits::OnlyFields(Params, {TEXT("operations"), TEXT("expected_state")}) || !Params->TryGetArrayField(TEXT("operations"), Operations) || Operations->IsEmpty() || Operations->Num() > JevEdits::MaxOperations)
        return Error(TEXT("bad_request"), TEXT("preview accepts operations (1 to 20) and optional expected_state."));
    const FString InitialRevision = Revision(World);
    if (Params->HasField(TEXT("expected_state")))
    {
        const TSharedPtr<FJsonObject>* State = nullptr;
        FString ExpectedSession, ExpectedWorld, ExpectedRevision;
        if (!Params->TryGetObjectField(TEXT("expected_state"), State) || !JevEdits::OnlyFields(*State, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) ||
            !JevEdits::String(*State, TEXT("session_id"), ExpectedSession) || ExpectedSession.IsEmpty() || ExpectedSession.Len() > 128 ||
            !JevEdits::String(*State, TEXT("world_path"), ExpectedWorld) || ExpectedWorld.IsEmpty() || ExpectedWorld.Len() > 1024 ||
            !JevEdits::String(*State, TEXT("revision"), ExpectedRevision) || ExpectedRevision.IsEmpty() || ExpectedRevision.Len() > 128)
            return Error(TEXT("bad_request"), TEXT("expected_state requires exactly nonempty session_id, world_path, and revision strings."));
        if (ExpectedSession != SessionId || ExpectedWorld != World->GetPathName() || ExpectedRevision != InitialRevision)
            return Error(TEXT("stale_plan"), TEXT("Editor state changed since inspection. Inspect again before requesting a preview."));
    }
    const double Now = Clock();
    PrunePlanRecords();
    for (auto It = Plans.CreateIterator(); It; ++It) if (It.Value().ExpiresAt <= Now) It.RemoveCurrent();
    if (Plans.Num() >= 64) return Error(TEXT("too_many_plans"), TEXT("At most 64 unexpired plans can be held."));
    FPlan Plan;
    Plan.Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    Plan.World = World->GetPathName();
    Plan.WorldInstance = World;
    TWeakObjectPtr<ULevel> PreviewLevel = World->GetCurrentLevel();
    const auto PreviewContextStable = [&]
    {
        return Plan.WorldInstance.IsValid() && PreviewLevel.IsValid() && GEditor && GEditor->GetEditorWorldContext().World() == Plan.WorldInstance.Get() &&
            !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor && World->GetCurrentLevel() == PreviewLevel.Get();
    };
    for (TActorIterator<AActor> It(World); It; ++It) Plan.SceneActors.Add(*It);
    Plan.Revision = InitialRevision;
    Plan.ExpiresAt = Now + JevEdits::PlanLifetime;
    TArray<TSharedPtr<FJsonValue>> Normalized;
    TArray<TSharedPtr<FJsonValue>> Before;
    TSet<FString> ExistingTargets;
    for (const auto& Value : *Operations)
    {
        const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr)) return Error(TEXT("bad_request"), TEXT("Each operation must be an object."));
        const auto Object = *ObjectPtr;
        FOperation Operation;
        if (!JevEdits::String(Object, TEXT("op"), Operation.Op)) return Error(TEXT("bad_request"), TEXT("Each operation requires op."));
        FVector Location = FVector::ZeroVector, Rotation = FVector::ZeroVector, Scale = FVector::OneVector;
        if (Operation.Op == TEXT("spawn_primitive") || Operation.Op == TEXT("spawn_static_mesh"))
        {
            const bool bPrimitive = Operation.Op == TEXT("spawn_primitive");
            const TArray<FString> Fields = bPrimitive ? TArray<FString>{TEXT("op"), TEXT("shape"), TEXT("label"), TEXT("location"), TEXT("rotation"), TEXT("scale")} : TArray<FString>{TEXT("op"), TEXT("asset_path"), TEXT("label"), TEXT("location"), TEXT("rotation"), TEXT("scale")};
            if (!JevEdits::OnlyFields(Object, Fields) || !JevEdits::Label(Object, Operation.Label)) return Error(TEXT("bad_request"), TEXT("Spawns require an allowed asset selector and a valid actor label (1 to 80 characters)."));
            if (bPrimitive)
            {
                if (!JevEdits::String(Object, TEXT("shape"), Operation.Shape) || JevEdits::MeshPath(Operation.Shape).IsEmpty()) return Error(TEXT("bad_request"), TEXT("Unknown primitive shape."));
            }
            else
            {
                if (!JevEdits::String(Object, TEXT("asset_path"), Operation.AssetPath)) return Error(TEXT("bad_request"), TEXT("asset_path must be an exact static mesh object path."));
                UStaticMesh* Mesh = nullptr;
                if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Mesh)) return Failure.ToSharedRef();
                Operation.MeshAsset = Mesh;
            }
        }
        else if (Operation.Op == TEXT("set_transform") || Operation.Op == TEXT("set_material") || Operation.Op == TEXT("set_metadata") || Operation.Op == TEXT("replace_mesh") || Operation.Op == TEXT("duplicate_mesh"))
        {
            if (!JevEdits::String(Object, TEXT("actor_path"), Operation.ActorPath) || Operation.ActorPath.IsEmpty() || Operation.ActorPath.Len() > 1024) return Error(TEXT("bad_request"), TEXT("Existing-actor edits require an exact actor_path of 1 to 1024 characters."));
            if (ExistingTargets.Contains(Operation.ActorPath)) return Error(TEXT("bad_request"), TEXT("A plan may contain only one operation per existing actor, across all edit types."));
            AActor* Actor = FindActor(World, Operation.ActorPath);
            if (!IsValid(Actor)) return Error(TEXT("actor_not_found"), TEXT("The edit target must exist in the editor world."));
            const TArray<FString> Blockers = ActorEditBlockers(Actor);
            if (!Blockers.IsEmpty()) return Error(Blockers.Contains(TEXT("actor_locked")) || Blockers.Contains(TEXT("level_locked")) ? TEXT("actor_locked") : TEXT("actor_unsupported"), TEXT("Edits require an editable, unlocked, exact native StaticMeshActor without parent, child, or child-actor attachments."));
            if (Actor->GetActorTransform().ContainsNaN()) return Error(TEXT("actor_unsupported"), TEXT("The target has a non-finite baseline transform."));
            ExistingTargets.Add(Operation.ActorPath);
            Operation.Target = Actor;
            Operation.TargetBaseline = ActorEditFingerprint(Actor);
            Location = Actor->GetActorLocation();
            const FRotator BaselineRotation = Actor->GetActorRotation();
            Rotation = FVector(BaselineRotation.Pitch, BaselineRotation.Yaw, BaselineRotation.Roll);
            Scale = Actor->GetActorScale3D();
            if (Operation.Op == TEXT("set_transform"))
            {
                if (!JevEdits::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("location"), TEXT("rotation"), TEXT("scale")}) || (!Object->HasField(TEXT("location")) && !Object->HasField(TEXT("rotation")) && !Object->HasField(TEXT("scale")))) return Error(TEXT("bad_request"), TEXT("set_transform requires at least one transform field."));
            }
            else if (Operation.Op == TEXT("set_material"))
            {
                double Slot = 0;
                if (!JevEdits::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("material_path"), TEXT("slot")}) || !JevEdits::String(Object, TEXT("material_path"), Operation.MaterialPath) || !Object->HasTypedField<EJson::Number>(TEXT("slot")) || !Object->TryGetNumberField(TEXT("slot"), Slot) || !FMath::IsFinite(Slot) || Slot < 0 || Slot > 63 || FMath::FloorToDouble(Slot) != Slot) return Error(TEXT("bad_request"), TEXT("set_material requires exact material_path and integer slot from 0 to 63."));
                Operation.Slot = static_cast<int32>(Slot);
                const auto* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                if (Operation.Slot >= Component->GetNumMaterials()) return Error(TEXT("material_slot_invalid"), TEXT("The selected slot does not exist on this actor's static mesh."));
                UMaterialInterface* Material = nullptr;
                if (const auto Failure = ResolveMaterialAsset(Operation.MaterialPath, Material)) return Failure.ToSharedRef();
                if (!PreviewContextStable() || !Operation.Target.IsValid() || Operation.TargetBaseline != ActorEditFingerprint(Operation.Target.Get())) return Error(TEXT("stale_plan"), TEXT("The source or editor context changed while loading the material."));
                Operation.MaterialAsset = Material;
            }
            else if (Operation.Op == TEXT("set_metadata"))
            {
                if (!JevEdits::OnlyFields(Object, {TEXT("op"), TEXT("actor_path"), TEXT("label"), TEXT("folder")}) || (!Object->HasField(TEXT("label")) && !Object->HasField(TEXT("folder")))) return Error(TEXT("bad_request"), TEXT("set_metadata requires label and/or folder."));
                Operation.bSetLabel = Object->HasField(TEXT("label"));
                Operation.bSetFolder = Object->HasField(TEXT("folder"));
                if (Operation.bSetLabel && (!Actor->IsActorLabelEditable() || !JevEdits::Label(Object, Operation.Label))) return Error(TEXT("bad_request"), TEXT("The label must be editable and contain 1 to 80 valid characters."));
                if (Operation.bSetFolder && !JevEdits::Folder(Object, Operation.Folder)) return Error(TEXT("bad_request"), TEXT("folder must be empty for root, or a relative slash-separated path of at most 256 characters without empty, dot, parent, or control segments."));
            }
            else
            {
                const bool bDuplicate = Operation.Op == TEXT("duplicate_mesh");
                const TArray<FString> Fields = bDuplicate ? TArray<FString>{TEXT("op"), TEXT("actor_path"), TEXT("label"), TEXT("location"), TEXT("rotation"), TEXT("scale")} : TArray<FString>{TEXT("op"), TEXT("actor_path"), TEXT("asset_path"), TEXT("material_policy")};
                if (!JevEdits::OnlyFields(Object, Fields)) return Error(TEXT("bad_request"), TEXT("Unexpected mesh operation field."));
                auto* StaticActor = CastChecked<AStaticMeshActor>(Actor);
                UStaticMeshComponent* Component = StaticActor->GetStaticMeshComponent();
                FString Unsupported;
                if (!SupportsMeshOperation(StaticActor, bDuplicate, &Unsupported) || (bDuplicate && Actor->GetLevel() != World->GetCurrentLevel()))
                {
                    auto Failure = Error(TEXT("actor_unsupported"), TEXT("Mesh edits require one native mesh component, bounded materials/tags, no simulation, per-instance painting or per-shape overrides. Copies additionally require the current level and supported default-or-copied properties."));
                    if (!Unsupported.IsEmpty()) Failure->GetObjectField(TEXT("error"))->SetStringField(TEXT("unsupported_property"), Unsupported);
                    return Failure;
                }
                Operation.Label = Actor->GetActorLabel();
                Operation.Folder = Actor->GetFolderPath().IsNone() ? FString() : Actor->GetFolderPath().ToString();
                auto Metadata = MakeShared<FJsonObject>();
                Metadata->SetStringField(TEXT("label"), Operation.Label);
                Metadata->SetStringField(TEXT("folder"), Operation.Folder);
                FString ValidLabel, ValidFolder;
                if (!JevEdits::Label(Metadata, ValidLabel) || ValidLabel != Operation.Label || !JevEdits::Folder(Metadata, ValidFolder))
                    return Error(TEXT("actor_unsupported"), TEXT("The source label and folder must fit the bounded metadata contract."));
                Operation.MeshSettings = CaptureMeshSettings(StaticActor);
                Operation.SourceComponent = Component;
                Operation.SourcePivotOffset = Actor->GetPivotOffset();
                UStaticMesh* Mesh = Component->GetStaticMesh();
                const auto RefreshSource = [&]
                {
                    if (!PreviewContextStable() || !Operation.Target.IsValid() || !Operation.SourceComponent.IsValid() || Operation.Target->GetWorld() != World ||
                        Operation.TargetBaseline != ActorEditFingerprint(Operation.Target.Get())) return false;
                    Actor = Operation.Target.Get();
                    StaticActor = Cast<AStaticMeshActor>(Actor);
                    if (!StaticActor || StaticActor->GetStaticMeshComponent() != Operation.SourceComponent.Get()) return false;
                    Component = Operation.SourceComponent.Get();
                    return true;
                };
                if (bDuplicate)
                {
                    if (!JevEdits::Label(Object, Operation.Label)) return Error(TEXT("bad_request"), TEXT("duplicate_mesh requires a valid label of 1 to 80 characters."));
                    Operation.AssetPath = Mesh->GetPathName();
                    UStaticMesh* Resolved = nullptr;
                    if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Resolved)) return Failure.ToSharedRef();
                    if (!RefreshSource()) return Error(TEXT("stale_plan"), TEXT("The source or editor context changed while loading the mesh."));
                    if (Resolved != Mesh) return Error(TEXT("stale_plan"), TEXT("The source mesh identity changed while resolving the preview."));
                }
                else
                {
                    if (!JevEdits::String(Object, TEXT("asset_path"), Operation.AssetPath) || !JevEdits::String(Object, TEXT("material_policy"), Operation.MaterialPolicy) ||
                        (Operation.MaterialPolicy != TEXT("preserve_slots") && Operation.MaterialPolicy != TEXT("mesh_defaults")))
                        return Error(TEXT("bad_request"), TEXT("replace_mesh requires exact asset_path and material_policy preserve_slots or mesh_defaults."));
                    if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Mesh)) return Failure.ToSharedRef();
                    if (!RefreshSource()) return Error(TEXT("stale_plan"), TEXT("The source or editor context changed while loading the mesh."));
                    if (Mesh->GetStaticMaterials().Num() > 64) return Error(TEXT("actor_unsupported"), TEXT("Mesh edits support at most 64 material slots."));
                    if (Operation.MaterialPolicy == TEXT("preserve_slots") && Mesh->GetStaticMaterials().Num() != Component->GetNumMaterials())
                        return Error(TEXT("material_slot_invalid"), TEXT("preserve_slots requires equal old/new material slot counts; assignment is by index, not slot name."));
                    if (Operation.MeshSettings.bUseMeshDefaultCollision && (!IsValid(Mesh->GetBodySetup()) || !JevEdits::SameCollision(Component->BodyInstance, Mesh->GetBodySetup()->DefaultInstance)))
                        return Error(TEXT("actor_unsupported"), TEXT("The replacement mesh has different or missing inherited collision defaults. Set an explicit component collision profile in Unreal, inspect again, and preview the replacement."));
                }
                Operation.MeshAsset = Mesh;
                Operation.MeshBaseline = MeshAssetFingerprint(Mesh);
                for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
                {
                    UMaterialInterface* Effective = !bDuplicate && Operation.MaterialPolicy == TEXT("mesh_defaults") ? Mesh->GetMaterial(Slot) : Component->GetEditorMaterial(Slot);
                    if (!bDuplicate && Operation.MaterialPolicy == TEXT("preserve_slots") && !Effective && Mesh->GetMaterial(Slot))
                        return Error(TEXT("actor_unsupported"), TEXT("An empty source slot cannot override a nonempty new mesh default. Choose mesh_defaults or assign a material first."));
                    Operation.MeshMaterials.Add(Effective);
                    Operation.MaterialBaselines.Add(MaterialAssetFingerprint(Effective));
                    if (!bDuplicate && Operation.MaterialPolicy == TEXT("preserve_slots")) Operation.MeshOverrides.Add(Effective);
                }
                if (bDuplicate) for (UMaterialInterface* Material : Component->OverrideMaterials) Operation.MeshOverrides.Add(Material);
            }
        }
        else return Error(TEXT("bad_request"), TEXT("Unsupported editor operation."));
        if (!PreviewContextStable() || (!Operation.ActorPath.IsEmpty() && !Operation.Target.IsValid())) return Error(TEXT("stale_plan"), TEXT("The editor or actor identity changed while resolving the preview."));
        if (!JevEdits::Vector(Object, TEXT("location"), Location, 1000000.0) || !JevEdits::Vector(Object, TEXT("rotation"), Rotation, 36000.0) || !JevEdits::Vector(Object, TEXT("scale"), Scale, 1000.0, true)) return Error(TEXT("bad_request"), TEXT("Transform fields require three finite numbers within the documented editor bounds."));
        Operation.Transform = FTransform(FRotator(Rotation.X, Rotation.Y, Rotation.Z), Location, Scale);
        if (Operation.Op == TEXT("replace_mesh") || Operation.Op == TEXT("duplicate_mesh"))
        {
            auto TransformFields = MakeShared<FJsonObject>();
            TransformFields->SetArrayField(TEXT("location"), JevEdits::Vector(Location));
            TransformFields->SetArrayField(TEXT("rotation"), JevEdits::Vector(Rotation));
            TransformFields->SetArrayField(TEXT("scale"), JevEdits::Vector(Scale));
            if (!JevEdits::Vector(TransformFields, TEXT("location"), Location, 1000000.0) || !JevEdits::Vector(TransformFields, TEXT("rotation"), Rotation, 36000.0) || !JevEdits::Vector(TransformFields, TEXT("scale"), Scale, 1000.0, true))
                return Error(TEXT("actor_unsupported"), TEXT("Source and requested mesh transforms must fit the documented finite positive-scale bounds."));
        }
        auto Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("op"), Operation.Op);
        if (Operation.Op == TEXT("spawn_primitive")) { Summary->SetStringField(TEXT("shape"), Operation.Shape); Summary->SetStringField(TEXT("label"), Operation.Label); }
        else if (Operation.Op == TEXT("spawn_static_mesh")) { Summary->SetStringField(TEXT("asset_path"), Operation.AssetPath); Summary->SetStringField(TEXT("label"), Operation.Label); }
        else
        {
            Summary->SetStringField(TEXT("actor_path"), Operation.ActorPath);
            if (Operation.Op == TEXT("set_material")) { Summary->SetStringField(TEXT("material_path"), Operation.MaterialPath); Summary->SetNumberField(TEXT("slot"), Operation.Slot); }
            if (Operation.bSetLabel) Summary->SetStringField(TEXT("label"), Operation.Label);
            if (Operation.bSetFolder) Summary->SetStringField(TEXT("folder"), Operation.Folder);
            if (Operation.Op == TEXT("replace_mesh") || Operation.Op == TEXT("duplicate_mesh"))
            {
                Summary->SetStringField(TEXT("source_instance_id"), ActorSnapshot(Operation.Target.Get())->GetStringField(TEXT("instance_id")));
                Summary->SetStringField(TEXT("asset_path"), Operation.AssetPath);
                Summary->SetStringField(TEXT("label"), Operation.Label);
                Summary->SetStringField(TEXT("folder"), Operation.Folder);
                if (Operation.Op == TEXT("duplicate_mesh")) Summary->SetStringField(TEXT("source_actor_path"), Operation.ActorPath);
                else Summary->SetStringField(TEXT("material_policy"), Operation.MaterialPolicy);
                Summary->SetObjectField(TEXT("mesh_settings"), MeshSettingsSnapshot(Operation.MeshSettings));
                Summary->SetNumberField(TEXT("material_slot_count"), Operation.MeshMaterials.Num());
                Summary->SetNumberField(TEXT("material_override_count"), Operation.MeshOverrides.Num());
                TArray<TSharedPtr<FJsonValue>> Materials;
                for (int32 Slot = 0; Slot < Operation.MeshMaterials.Num(); ++Slot)
                {
                    auto Material = MakeShared<FJsonObject>();
                    Material->SetNumberField(TEXT("slot"), Slot);
                    if (Operation.MeshMaterials[Slot].IsValid()) Material->SetStringField(TEXT("path"), Operation.MeshMaterials[Slot]->GetPathName());
                    else Material->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
                    if (Operation.MeshOverrides.IsValidIndex(Slot) && Operation.MeshOverrides[Slot].IsValid()) Material->SetStringField(TEXT("override_path"), Operation.MeshOverrides[Slot]->GetPathName());
                    else Material->SetField(TEXT("override_path"), MakeShared<FJsonValueNull>());
                    Materials.Add(MakeShared<FJsonValueObject>(Material));
                }
                Summary->SetArrayField(TEXT("materials"), Materials);
                auto Review = MakeShared<FJsonObject>();
                Review->SetObjectField(TEXT("source_mesh"), MeshAssetSnapshot(CastChecked<AStaticMeshActor>(Operation.Target.Get())->GetStaticMeshComponent()->GetStaticMesh()));
                Review->SetObjectField(TEXT("result_mesh"), MeshAssetSnapshot(Operation.MeshAsset.Get()));
                Review->SetArrayField(TEXT("actor_pivot_offset_cm"), JevEdits::Vector(Operation.Target->GetPivotOffset()));
                Review->SetStringField(TEXT("material_assignment"), Operation.MaterialPolicy == TEXT("preserve_slots") ? TEXT("Effective source materials become explicit overrides by equal slot index; slot names do not remap assignments.") : Operation.Op == TEXT("duplicate_mesh") ? TEXT("Source effective materials and explicit override array are copied.") : TEXT("All component material overrides are cleared; new mesh defaults are used."));
                Review->SetStringField(TEXT("collision_semantics"), TEXT("Component collision settings are retained; mesh collision geometry and local bounds follow the reviewed result mesh. Collision overlap and visual fit require inspection."));
                Review->SetStringField(TEXT("copy_scope"), TEXT("A new native actor with only the reported settings when duplicating; no script state, attachments, custom components, per-instance paint, or baked lighting is cloned."));
                Summary->SetObjectField(TEXT("mesh_review"), Review);
            }
        }
        Summary->SetArrayField(TEXT("location"), JevEdits::Vector(Location));
        Summary->SetArrayField(TEXT("rotation"), JevEdits::Vector(Rotation));
        Summary->SetArrayField(TEXT("scale"), JevEdits::Vector(Scale));
        if (Operation.Target.IsValid())
        {
            const auto Snapshot = ActorSnapshot(Operation.Target.Get());
            auto Baseline = MakeShared<FJsonObject>();
            for (const TCHAR* Key : {TEXT("path"), TEXT("instance_id"), TEXT("label"), TEXT("folder"), TEXT("location"), TEXT("rotation"), TEXT("scale")})
                Baseline->SetField(Key, Snapshot->TryGetField(Key));
            if (Operation.Op == TEXT("set_material"))
            {
                const auto* Component = CastChecked<AStaticMeshActor>(Operation.Target.Get())->GetStaticMeshComponent();
                UMaterialInterface* Assigned = Component->GetEditorMaterial(Operation.Slot);
                Baseline->SetNumberField(TEXT("slot"), Operation.Slot);
                if (IsValid(Assigned)) Baseline->SetStringField(TEXT("material_path"), Assigned->GetPathName());
                else Baseline->SetField(TEXT("material_path"), MakeShared<FJsonValueNull>());
            }
            if (Operation.Op == TEXT("replace_mesh") || Operation.Op == TEXT("duplicate_mesh"))
                for (const TCHAR* Key : {TEXT("static_mesh_path"), TEXT("materials"), TEXT("material_slot_count"), TEXT("material_override_count"), TEXT("mesh_settings")}) Baseline->SetField(Key, Snapshot->TryGetField(Key));
            Before.Add(MakeShared<FJsonValueObject>(Baseline));
        }
        else Before.Add(MakeShared<FJsonValueNull>());
        Normalized.Add(MakeShared<FJsonValueObject>(Summary));
        Plan.Operations.Add(MoveTemp(Operation));
    }
    if (!PreviewContextStable() || Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor state changed while resolving the preview assets. Inspect again."));
    auto Review = MakeShared<FJsonObject>();
    Review->SetStringField(TEXT("project_file"), Plan.Project);
    Review->SetStringField(TEXT("world_path"), Plan.World);
    Review->SetStringField(TEXT("revision"), Plan.Revision);
    Review->SetArrayField(TEXT("operations"), Normalized);
    Review->SetArrayField(TEXT("before"), Before);
    FString EncodedReview;
    auto ReviewWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EncodedReview);
    if (!FJsonSerializer::Serialize(Review, ReviewWriter) || FTCHARToUTF8(*EncodedReview).Length() > 131072)
        return Error(TEXT("response_too_large"), TEXT("The human-readable plan record exceeds 128 KiB. Reduce the number of operations."));
    while (PlanRecords.Num() >= 64)
    {
        const int32 OldestCompleted = PlanRecordOrder.IndexOfByPredicate([this](const FString& Id)
        {
            const FPlanRecord* Existing = PlanRecords.Find(Id);
            return !Plans.Contains(Id) && Existing && Existing->Status != TEXT("applying");
        });
        if (OldestCompleted == INDEX_NONE) return Error(TEXT("too_many_plans"), TEXT("At most 64 retained pending plans can be held."));
        PlanRecords.Remove(PlanRecordOrder[OldestCompleted]);
        PlanRecordOrder.RemoveAt(OldestCompleted);
    }
    const FString PlanId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    FPlanRecord Record;
    Record.Review = Review;
    Record.CreatedAt = Now;
    Record.UpdatedAt = Now;
    Record.ExpiresAt = Plan.ExpiresAt;
    PlanRecords.Add(PlanId, MoveTemp(Record));
    PlanRecordOrder.Add(PlanId);
    Plans.Add(PlanId, MoveTemp(Plan));
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("plan_id"), PlanId);
    Result->SetNumberField(TEXT("expires_in_seconds"), JevEdits::PlanLifetime);
    Result->SetStringField(TEXT("revision"), InitialRevision);
    Result->SetArrayField(TEXT("operations"), Normalized);
    return JevEdits::Success(Result);
}

TSharedRef<FJsonObject> FJevEditorBridge::Apply(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
    FString PlanId;
    if (!JevEdits::OnlyFields(Params, {TEXT("plan_id")}) || !JevEdits::String(Params, TEXT("plan_id"), PlanId) || PlanId.Len() > 64) return Error(TEXT("bad_request"), TEXT("apply requires only plan_id."));
    FPlan Plan;
    if (!Plans.RemoveAndCopyValue(PlanId, Plan)) return Error(TEXT("unknown_plan"), TEXT("Plan does not exist or has already been consumed."));
#if WITH_DEV_AUTOMATION_TESTS
    if (ApplyConsumedCallbackForTesting)
    {
        TFunction<void()> Callback = MoveTemp(ApplyConsumedCallbackForTesting);
        ApplyConsumedCallbackForTesting = {};
        Callback();
    }
#endif
    if (Plan.ExpiresAt <= Clock()) return Error(TEXT("expired_plan"), TEXT("Plan expired. Preview again."));
    if (Plan.Project != FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()) || Plan.WorldInstance.Get() != World || Plan.World != World->GetPathName() || Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor state changed after preview."));
    for (auto Actor : Plan.SceneActors) if (!Actor.IsValid() || Actor->GetWorld() != World) return Error(TEXT("stale_plan"), TEXT("An actor instance was replaced after preview."));
    if (!World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel())) return Error(TEXT("level_locked"), TEXT("The current editor level must be editable."));
    if (!GEditor->CanTransact() || GEditor->IsTransactionActive() || GIsTransacting) return Error(TEXT("editor_busy"), TEXT("An independent editor Undo transaction must be available before applying a plan."));

    TMap<FString, TStrongObjectPtr<UStaticMesh>> Meshes;
    TMap<FString, TStrongObjectPtr<UMaterialInterface>> Materials;
    TMap<FString, TWeakObjectPtr<AActor>> Targets;
    const auto ResolveContextStable = [&]
    {
        return Plan.WorldInstance.IsValid() && GEditor && GEditor->GetEditorWorldContext().World() == Plan.WorldInstance.Get() &&
            !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor;
    };
    for (const FOperation& Operation : Plan.Operations)
    {
        if (Operation.Op == TEXT("spawn_primitive"))
        {
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *JevEdits::MeshPath(Operation.Shape));
            if (!IsValid(Mesh)) return Error(TEXT("asset_unavailable"), TEXT("An engine primitive mesh could not be loaded."));
            Meshes.Add(Operation.Shape, TStrongObjectPtr<UStaticMesh>(Mesh));
        }
        else if (Operation.Op == TEXT("spawn_static_mesh"))
        {
            if (!Operation.MeshAsset.IsValid()) return Error(TEXT("stale_plan"), TEXT("The selected mesh is no longer the reviewed live object."));
            UStaticMesh* Mesh = nullptr;
            if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Mesh)) return Failure.ToSharedRef();
            if (Operation.MeshAsset.Get() != Mesh) return Error(TEXT("stale_plan"), TEXT("The mesh asset was replaced after preview."));
            Meshes.Add(Operation.AssetPath, TStrongObjectPtr<UStaticMesh>(Mesh));
        }
        else
        {
            AActor* Actor = FindActor(World, Operation.ActorPath);
            if (!IsValid(Actor) || Operation.Target.Get() != Actor || !ActorEditBlockers(Actor).IsEmpty() || Operation.TargetBaseline != ActorEditFingerprint(Actor)) return Error(TEXT("stale_plan"), TEXT("A target's editability, material assignments, metadata, or object identity changed after preview."));
            Targets.Add(Operation.ActorPath, Actor);
            if (Operation.Op == TEXT("set_material"))
            {
                if (!Operation.MaterialAsset.IsValid()) return Error(TEXT("stale_plan"), TEXT("The selected material is no longer the reviewed live object."));
                const auto* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                if (Operation.Slot < 0 || Operation.Slot >= Component->GetNumMaterials()) return Error(TEXT("stale_plan"), TEXT("The reviewed material slot no longer exists."));
                UMaterialInterface* Material = nullptr;
                if (const auto Failure = ResolveMaterialAsset(Operation.MaterialPath, Material)) return Failure.ToSharedRef();
                if (Operation.MaterialAsset.Get() != Material) return Error(TEXT("stale_plan"), TEXT("The material asset was replaced after preview."));
                Materials.Add(Operation.MaterialPath, TStrongObjectPtr<UMaterialInterface>(Material));
            }
            if (Operation.Op == TEXT("replace_mesh") || Operation.Op == TEXT("duplicate_mesh"))
            {
                const bool bDuplicate = Operation.Op == TEXT("duplicate_mesh");
                auto* StaticActor = CastChecked<AStaticMeshActor>(Actor);
                if (!SupportsMeshOperation(StaticActor, bDuplicate) || Operation.SourceComponent.Get() != StaticActor->GetStaticMeshComponent() || (bDuplicate && Actor->GetLevel() != World->GetCurrentLevel()))
                    return Error(TEXT("stale_plan"), TEXT("The source no longer satisfies the reviewed mesh-copy/edit scope."));
                UStaticMesh* Mesh = nullptr;
                if (!Operation.MeshAsset.IsValid()) return Error(TEXT("stale_plan"), TEXT("The reviewed mesh object is no longer live."));
                if (const auto Failure = ResolveStaticMeshAsset(Operation.AssetPath, Mesh)) return Failure.ToSharedRef();
                if (!ResolveContextStable() || !Operation.Target.IsValid() || !Operation.SourceComponent.IsValid() || Operation.TargetBaseline != ActorEditFingerprint(Operation.Target.Get()))
                    return Error(TEXT("stale_plan"), TEXT("The reviewed source or editor context changed during mesh resolution."));
                Actor = Operation.Target.Get();
                StaticActor = CastChecked<AStaticMeshActor>(Actor);
                if (StaticActor->GetStaticMeshComponent() != Operation.SourceComponent.Get()) return Error(TEXT("stale_plan"), TEXT("The reviewed component identity changed during mesh resolution."));
                if (Mesh != Operation.MeshAsset.Get() || MeshAssetFingerprint(Mesh) != Operation.MeshBaseline)
                    return Error(TEXT("stale_plan"), TEXT("The reviewed mesh object or mesh content changed."));
                Meshes.Add(Operation.AssetPath, TStrongObjectPtr<UStaticMesh>(Mesh));
                UStaticMesh* OriginalMesh = StaticActor->GetStaticMeshComponent()->GetStaticMesh();
                Meshes.Add(OriginalMesh->GetPathName(), TStrongObjectPtr<UStaticMesh>(OriginalMesh));
                for (int32 Slot = 0; Slot < Operation.MeshMaterials.Num(); ++Slot)
                {
                    UMaterialInterface* Material = Operation.MeshMaterials[Slot].Get();
                    if (MaterialAssetFingerprint(Material) != Operation.MaterialBaselines[Slot]) return Error(TEXT("stale_plan"), TEXT("A reviewed material object or content changed."));
                    if (Material) Materials.Add(Material->GetPathName(), TStrongObjectPtr<UMaterialInterface>(Material));
                }
                for (const auto Material : Operation.MeshOverrides)
                {
                    if (!Material.IsValid() && !Material.IsExplicitlyNull()) return Error(TEXT("stale_plan"), TEXT("A reviewed material override is no longer live."));
                    if (Material.IsValid()) Materials.Add(Material->GetPathName(), TStrongObjectPtr<UMaterialInterface>(Material.Get()));
                }
            }
            if (Operation.bSetLabel && !Actor->IsActorLabelEditable()) return Error(TEXT("stale_plan"), TEXT("The actor label is no longer editable."));
        }
    }
    if (!ResolveContextStable() || Plan.Revision != Revision(World)) return Error(TEXT("stale_plan"), TEXT("Editor state changed while resolving the plan."));
    if (!GEditor->CanTransact() || GEditor->IsTransactionActive() || GIsTransacting) return Error(TEXT("editor_busy"), TEXT("The editor became busy while resolving the plan."));
    TArray<TWeakObjectPtr<AActor>> ChangedActors;
    TWeakObjectPtr<UWorld> ApplyWorld = World;
    TWeakObjectPtr<ULevel> ApplyLevel = World->GetCurrentLevel();
    const uint64 ApplyAssetEpoch = AssetChangeEpoch;
    TMap<TWeakObjectPtr<AActor>, FString> SceneBaselines;
    for (TActorIterator<AActor> It(World); It; ++It) SceneBaselines.Add(*It, ActorEditFingerprint(*It));
    const auto ContextStable = [&]
    {
        return ApplyWorld.IsValid() && ApplyLevel.IsValid() && GEditor && GEditor->GetEditorWorldContext().World() == ApplyWorld.Get() &&
            !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor && World->GetCurrentLevel() == ApplyLevel.Get() && !FLevelUtils::IsLevelLocked(ApplyLevel.Get()) && AssetChangeEpoch == ApplyAssetEpoch;
    };
    const auto AssetsStable = [&]
    {
        for (const FOperation& Item : Plan.Operations)
        {
            if (Item.Op != TEXT("replace_mesh") && Item.Op != TEXT("duplicate_mesh")) continue;
            if (!Item.MeshAsset.IsValid() || MeshAssetFingerprint(Item.MeshAsset.Get()) != Item.MeshBaseline) return false;
            for (int32 I = 0; I < Item.MeshMaterials.Num(); ++I)
                if (MaterialAssetFingerprint(Item.MeshMaterials[I].Get()) != Item.MaterialBaselines[I]) return false;
        }
        return true;
    };
    const auto SceneStable = [&](AActor* EditedActor)
    {
        if (!ContextStable() || !AssetsStable()) return false;
        for (const auto& Entry : SceneBaselines)
            if (!Entry.Key.IsValid() || Entry.Key->GetWorld() != World || (Entry.Key.Get() != EditedActor && ActorEditFingerprint(Entry.Key.Get()) != Entry.Value)) return false;
        int32 Count = 0;
        for (TActorIterator<AActor> It(World); It; ++It) ++Count;
        return Count == SceneBaselines.Num() + (EditedActor && !SceneBaselines.Contains(EditedActor) ? 1 : 0);
    };
    bool bFailed = false;
    FString FailureStage = TEXT("operation");
    FString UnsupportedProperty;
    FGuid ApplyTransactionId;
    {
        FScopedTransaction Transaction(NSLOCTEXT("JevEditor", "ApplyPlan", "Apply Jev scene plan"));
        if (!Transaction.IsOutstanding()) return Error(TEXT("editor_busy"), TEXT("An editor Undo transaction could not be started."));
        ApplyTransactionId = GEditor->Trans->GetUndoContext(false).TransactionId;
        if (!ApplyTransactionId.IsValid())
        {
            Transaction.Cancel();
            return Error(TEXT("editor_busy"), TEXT("The editor transaction identity could not be established before editing."));
        }
        World->GetCurrentLevel()->Modify();
        for (const FOperation& Operation : Plan.Operations)
        {
            AActor* Actor = nullptr;
            if (!SceneStable(nullptr)) { bFailed = true; break; }
            if (Operation.Op == TEXT("spawn_primitive") || Operation.Op == TEXT("spawn_static_mesh") || Operation.Op == TEXT("duplicate_mesh"))
            {
                const FString& Key = Operation.Op == TEXT("spawn_primitive") ? Operation.Shape : Operation.AssetPath;
                UStaticMesh* Mesh = Meshes[Key].Get();
                if (!IsValid(Mesh)) { bFailed = true; break; }
                FActorSpawnParameters Spawn;
                Spawn.OverrideLevel = World->GetCurrentLevel();
                Spawn.ObjectFlags |= RF_Transactional;
                Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                auto* StaticActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Operation.Transform, Spawn);
                if (!IsValid(StaticActor) || !ContextStable() || StaticActor->GetWorld() != World || StaticActor->GetLevel() != ApplyLevel.Get() ||
                    !IsValid(StaticActor->GetStaticMeshComponent()) || StaticActor->GetRootComponent() != StaticActor->GetStaticMeshComponent()) { bFailed = true; break; }
                Actor = StaticActor;
                TWeakObjectPtr<AStaticMeshActor> SpawnedActor = StaticActor;
                TWeakObjectPtr<UStaticMeshComponent> SpawnedComponent = StaticActor->GetStaticMeshComponent();
                Actor->Modify();
                StaticActor->GetStaticMeshComponent()->Modify();
                if (!StaticActor->GetStaticMeshComponent()->SetStaticMesh(Mesh)) { bFailed = true; break; }
                if (!ContextStable() || !SpawnedActor.IsValid() || !SpawnedComponent.IsValid() || SpawnedActor->GetStaticMeshComponent() != SpawnedComponent.Get()) { bFailed = true; break; }
                Actor->SetActorLabel(Operation.Label);
                if (!ContextStable() || !SpawnedActor.IsValid() || !SpawnedComponent.IsValid() || SpawnedActor->GetStaticMeshComponent() != SpawnedComponent.Get() || Actor->GetActorLabel() != Operation.Label) { bFailed = true; break; }
                if (Operation.Op == TEXT("duplicate_mesh"))
                {
                    UStaticMeshComponent* Component = StaticActor->GetStaticMeshComponent();
                    Component->OverrideMaterials.Reset();
                    for (const auto Material : Operation.MeshOverrides) Component->OverrideMaterials.Add(Material.Get());
                    Component->MarkRenderStateDirty();
                    Actor->SetFolderPath(Operation.Folder.IsEmpty() ? NAME_None : FName(*Operation.Folder));
                    if (!ContextStable() || !SpawnedActor.IsValid() || !SpawnedComponent.IsValid() || SpawnedActor->GetStaticMeshComponent() != SpawnedComponent.Get() || !SetMeshSettings(StaticActor, Operation.MeshSettings)) { bFailed = true; break; }
                }
                Actor->PostEditMove(true);
            }
            else
            {
                Actor = Targets[Operation.ActorPath].Get();
                if (!IsValid(Actor) || !ActorEditBlockers(Actor).IsEmpty()) { bFailed = true; break; }
                Actor->Modify();
                Actor->GetRootComponent()->Modify();
                if (Operation.Op == TEXT("set_transform"))
                {
                    if (!Actor->SetActorTransform(Operation.Transform, false, nullptr, ETeleportType::TeleportPhysics)) { bFailed = true; break; }
                    Actor->PostEditMove(true);
                    if (!IsValid(Actor) || !Actor->GetActorTransform().Equals(Operation.Transform, 0.001)) { bFailed = true; break; }
                }
                else if (Operation.Op == TEXT("set_material"))
                {
                    UMaterialInterface* Material = Materials[Operation.MaterialPath].Get();
                    if (!IsValid(Material)) { bFailed = true; break; }
                    UStaticMeshComponent* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                    Component->Modify();
                    Component->SetMaterial(Operation.Slot, Material);
                    if (!IsValid(Actor) || Component->GetEditorMaterial(Operation.Slot) != Material) { bFailed = true; break; }
                }
                else if (Operation.Op == TEXT("set_metadata"))
                {
                    if (Operation.bSetLabel) Actor->SetActorLabel(Operation.Label);
                    if (!IsValid(Actor)) { bFailed = true; break; }
                    if (Operation.bSetFolder) Actor->SetFolderPath(Operation.Folder.IsEmpty() ? NAME_None : FName(*Operation.Folder));
                    if (!IsValid(Actor) || (Operation.bSetLabel && Actor->GetActorLabel() != Operation.Label) || (Operation.bSetFolder && (Actor->GetFolderPath().IsNone() ? FString() : Actor->GetFolderPath().ToString()) != Operation.Folder)) { bFailed = true; break; }
                }
                else if (Operation.Op == TEXT("replace_mesh"))
                {
                    UStaticMeshComponent* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                    UStaticMesh* Mesh = Meshes[Operation.AssetPath].Get();
                    if (!IsValid(Mesh) || (Component->GetStaticMesh() != Mesh && !Component->SetStaticMesh(Mesh))) { bFailed = true; break; }
                    if (!ContextStable() || !IsValid(Actor) || Operation.SourceComponent.Get() != CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent()) { bFailed = true; break; }
                    Component->OverrideMaterials.Reset();
                    for (const auto Material : Operation.MeshOverrides) Component->OverrideMaterials.Add(Material.Get());
                    Component->MarkRenderStateDirty();
                    Actor->PostEditMove(true);
                }
            }
            if (!IsValid(Actor)) { bFailed = true; break; }
            if (Operation.Op == TEXT("replace_mesh") || Operation.Op == TEXT("duplicate_mesh"))
            {
#if WITH_DEV_AUTOMATION_TESTS
                if (MeshOperationCallbackForTesting)
                {
                    TFunction<void()> Callback = MoveTemp(MeshOperationCallbackForTesting);
                    MeshOperationCallbackForTesting = {};
                    Callback();
                }
#endif
                const bool bDuplicate = Operation.Op == TEXT("duplicate_mesh");
                if (!IsValid(Actor) || !SceneStable(Actor)) { FailureStage = TEXT("scene_or_asset_changed"); bFailed = true; break; }
                if (!ActorEditBlockers(Actor).IsEmpty() || !SupportsMeshOperation(CastChecked<AStaticMeshActor>(Actor), bDuplicate, &UnsupportedProperty))
                    { FailureStage = TEXT("mesh_copy_scope"); bFailed = true; break; }
                if (Actor->GetActorLabel() != Operation.Label || (Actor->GetFolderPath().IsNone() ? FString() : Actor->GetFolderPath().ToString()) != Operation.Folder ||
                    !Actor->GetActorTransform().Equals(Operation.Transform, 0.001) || !Actor->GetPivotOffset().Equals(Operation.SourcePivotOffset, 0.001))
                    { FailureStage = TEXT("mesh_metadata_or_transform"); bFailed = true; break; }
                if (!JevEdits::EqualJson(MeshSettingsSnapshot(CaptureMeshSettings(CastChecked<AStaticMeshActor>(Actor))), MeshSettingsSnapshot(Operation.MeshSettings)))
                {
                    FailureStage = TEXT("mesh_settings");
                    const auto ActualSettings = MeshSettingsSnapshot(CaptureMeshSettings(CastChecked<AStaticMeshActor>(Actor)));
                    const auto ExpectedSettings = MeshSettingsSnapshot(Operation.MeshSettings);
                    for (const auto& Pair : ExpectedSettings->Values)
                    {
                        auto ActualValue = MakeShared<FJsonObject>(), ExpectedValue = MakeShared<FJsonObject>();
                        ActualValue->SetField(TEXT("value"), ActualSettings->TryGetField(Pair.Key));
                        ExpectedValue->SetField(TEXT("value"), Pair.Value);
                        if (!JevEdits::EqualJson(ActualValue, ExpectedValue)) { FailureStage += TEXT(":") + FString(Pair.Key); break; }
                    }
                    bFailed = true;
                    break;
                }
                UStaticMeshComponent* Component = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
                if (Component->GetStaticMesh() != Operation.MeshAsset.Get() || Component->GetNumMaterials() != Operation.MeshMaterials.Num() ||
                    Component->GetNumOverrideMaterials() != Operation.MeshOverrides.Num() || (!bDuplicate && Component != Operation.SourceComponent.Get())) { bFailed = true; break; }
                for (int32 Slot = 0; Slot < Operation.MeshMaterials.Num(); ++Slot)
                    if (Component->GetEditorMaterial(Slot) != Operation.MeshMaterials[Slot].Get() || (Component->OverrideMaterials.IsValidIndex(Slot) && Component->OverrideMaterials[Slot].Get() != Operation.MeshOverrides[Slot].Get())) { bFailed = true; break; }
                if (bFailed) break;
                if (bDuplicate && (!Operation.Target.IsValid() || Actor == Operation.Target.Get() || !SupportsMeshOperation(CastChecked<AStaticMeshActor>(Operation.Target.Get()), true) || Operation.TargetBaseline != ActorEditFingerprint(Operation.Target.Get()))) { bFailed = true; break; }
            }
            if (!SceneStable(Actor)) { bFailed = true; break; }
            Actor->MarkPackageDirty();
            ChangedActors.Add(Actor);
            SceneBaselines.Add(Actor, ActorEditFingerprint(Actor));
#if WITH_DEV_AUTOMATION_TESTS
            if (FailureAfterOperationsForTesting > 0 && ChangedActors.Num() == FailureAfterOperationsForTesting)
            {
                FailureAfterOperationsForTesting = INDEX_NONE;
                bFailed = true;
                break;
            }
#endif
        }
    }
    if (bFailed)
    {
        // Finalization discards no-op transactions and restores the earlier Undo/Redo
        // stack. Never undo whatever happens to be on top: only undo our exact ID.
        // false discards this failed transaction's redo record after restoration.
        bool bRestored = false;
        // A callback may replace the editor world. Its new Undo stack is not ours.
        if (!ApplyWorld.IsValid() || !GEditor || GEditor->GetEditorWorldContext().World() != ApplyWorld.Get() || GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
            return Error(TEXT("rollback_failed"), TEXT("An editor callback changed the world/session. No unrelated Undo transaction was used; inspect the current scene."));
        if (GEditor->Trans && !GEditor->IsTransactionActive() && !GIsTransacting &&
            GEditor->Trans->GetUndoContext(false).TransactionId == ApplyTransactionId)
            bRestored = GEditor->UndoTransaction(false);
        else
            bRestored = Plan.Revision == Revision(World);
        if (!ApplyWorld.IsValid() || !GEditor || GEditor->GetEditorWorldContext().World() != ApplyWorld.Get() || GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
            return Error(TEXT("rollback_failed"), TEXT("An Undo callback changed the world/session. Inspect the current scene before further edits."));
        GEditor->RedrawLevelEditingViewports(true);
        if (!bRestored || Plan.Revision != Revision(World)) return Error(TEXT("rollback_failed"), TEXT("An editor operation failed and complete rollback could not be verified. Inspect the current scene before further edits."));
        auto Failure = Error(TEXT("apply_failed"), TEXT("An editor operation failed. The complete transaction was rolled back and the restored scene revision was verified."));
        Failure->GetObjectField(TEXT("error"))->SetStringField(TEXT("stage"), FailureStage);
        if (!UnsupportedProperty.IsEmpty()) Failure->GetObjectField(TEXT("error"))->SetStringField(TEXT("unsupported_property"), UnsupportedProperty);
        return Failure;
    }
    GEditor->RedrawLevelEditingViewports(true);
    TArray<TSharedPtr<FJsonValue>> Changed;
    for (auto Actor : ChangedActors) if (Actor.IsValid()) Changed.Add(MakeShared<FJsonValueObject>(ActorSnapshot(Actor.Get())));
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("applied"), true);
    Result->SetArrayField(TEXT("actors"), Changed);
    Result->SetStringField(TEXT("revision"), Revision(World));
    return JevEdits::Success(Result);
}
