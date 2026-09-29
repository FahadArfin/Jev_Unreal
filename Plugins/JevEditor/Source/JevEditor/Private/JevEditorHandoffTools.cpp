#include "JevEditorHandoffTools.h"
#include "JevEditorBridge.h"
#include "JevEditorWorkflowTools.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

FString FJevHandoffTools::SourceSha256(const TArray<uint8>& Bytes)
{
    uint8 Hash[SHA256_DIGEST_LENGTH];
    if (!SHA256(Bytes.GetData(), Bytes.Num(), Hash)) return {};
    return BytesToHex(Hash, SHA256_DIGEST_LENGTH).ToLower();
}

namespace JevHandoff
{
using namespace JevWorkflow;
constexpr int64 MaximumSourceBytes = 64 * 1024 * 1024;
const TCHAR* Section = TEXT("JevEditor.Handoff");
struct FBundle { FString Alias, Source, Hash, Asset; };
struct FPolicy { bool bEnabled = false, bValid = true; TMap<FString, FBundle> Bundles; };

bool Alias(const FString& Value)
{
    if (Value.IsEmpty() || Value.Len() > 64) return false;
    for (TCHAR C : Value) if (!FChar::IsAlnum(C) || C > 127) { if (C != '_' && C != '-') return false; }
    return true;
}

bool Ordinary(const FString& Path)
{
    IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
    FString Current = Path;
    for (int32 Depth = 0; Depth < 128; ++Depth)
    {
        if (File.IsSymlink(*Current) != ESymlinkResult::NonSymlink) return false;
        FString Parent = FPaths::GetPath(Current);
        if (Parent.IsEmpty() || Parent == Current) return true;
        Current = Parent;
    }
    return false;
}

FPolicy Policy()
{
    FPolicy Result;
    GConfig->GetBool(Section, TEXT("bEnabled"), Result.bEnabled, GGameIni);
    TArray<FString> Entries; GConfig->GetArray(Section, TEXT("Bundles"), Entries, GGameIni);
    if (Entries.Num() > 32) { Result.bValid = false; return Result; }
    TSet<FString> Assets;
    for (const FString& Entry : Entries)
    {
        TArray<FString> Parts; Entry.ParseIntoArray(Parts, TEXT("|"), false);
        bool Valid = Parts.Num() == 4;
        if (Valid)
        {
            const FString& Source = Parts[1]; const FString& Asset = Parts[3];
            Valid = Alias(Parts[0]) && !Result.Bundles.Contains(Parts[0]) && !Source.IsEmpty() && Source.Len() <= 2048 &&
                !FPaths::IsRelative(Source) && !Source.StartsWith(TEXT("\\\\")) && !Source.StartsWith(TEXT("//")) &&
                !Source.Contains(TEXT("..")) && Source.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase) &&
                Parts[2].Len() == 64 && Asset.StartsWith(TEXT("/Game/")) && Asset.Len() <= 512 &&
                FPackageName::IsValidObjectPath(Asset) && !Asset.Contains(TEXT(":")) && !Asset.Contains(TEXT("..")) && !Assets.Contains(Asset.ToLower()) &&
                FPackageName::GetLongPackageAssetName(FPackageName::ObjectPathToPackageName(Asset)) == FPackageName::ObjectPathToObjectName(Asset);
            for (TCHAR C : Parts[2]) if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) Valid = false;
            for (TCHAR C : Source) if (FChar::IsControl(C)) Valid = false;
        }
        if (!Valid) { Result.bValid = false; Result.Bundles.Reset(); return Result; }
        Result.Bundles.Add(Parts[0], {Parts[0], Parts[1], Parts[2], Parts[3]}); Assets.Add(Parts[3].ToLower());
    }
    return Result;
}

bool ReadPinned(const FBundle& Bundle, TArray<uint8>& Data)
{
    if (!Ordinary(Bundle.Source)) return false;
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Bundle.Source));
    if (!File || File->TotalSize() < 24 || File->TotalSize() > MaximumSourceBytes) return false;
    Data.SetNumUninitialized(static_cast<int32>(File->TotalSize())); File->Serialize(Data.GetData(), Data.Num());
    if (File->IsError() || File->TotalSize() != Data.Num()) return false;
    // This bounded importer deliberately accepts binary FBX only, not arbitrary importer formats.
    if (FMemory::Memcmp(Data.GetData(), "Kaydara FBX Binary  ", 19) != 0) return false;
    return FJevHandoffTools::SourceSha256(Data) == Bundle.Hash;
}

bool Exists(const FString& Asset)
{
    const FString Package = FPackageName::ObjectPathToPackageName(Asset);
    return FindObject<UObject>(nullptr, *Asset) || FindPackage(nullptr, *Package) || FPackageName::DoesPackageExist(Package);
}

UStaticMesh* CleanMesh(const FString& Asset)
{
    UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *Asset);
    if (!Mesh || Mesh->GetClass() != UStaticMesh::StaticClass() || Mesh->GetOutermost()->IsDirty() || Mesh->IsCompiling()) return nullptr;
    const FAssetData Data = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(Asset));
    return Data.IsValid() && Data.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName() ? Mesh : nullptr;
}

void Scope(const TSharedRef<FJsonObject>& Result)
{
    Result->SetBoolField(TEXT("save_requested"), false);
    Result->SetField(TEXT("saved"), MakeShared<FJsonValueNull>());
    Result->SetBoolField(TEXT("rollback_available"), false);
    Result->SetBoolField(TEXT("callback_side_effects_tracked"), false);
    Result->SetStringField(TEXT("scope"), TEXT("One fixed binary FBX static mesh import, or replacement of one clean loaded mesh. Fixed centimeter conversion, combined mesh, no materials/textures/animations import and no save requested. Trusted importer callbacks can affect other editor state. No hard timeout, cancellation or automatic rollback. Inspect and verify the unsaved result before saving manually."));
}
}

struct FJevHandoffTools::FPlan
{
    JevHandoff::FBundle Bundle;
    FString Operation;
    TWeakObjectPtr<UStaticMesh> Target;
    TSharedPtr<FJsonObject> Identity;
    double Created = 0;
    uint64 Epoch = 0;
};

FJevHandoffTools::FJevHandoffTools(TFunction<double()> InClock)
    : Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
    ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject*) { ++Epoch; });
    PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject*, FPropertyChangedEvent&) { ++Epoch; });
}
FJevHandoffTools::~FJevHandoffTools()
{
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
    Shutdown();
}
void FJevHandoffTools::Shutdown() { Plans.Reset(); }
bool FJevHandoffTools::HandlesAction(const FString& Action)
{
    return Action == TEXT("handoff_manifest") || Action == TEXT("handoff_preview") || Action == TEXT("handoff_apply");
}

TSharedRef<FJsonObject> FJevHandoffTools::Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    using namespace JevHandoff;
    auto Error = [](const TCHAR* Code, const TCHAR* Message) { return FJevEditorBridge::Error(Code, Message); };
    const FPolicy Config = Policy();
    if (!Config.bValid) return Error(TEXT("policy_invalid"), TEXT("Invalid handoff alias configuration."));
    if (Action == TEXT("handoff_manifest"))
    {
        if (!Only(Params, {})) return Error(TEXT("bad_request"), TEXT("Manifest takes no arguments."));
        auto Result = Base(Identity); Result->SetBoolField(TEXT("enabled"), Config.bEnabled);
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const auto& Pair : Config.Bundles)
        {
            auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("alias"), Pair.Key);
            Row->SetStringField(TEXT("asset_path"), Pair.Value.Asset); Row->SetStringField(TEXT("source_sha256"), Pair.Value.Hash);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("aliases"), Rows); Scope(Result); return Success(Result);
    }
    if (!HandlesAction(Action)) return Error(TEXT("unknown_action"), TEXT("Unknown handoff action."));
    if (!Config.bEnabled) return Error(TEXT("handoff_disabled"), TEXT("Handoff imports must be enabled by the project."));
    FString Project;
    if (!Text(Params, TEXT("expected_project"), Project, 2048) || Project != Identity->GetStringField(TEXT("project_file"))) return Error(TEXT("wrong_project"), TEXT("Exact project binding required."));
    if (!Editing(Identity) || (GEditor && (GEditor->PlayWorld || GEditor->IsPlaySessionRequestQueued()))) return Error(TEXT("play_mode"), TEXT("Stop PIE before importing."));
    if (bImporting) return Error(TEXT("editor_busy"), TEXT("An import is active."));
    for (auto It = Plans.CreateIterator(); It; ++It) if (Clock() - It.Value()->Created >= 120) It.RemoveCurrent();
    if (Action == TEXT("handoff_preview"))
    {
        FString Id, Operation; const TSharedPtr<FJsonObject>* State = nullptr;
        if (!Only(Params, {TEXT("alias"), TEXT("operation"), TEXT("expected_state"), TEXT("expected_project")}) ||
            !Text(Params, TEXT("alias"), Id, 64) || !Text(Params, TEXT("operation"), Operation, 16) ||
            (Operation != TEXT("import") && Operation != TEXT("reimport")) || !Params->TryGetObjectField(TEXT("expected_state"), State))
            return Error(TEXT("bad_request"), TEXT("Supply an alias, operation and reviewed state."));
        if (!Same(*State, Identity)) return Error(TEXT("stale_plan"), TEXT("Editor state changed."));
        const FBundle* Bundle = Config.Bundles.Find(Id);
        if (!Bundle) return Error(TEXT("target_not_allowed"), TEXT("Alias is not approved."));
        if (Plans.Num() >= 64) return Error(TEXT("too_many_plans"), TEXT("Wait for old previews to expire."));
        UStaticMesh* Target = Operation == TEXT("reimport") ? CleanMesh(Bundle->Asset) : nullptr;
        if ((Operation == TEXT("import") && Exists(Bundle->Asset)) || (Operation == TEXT("reimport") && !Target))
            return Error(TEXT("asset_unavailable"), TEXT("Import requires a new package; reimport requires one clean loaded native static mesh."));
        TArray<uint8> Bytes;
        if (!ReadPinned(*Bundle, Bytes)) return Error(TEXT("source_changed"), TEXT("Approved ordinary binary FBX is missing, oversized or differs from its pinned hash."));
        auto Plan = MakeShared<FPlan>(); Plan->Bundle = *Bundle; Plan->Operation = Operation; Plan->Target = Target;
        Plan->Identity = Identity; Plan->Created = Clock(); Plan->Epoch = Epoch;
        const FString PlanId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plans.Add(PlanId, Plan);
        auto Result = Base(Identity); Result->SetStringField(TEXT("plan_id"), PlanId); Result->SetStringField(TEXT("alias"), Id);
        Result->SetStringField(TEXT("operation"), Operation); Result->SetStringField(TEXT("asset_path"), Bundle->Asset);
        Result->SetStringField(TEXT("source_sha256"), Bundle->Hash); Result->SetNumberField(TEXT("source_bytes"), Bytes.Num());
        Result->SetNumberField(TEXT("expires_in_seconds"), 120); Result->SetBoolField(TEXT("can_apply"), true); Scope(Result);
        return Success(Result);
    }
    FString Id;
    if (!Only(Params, {TEXT("plan_id"), TEXT("expected_project")}) || !Text(Params, TEXT("plan_id"), Id, 64)) return Error(TEXT("bad_request"), TEXT("Supply one reviewed plan ID."));
    TSharedPtr<FPlan> Plan;
    if (!Plans.RemoveAndCopyValue(Id, Plan)) return Error(TEXT("unknown_plan"), TEXT("Preview expired, was consumed or belongs to another session."));
    const FBundle* Current = Config.Bundles.Find(Plan->Bundle.Alias);
    if (!Same(Plan->Identity, Identity) || Plan->Epoch != Epoch || !Current || Current->Source != Plan->Bundle.Source || Current->Hash != Plan->Bundle.Hash || Current->Asset != Plan->Bundle.Asset ||
        (Plan->Operation == TEXT("import") && Exists(Plan->Bundle.Asset)) ||
        (Plan->Operation == TEXT("reimport") && (!Plan->Target.IsValid() || CleanMesh(Plan->Bundle.Asset) != Plan->Target.Get())))
        return Error(TEXT("stale_plan"), TEXT("State, source approval or target changed; preview again."));
    TArray<uint8> Bytes;
    if (!ReadPinned(Plan->Bundle, Bytes)) return Error(TEXT("source_changed"), TEXT("Source bytes changed since approval."));
    // Import the exact checked bytes from a unique private staging file, closing the source-read race.
    const FString Intermediate = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir());
    if (!Ordinary(Intermediate)) return Error(TEXT("apply_failed"), TEXT("Ordinary intermediate directory unavailable."));
    const FString Directory = Intermediate / TEXT("JevHandoff");
    if (!IFileManager::Get().MakeDirectory(*Directory, true) || !Ordinary(Directory)) return Error(TEXT("apply_failed"), TEXT("Ordinary staging directory unavailable."));
    const FString Staged = Directory / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".fbx"));
    if (IFileManager::Get().FileExists(*Staged) || !FFileHelper::SaveArrayToFile(Bytes, *Staged)) return Error(TEXT("apply_failed"), TEXT("Cannot stage approved bytes."));
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*Staged); bImporting = false; };
    bImporting = true;
    TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
    TStrongObjectPtr<UFbxImportUI> Options(NewObject<UFbxImportUI>());
    Options->bImportMesh = true; Options->bImportAsSkeletal = false; Options->bImportMaterials = false;
    Options->bImportTextures = false; Options->bImportAnimations = false; Options->bAutomatedImportShouldDetectType = false;
    Options->MeshTypeToImport = FBXIT_StaticMesh; Options->bOverrideFullName = true;
    Options->StaticMeshImportData->bConvertScene = true; Options->StaticMeshImportData->bConvertSceneUnit = true;
    Options->StaticMeshImportData->bForceFrontXAxis = false; Options->StaticMeshImportData->bCombineMeshes = true;
    Options->StaticMeshImportData->bAutoGenerateCollision = true;
    Task->Filename = Staged; Task->DestinationPath = FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(Plan->Bundle.Asset));
    Task->DestinationName = FPackageName::ObjectPathToObjectName(Plan->Bundle.Asset);
    Task->bAutomated = true; Task->bAsync = false; Task->bSave = false;
    Task->bReplaceExisting = Plan->Operation == TEXT("reimport"); Task->bReplaceExistingSettings = true;
    Task->Options = Options.Get(); Task->Factory = NewObject<UFbxFactory>(Task.Get());
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task.Get()});
    const TArray<UObject*>& Objects = Task->GetObjects();
    UStaticMesh* Mesh = Objects.Num() == 1 ? Cast<UStaticMesh>(Objects[0]) : nullptr;
    const bool Passed = Mesh && Mesh->GetClass() == UStaticMesh::StaticClass() && Mesh->GetPathName() == Plan->Bundle.Asset;
    if (Passed && Mesh->GetAssetImportData()) Mesh->GetAssetImportData()->UpdateFilenameOnly(Plan->Bundle.Source);
    auto Result = Base(Identity); Result->SetStringField(TEXT("plan_id"), Id); Result->SetStringField(TEXT("alias"), Plan->Bundle.Alias);
    Result->SetStringField(TEXT("operation"), Plan->Operation); Result->SetStringField(TEXT("asset_path"), Plan->Bundle.Asset);
    Result->SetStringField(TEXT("source_sha256"), Plan->Bundle.Hash); Result->SetStringField(TEXT("status"), Passed ? TEXT("imported") : TEXT("failed"));
    if (Passed) Result->SetBoolField(TEXT("target_dirty"), Mesh->GetOutermost()->IsDirty());
    else Result->SetField(TEXT("target_dirty"), MakeShared<FJsonValueNull>());
    Scope(Result);
    return Success(Result);
}
