#include "JevEditorProjectTools.h"
#include "JevEditorBridge.h"
#include "JevEditorBlueprintTools.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "WidgetBlueprint.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorValidatorBase.h"
#include "EditorFramework/AssetImportData.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace JevProject
{
constexpr int32 MaxRules = 64;
constexpr int32 MaxMessages = 256;
constexpr double RetentionSeconds = 900;
const TCHAR* ConfigSection = TEXT("JevEditor.Validation");

TSharedRef<FJsonObject> Success(const TSharedRef<FJsonObject>& Result)
{
    auto Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), true);
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

TSharedRef<FJsonObject> Base(const TSharedRef<FJsonObject>& Identity)
{
    auto Result = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        FString Value;
        if (Identity->TryGetStringField(Key, Value)) Result->SetStringField(Key, Value);
    }
    Result->SetBoolField(TEXT("cloud_used"), false);
    return Result;
}

bool Only(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& Fields)
{
    if (!Params) return false;
    for (const auto& Pair : Params->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool Integer(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Min, int32 Max, int32& Value)
{
    if (!Params->HasField(Name)) return true;
    double Number = 0;
    if (!Params->HasTypedField<EJson::Number>(Name) || !Params->TryGetNumberField(Name, Number) || !FMath::IsFinite(Number) || Number < Min || Number > Max || FMath::FloorToDouble(Number) != Number) return false;
    Value = static_cast<int32>(Number);
    return true;
}

bool Text(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, FString& Value, int32 Max = 1024)
{
    return Params->HasTypedField<EJson::String>(Name) && Params->TryGetStringField(Name, Value) && !Value.IsEmpty() && Value.Len() <= Max;
}

bool AssetPath(const FString& Path)
{
    if (Path.Len() > 1024 || !(Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/"))) || Path.Contains(TEXT(":")) || Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\"))) return false;
    for (const TCHAR Char : Path) if (FChar::IsWhitespace(Char) || FChar::IsControl(Char)) return false;
    return FPackageName::IsValidObjectPath(Path) && !FPackageName::ObjectPathToObjectName(Path).IsEmpty();
}

FString Clip(const FString& Value, int32 Limit, bool& bTruncated)
{
    bTruncated |= Value.Len() > Limit;
    return Value.Left(Limit);
}

FAssetData RegistryAsset(const FString& Path)
{
    return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(Path));
}

TSharedRef<FJsonObject> PinType(const FEdGraphPinType& Type, bool& bTruncated)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("category"), Clip(Type.PinCategory.ToString(), 128, bTruncated));
    Result->SetStringField(TEXT("subcategory"), Clip(Type.PinSubCategory.ToString(), 128, bTruncated));
    Result->SetStringField(TEXT("object_type"), Clip(GetPathNameSafe(Type.PinSubCategoryObject.Get()), 256, bTruncated));
    Result->SetStringField(TEXT("container"), Type.ContainerType == EPinContainerType::Array ? TEXT("array") : Type.ContainerType == EPinContainerType::Set ? TEXT("set") : Type.ContainerType == EPinContainerType::Map ? TEXT("map") : TEXT("none"));
    Result->SetBoolField(TEXT("reference"), Type.bIsReference);
    Result->SetBoolField(TEXT("const"), Type.bIsConst);
    return Result;
}

FString BlueprintStatus(EBlueprintStatus Status)
{
    switch (Status)
    {
    case BS_Unknown: return TEXT("unknown");
    case BS_Dirty: return TEXT("dirty");
    case BS_Error: return TEXT("error");
    case BS_UpToDate: return TEXT("up_to_date");
    case BS_BeingCreated: return TEXT("being_created");
    case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
    default: return TEXT("unknown");
    }
}

TSharedRef<FJsonObject> Blueprint(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    FString Path;
    int32 GraphLimit = 16, NodeLimit = 128, PinLimit = 512;
    if (!Only(Params, {TEXT("asset_path"), TEXT("graph_limit"), TEXT("node_limit"), TEXT("pin_limit")}) || !Text(Params, TEXT("asset_path"), Path) || !AssetPath(Path) || !Integer(Params, TEXT("graph_limit"), 1, 32, GraphLimit) || !Integer(Params, TEXT("node_limit"), 1, 256, NodeLimit) || !Integer(Params, TEXT("pin_limit"), 1, 1024, PinLimit)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Blueprint inspection requires one exact /Game or /Engine object path and bounded integer graph/node/pin limits."));
    const FAssetData Data = RegistryAsset(Path);
    if (!Data.IsValid()) return FJevEditorBridge::Error(TEXT("asset_not_found"), TEXT("The exact asset is absent from the asset registry."));
    if (Data.AssetClassPath != UBlueprint::StaticClass()->GetClassPathName() && Data.AssetClassPath != UWidgetBlueprint::StaticClass()->GetClassPathName() && Data.AssetClassPath != UAnimBlueprint::StaticClass()->GetClassPathName()) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("Inspection supports exact native Blueprint, WidgetBlueprint and AnimBlueprint assets only."));
    UBlueprint* BP = FindObject<UBlueprint>(nullptr, *Path);
    if (!BP) return FJevEditorBridge::Error(TEXT("asset_not_loaded"), TEXT("Open this Blueprint in Unreal first. Inspection never loads or compiles a Blueprint implicitly."));
    if (!FJevBlueprintTools::IsSupportedClass(BP->GetClass()) || BP->GetClass()->GetClassPathName() != Data.AssetClassPath || BP->GetPathName() != Path) return FJevEditorBridge::Error(TEXT("unsupported_asset"), TEXT("Loaded Blueprint identity does not match the requested native asset."));
    auto Result = Base(Identity);
    bool bTruncated = false;
    Result->SetStringField(TEXT("asset_path"), Path);
    Result->SetStringField(TEXT("compile_status"), BlueprintStatus(BP->Status));
    Result->SetStringField(TEXT("asset_class"), BP->GetClass()->GetPathName());
    Result->SetStringField(TEXT("blueprint_kind"), BP->GetClass() == UWidgetBlueprint::StaticClass() ? TEXT("widget") : BP->GetClass() == UAnimBlueprint::StaticClass() ? TEXT("animation") : TEXT("blueprint"));
    if (const auto* Anim = Cast<UAnimBlueprint>(BP)) Result->SetStringField(TEXT("target_skeleton"), GetPathNameSafe(Anim->TargetSkeleton));
    if (const auto* Widget = Cast<UWidgetBlueprint>(BP)) Result->SetNumberField(TEXT("widget_animation_count"), Widget->Animations.Num());
    Result->SetStringField(TEXT("parent_class"), GetPathNameSafe(BP->ParentClass));
    Result->SetStringField(TEXT("generated_class"), GetPathNameSafe(BP->GeneratedClass));
    Result->SetBoolField(TEXT("package_dirty"), BP->GetOutermost()->IsDirty());
    Result->SetBoolField(TEXT("compiled"), false);
    Result->SetBoolField(TEXT("modified"), false);
    Result->SetStringField(TEXT("diagnostic_scope"), TEXT("Stored node compiler messages and existing compile status; no new compiler run and no complete historical compiler log."));
    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const auto& Variable : BP->NewVariables)
    {
        if (Variables.Num() == 128) { bTruncated = true; break; }
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Clip(Variable.VarName.ToString(), 128, bTruncated));
        Row->SetStringField(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphens));
        Row->SetObjectField(TEXT("type"), PinType(Variable.VarType, bTruncated));
        Row->SetStringField(TEXT("category"), Clip(Variable.Category.ToString(), 128, bTruncated));
        // Defaults can contain private project data; only expose type and identity.
        Variables.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("variables"), Variables);
    Result->SetNumberField(TEXT("variable_count"), BP->NewVariables.Num());
    TArray<UEdGraph*> Graphs;
    TSet<UEdGraph*> SeenGraphs;
    int32 GraphReferences = 0;
    bool bGraphScanComplete = true;
    auto AddGraph = [&](UEdGraph* Graph)
    {
        if (++GraphReferences > 1024 || Graphs.Num() >= 256) { bGraphScanComplete = false; return; }
        if (Graph && !SeenGraphs.Contains(Graph)) { SeenGraphs.Add(Graph); Graphs.Add(Graph); }
    };
    auto AddGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Source)
    {
        for (UEdGraph* Graph : Source) { AddGraph(Graph); if (!bGraphScanComplete) break; }
    };
    AddGraphs(BP->FunctionGraphs); AddGraphs(BP->MacroGraphs); AddGraphs(BP->UbergraphPages); AddGraphs(BP->DelegateSignatureGraphs);
    for (const auto& Interface : BP->ImplementedInterfaces)
    {
        if (!bGraphScanComplete || ++GraphReferences > 1024) { bGraphScanComplete = false; break; }
        AddGraphs(Interface.Graphs);
    }
    for (int32 I = 0; I < Graphs.Num() && bGraphScanComplete; ++I) AddGraphs(Graphs[I]->SubGraphs);
    const bool bExtensionsOmitted = !BP->GetExtensions().IsEmpty();
    bTruncated |= !bGraphScanComplete || bExtensionsOmitted;
    Result->SetBoolField(TEXT("graph_scan_complete"), bGraphScanComplete && !bExtensionsOmitted);
    Result->SetBoolField(TEXT("extension_graphs_omitted"), bExtensionsOmitted);
    Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GetPathName() < B.GetPathName(); });
    TArray<TSharedPtr<FJsonValue>> GraphRows;
    int32 NodesReturned = 0, PinsReturned = 0;
    for (const UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        if (GraphRows.Num() >= GraphLimit) { bTruncated = true; break; }
        auto GraphRow = MakeShared<FJsonObject>();
        GraphRow->SetStringField(TEXT("path"), Clip(Graph->GetPathName(), 512, bTruncated));
        GraphRow->SetStringField(TEXT("name"), Clip(Graph->GetName(), 128, bTruncated));
        GraphRow->SetStringField(TEXT("guid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
        GraphRow->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            if (NodesReturned >= NodeLimit) { bTruncated = true; break; }
            ++NodesReturned;
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            Row->SetStringField(TEXT("class"), Clip(Node->GetClass()->GetPathName(), 256, bTruncated));
            Row->SetStringField(TEXT("name"), Clip(Node->GetName(), 128, bTruncated));
            Row->SetNumberField(TEXT("x"), Node->NodePosX);
            Row->SetNumberField(TEXT("y"), Node->NodePosY);
            Row->SetBoolField(TEXT("has_compiler_message"), !!Node->bHasCompilerMessage);
            if (Node->bHasCompilerMessage)
            {
                Row->SetNumberField(TEXT("severity"), Node->ErrorType);
                Row->SetStringField(TEXT("compiler_message"), Clip(Node->ErrorMsg, 512, bTruncated));
            }
            TArray<TSharedPtr<FJsonValue>> Pins;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                if (PinsReturned >= PinLimit) { bTruncated = true; break; }
                ++PinsReturned;
                auto PinRow = MakeShared<FJsonObject>();
                PinRow->SetStringField(TEXT("guid"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
                PinRow->SetStringField(TEXT("name"), Clip(Pin->PinName.ToString(), 128, bTruncated));
                PinRow->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                PinRow->SetObjectField(TEXT("type"), PinType(Pin->PinType, bTruncated));
                PinRow->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
                TArray<TSharedPtr<FJsonValue>> Links;
                for (const UEdGraphPin* Link : Pin->LinkedTo)
                {
                    if (!Link) continue;
                    if (!Link->GetOwningNode()) { bTruncated = true; continue; }
                    if (Links.Num() == 8) { bTruncated = true; break; }
                    auto LinkRow = MakeShared<FJsonObject>();
                    LinkRow->SetStringField(TEXT("pin_guid"), Link->PinId.ToString(EGuidFormats::DigitsWithHyphens));
                    LinkRow->SetStringField(TEXT("node_guid"), Link->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
                    Links.Add(MakeShared<FJsonValueObject>(LinkRow));
                }
                PinRow->SetArrayField(TEXT("links"), Links);
                PinRow->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
                Pins.Add(MakeShared<FJsonValueObject>(PinRow));
            }
            Row->SetArrayField(TEXT("pins"), Pins);
            Row->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
            Nodes.Add(MakeShared<FJsonValueObject>(Row));
        }
        GraphRow->SetArrayField(TEXT("nodes"), Nodes);
        GraphRows.Add(MakeShared<FJsonValueObject>(GraphRow));
    }
    Result->SetArrayField(TEXT("graphs"), GraphRows);
    Result->SetNumberField(TEXT("graph_count"), Graphs.Num());
    Result->SetNumberField(TEXT("nodes_returned"), NodesReturned);
    Result->SetNumberField(TEXT("pins_returned"), PinsReturned);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    return Success(Result);
}

TSharedRef<FJsonObject> Dependencies(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    FString Path, Direction = TEXT("dependencies"), Category = TEXT("package");
    int32 Offset = 0, Limit = 100;
    if (!Only(Params, {TEXT("asset_path"), TEXT("direction"), TEXT("category"), TEXT("offset"), TEXT("limit")}) || !Text(Params, TEXT("asset_path"), Path) || !AssetPath(Path) || !Integer(Params, TEXT("offset"), 0, 100000, Offset) || !Integer(Params, TEXT("limit"), 1, 200, Limit) || (Params->HasField(TEXT("direction")) && !Text(Params, TEXT("direction"), Direction, 16)) || (Params->HasField(TEXT("category")) && !Text(Params, TEXT("category"), Category, 20)) || (Direction != TEXT("dependencies") && Direction != TEXT("referencers")) || (Category != TEXT("package") && Category != TEXT("manage") && Category != TEXT("searchable_name"))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Dependency queries require one exact asset, a supported direction/category, offset 0..100000 and limit 1..200."));
    const FAssetData Asset = RegistryAsset(Path);
    if (!Asset.IsValid()) return FJevEditorBridge::Error(TEXT("asset_not_found"), TEXT("The exact asset is absent from the asset registry."));
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const auto Mask = Category == TEXT("package") ? UE::AssetRegistry::EDependencyCategory::Package : Category == TEXT("manage") ? UE::AssetRegistry::EDependencyCategory::Manage : UE::AssetRegistry::EDependencyCategory::SearchableName;
    TArray<FAssetDependency> Edges;
    if (Direction == TEXT("dependencies")) Registry.GetDependencies(FAssetIdentifier(Asset.PackageName), Edges, Mask);
    else Registry.GetReferencers(FAssetIdentifier(Asset.PackageName), Edges, Mask);
    Edges.Sort([](const FAssetDependency& A, const FAssetDependency& B) { return A.LexicalLess(B); });
    auto Result = Base(Identity);
    Result->SetStringField(TEXT("asset_path"), Path);
    Result->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
    Result->SetStringField(TEXT("direction"), Direction);
    Result->SetStringField(TEXT("category"), Category);
    Result->SetStringField(TEXT("scope"), TEXT("Direct package-level asset-registry edges; no recursive traversal, asset loading or proof of runtime references. Pages can change while the registry updates."));
    TArray<TSharedPtr<FJsonValue>> Rows;
    bool bTextTruncated = false;
    for (int32 Index = Offset; Index < Edges.Num() && Rows.Num() < Limit; ++Index)
    {
        const auto& Edge = Edges[Index];
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("identifier"), Clip(Edge.AssetId.ToString(), 1024, bTextTruncated));
        Row->SetNumberField(TEXT("properties"), static_cast<uint32>(Edge.Properties));
        Row->SetStringField(TEXT("category"), Category);
        if (Category == TEXT("package"))
        {
            Row->SetBoolField(TEXT("hard"), EnumHasAnyFlags(Edge.Properties, UE::AssetRegistry::EDependencyProperty::Hard));
            Row->SetBoolField(TEXT("game"), EnumHasAnyFlags(Edge.Properties, UE::AssetRegistry::EDependencyProperty::Game));
            Row->SetBoolField(TEXT("build"), EnumHasAnyFlags(Edge.Properties, UE::AssetRegistry::EDependencyProperty::Build));
        }
        if (Category == TEXT("manage"))
        {
            Row->SetBoolField(TEXT("direct"), EnumHasAnyFlags(Edge.Properties, UE::AssetRegistry::EDependencyProperty::Direct));
            Row->SetBoolField(TEXT("cook_rule"), EnumHasAnyFlags(Edge.Properties, UE::AssetRegistry::EDependencyProperty::CookRule));
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("edges"), Rows);
    Result->SetNumberField(TEXT("total"), Edges.Num());
    Result->SetNumberField(TEXT("offset"), Offset);
    const bool bMore = Offset + Rows.Num() < Edges.Num();
    if (bMore) Result->SetNumberField(TEXT("next_offset"), Offset + Rows.Num());
    else Result->SetField(TEXT("next_offset"), MakeShared<FJsonValueNull>());
    Result->SetBoolField(TEXT("truncated"), bMore || bTextTruncated);
    Result->SetBoolField(TEXT("registry_loading"), Registry.IsLoadingAssets());
    Result->SetBoolField(TEXT("modified"), false);
    return Success(Result);
}

TSharedRef<FJsonObject> ImportInfo(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    FString Path;
    if (!Only(Params, {TEXT("asset_path")}) || !Text(Params, TEXT("asset_path"), Path) || !AssetPath(Path)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Import inspection requires one exact /Game or /Engine asset path."));
    const FAssetData Data = RegistryAsset(Path);
    if (!Data.IsValid()) return FJevEditorBridge::Error(TEXT("asset_not_found"), TEXT("The exact asset is absent from the asset registry."));
    auto Result = Base(Identity);
    Result->SetStringField(TEXT("asset_path"), Path);
    Result->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
    Result->SetBoolField(TEXT("modified"), false);
    Result->SetStringField(TEXT("scope"), TEXT("Recorded import provenance from asset-registry metadata only. Source paths are reduced to basenames; files are never opened. Missing metadata is not proof of a missing source file."));
    FString Json;
    const bool bPresent = Data.GetTagValue(FName(TEXT("SourceFile")), Json);
    Result->SetBoolField(TEXT("metadata_present"), bPresent);
    TArray<TSharedPtr<FJsonValue>> Sources;
    bool bTruncated = Json.Len() > 65536;
    bool bParsed = false;
    if (bPresent && !bTruncated)
    {
        const auto Info = FAssetImportInfo::FromJson(Json);
        bParsed = Info.IsSet();
        if (Info)
        {
            for (const auto& Source : Info->SourceFiles)
            {
                if (Sources.Num() == 16) { bTruncated = true; break; }
                auto Row = MakeShared<FJsonObject>();
                FString Normalized = Source.RelativeFilename.Replace(TEXT("\\"), TEXT("/"));
                Row->SetStringField(TEXT("filename"), Clip(FPaths::GetCleanFilename(Normalized), 256, bTruncated));
                Row->SetStringField(TEXT("recorded_timestamp_utc"), Source.Timestamp.ToIso8601());
                Row->SetStringField(TEXT("recorded_md5"), Source.FileHash.IsValid() ? LexToString(Source.FileHash) : FString());
                Sources.Add(MakeShared<FJsonValueObject>(Row));
            }
        }
    }
    Result->SetArrayField(TEXT("sources"), Sources);
    Result->SetBoolField(TEXT("metadata_parsed"), bParsed);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    return Success(Result);
}

struct FRule { FString Id; FString ClassPath; };

bool RuleId(const FString& Id)
{
    if (Id.IsEmpty() || Id.Len() > 64) return false;
    for (const TCHAR C : Id) if (!(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.'))) return false;
    return true;
}

TArray<FRule> Rules(bool& bEnabled, bool& bConfigValid, double& Timeout)
{
    bEnabled = false; bConfigValid = true; Timeout = 30;
    TArray<FString> Entries;
    if (GConfig)
    {
        GConfig->GetBool(ConfigSection, TEXT("bEnabled"), bEnabled, GGameIni);
        GConfig->GetArray(ConfigSection, TEXT("Rules"), Entries, GGameIni);
        GConfig->GetDouble(ConfigSection, TEXT("MaxJobSeconds"), Timeout, GGameIni);
    }
    bConfigValid = Entries.Num() <= MaxRules && FMath::IsFinite(Timeout) && Timeout >= 1 && Timeout <= 120;
    TSet<FString> Seen;
    TArray<FRule> Result;
    for (const FString& Entry : Entries)
    {
        FString Id, ClassPath;
        if (!Entry.Split(TEXT("|"), &Id, &ClassPath) || !RuleId(Id) || !ClassPath.StartsWith(TEXT("/Script/")) || ClassPath.Len() > 512 || ClassPath.Contains(TEXT("|")) || !FPackageName::IsValidObjectPath(ClassPath) || Seen.Contains(Id)) { bConfigValid = false; continue; }
        Seen.Add(Id);
        if (Result.Num() < MaxRules) Result.Add({Id, ClassPath});
    }
    return Result;
}

UClass* RuleClass(const FString& ClassPath)
{
    UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
    return Class && Class->HasAnyClassFlags(CLASS_Native) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) && Class->IsChildOf(UEditorValidatorBase::StaticClass()) ? Class : nullptr;
}

bool SameIdentity(const TSharedRef<FJsonObject>& A, const TSharedRef<FJsonObject>& B)
{
    for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path")})
    {
        FString AV, BV;
        if (!A->TryGetStringField(Key, AV) || !B->TryGetStringField(Key, BV) || AV.IsEmpty() || AV != BV) return false;
    }
    return true;
}

bool Strings(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, int32 Limit, bool bAssetPaths, TArray<FString>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Values;
    if (!Params->TryGetArrayField(Key, Values) || Values->IsEmpty() || Values->Num() > Limit) return false;
    TSet<FString> Seen;
    for (const auto& Value : *Values)
    {
        FString String;
        if (!Value || Value->Type != EJson::String || !Value->TryGetString(String) || !(bAssetPaths ? AssetPath(String) : RuleId(String)) || Seen.Contains(String)) return false;
        Seen.Add(String); Out.Add(String);
    }
    return true;
}
}

struct FJevProjectTools::FJob
{
    FString Id;
    FString State = TEXT("queued");
    FString StopReason;
    TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
    TArray<FString> Assets;
    TArray<JevProject::FRule> Rules;
    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Index = 0;
    int32 Messages = 0;
    double Started = 0;
    double Finished = 0;
    double Timeout = 30;
    bool bTruncated = false;
};

FJevProjectTools::FJevProjectTools() = default;
FJevProjectTools::~FJevProjectTools() = default;

bool FJevProjectTools::HandlesAction(const FString& Action)
{
    return Action == TEXT("blueprint_inspect") || Action == TEXT("asset_dependencies") || Action == TEXT("asset_import_info") || Action == TEXT("validation_rules") || Action == TEXT("validation_start") || Action == TEXT("validation_job") || Action == TEXT("validation_cancel");
}

TSharedRef<FJsonObject> FJevProjectTools::ValidationRules(const TSharedRef<FJsonObject>& Identity) const
{
    bool bEnabled, bValid; double Timeout;
    const auto Rules = JevProject::Rules(bEnabled, bValid, Timeout);
    auto Result = JevProject::Base(Identity);
    Result->SetBoolField(TEXT("enabled"), bEnabled && bValid);
    Result->SetBoolField(TEXT("configuration_valid"), bValid);
    Result->SetNumberField(TEXT("max_job_seconds"), bValid ? Timeout : 0);
    Result->SetStringField(TEXT("configuration_section"), TEXT("JevEditor.Validation in project DefaultGame.ini"));
    Result->SetStringField(TEXT("execution_scope"), TEXT("Only explicitly configured loaded native UEditorValidatorBase classes. Validators and asset loading execute trusted project code and may have side effects. No automatic fixes or saves are requested."));
    Result->SetStringField(TEXT("instance_lifetime"), TEXT("fresh_transient_instance_per_asset_rule"));
    Result->SetBoolField(TEXT("post_asset_validation_called"), false);
    Result->SetStringField(TEXT("compatibility_requirement"), TEXT("Rules must complete their verdict and release resources within ValidateLoadedAsset. Rules requiring a shared subsystem instance, changelist setup, cross-asset accumulation or PostAssetValidation cleanup are unsupported."));
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const auto& Rule : Rules)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("id"), Rule.Id);
        Row->SetStringField(TEXT("class_path"), Rule.ClassPath);
        Row->SetBoolField(TEXT("available"), JevProject::RuleClass(Rule.ClassPath) != nullptr);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("rules"), Rows);
    return JevProject::Success(Result);
}

TSharedRef<FJsonObject> FJevProjectTools::JobSnapshot(const FJob& Job) const
{
    auto Result = JevProject::Base(Job.Identity);
    Result->SetStringField(TEXT("job_id"), Job.Id);
    Result->SetStringField(TEXT("state"), Job.State);
    Result->SetStringField(TEXT("stop_reason"), Job.StopReason);
    Result->SetNumberField(TEXT("completed"), Job.Index);
    Result->SetNumberField(TEXT("total"), Job.Assets.Num() * Job.Rules.Num());
    Result->SetNumberField(TEXT("elapsed_seconds"), (Job.Finished > 0 ? Job.Finished : FPlatformTime::Seconds()) - Job.Started);
    Result->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, JevProject::RetentionSeconds - (FPlatformTime::Seconds() - (Job.Finished > 0 ? Job.Finished : Job.Started))));
    Result->SetArrayField(TEXT("results"), Job.Results);
    Result->SetBoolField(TEXT("truncated"), Job.bTruncated);
    bool bInvalid = false, bNotValidated = false;
    for (const auto& Row : Job.Results)
    {
        const FString State = Row->AsObject()->GetStringField(TEXT("result"));
        bInvalid |= State == TEXT("invalid");
        bNotValidated |= State == TEXT("not_validated");
    }
    Result->SetStringField(TEXT("verdict"), Job.State != TEXT("completed") ? TEXT("incomplete") : bInvalid ? TEXT("invalid") : bNotValidated ? TEXT("not_validated") : TEXT("valid"));
    Result->SetBoolField(TEXT("save_requested"), false);
    Result->SetField(TEXT("saved"), MakeShared<FJsonValueNull>());
    Result->SetBoolField(TEXT("callback_side_effects_tracked"), false);
    Result->SetStringField(TEXT("instance_lifetime"), TEXT("fresh_transient_instance_per_asset_rule"));
    Result->SetBoolField(TEXT("post_asset_validation_called"), false);
    Result->SetStringField(TEXT("cancellation"), TEXT("Cooperative between asset/rule calls. Loading or a running validator cannot be interrupted; elapsed time can exceed the configured limit."));
    Result->SetStringField(TEXT("scope"), TEXT("Selected rules only; no global validation, recursive dependencies, UObject::IsDataValid, automatic fixes or saves."));
    return JevProject::Success(Result);
}

void FJevProjectTools::Prune()
{
    const double Now = FPlatformTime::Seconds();
    for (auto It = Jobs.CreateIterator(); It; ++It) if (It.Key() != ActiveJob && Now - It.Value()->Finished >= JevProject::RetentionSeconds) It.RemoveCurrent();
    while (Jobs.Num() >= 8)
    {
        FString Oldest; double Time = DBL_MAX;
        for (const auto& Pair : Jobs) if (Pair.Key != ActiveJob && Pair.Value->Started < Time) { Oldest = Pair.Key; Time = Pair.Value->Started; }
        if (Oldest.IsEmpty()) break;
        Jobs.Remove(Oldest);
    }
}

TSharedRef<FJsonObject> FJevProjectTools::StartValidation(const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    TArray<FString> Assets, Ids;
    FString ExpectedProject;
    const TSharedPtr<FJsonObject>* ExpectedState = nullptr;
    if (!JevProject::Only(Params, {TEXT("asset_paths"), TEXT("rule_ids"), TEXT("expected_project"), TEXT("expected_state")}) || !JevProject::Strings(Params, TEXT("asset_paths"), 20, true, Assets) || !JevProject::Strings(Params, TEXT("rule_ids"), 8, false, Ids) || !JevProject::Text(Params, TEXT("expected_project"), ExpectedProject, 2048) || !Params->TryGetObjectField(TEXT("expected_state"), ExpectedState) || !JevProject::Only(*ExpectedState, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Validation requires 1..20 unique exact assets, 1..8 rule IDs and the authenticated expected_project and expected_state."));
    FString CurrentProject;
    if (!Identity->TryGetStringField(TEXT("project_file"), CurrentProject) || ExpectedProject != CurrentProject) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("The editor project changed after validation preflight."));
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
    {
        FString Expected, Current;
        const int32 Limit = FString(Key) == TEXT("session_id") ? 64 : FString(Key) == TEXT("revision") ? 128 : 1024;
        if (!JevProject::Text(*ExpectedState, Key, Expected, Limit)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("expected_state requires bounded nonempty session_id, world_path and revision strings."));
        if (!Identity->TryGetStringField(Key, Current) || Current != Expected) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("The editor session, world or revision changed after validation preflight."));
    }
    bool bPIE = false, bSimulating = false;
    Identity->TryGetBoolField(TEXT("play_in_editor"), bPIE); Identity->TryGetBoolField(TEXT("simulating"), bSimulating);
    if (!JevProject::SameIdentity(Identity, Identity)) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Validation requires an explicit current project, session and world identity."));
    if (bPIE || bSimulating) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("Validation cannot start during Play or Simulate in Editor."));
    bool bEnabled, bValid; double Timeout;
    const auto Available = JevProject::Rules(bEnabled, bValid, Timeout);
    if (!bEnabled || !bValid) return FJevEditorBridge::Error(TEXT("validation_disabled"), TEXT("Project validation execution is disabled or its explicit rule configuration is invalid."));
    if (!ActiveJob.IsEmpty()) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("Only one validation job can run at a time."));
    auto Job = MakeShared<FJob>();
    for (const auto& Id : Ids)
    {
        const auto* Rule = Available.FindByPredicate([&Id](const JevProject::FRule& Candidate) { return Candidate.Id == Id; });
        if (!Rule) return FJevEditorBridge::Error(TEXT("rule_not_allowed"), TEXT("A requested rule ID is not explicitly configured by the project."));
        if (!JevProject::RuleClass(Rule->ClassPath)) return FJevEditorBridge::Error(TEXT("rule_unavailable"), TEXT("A configured validator must be a loaded concrete native UEditorValidatorBase class."));
        Job->Rules.Add(*Rule);
    }
    for (const auto& Path : Assets) if (!JevProject::RegistryAsset(Path).IsValid()) return FJevEditorBridge::Error(TEXT("asset_not_found"), TEXT("Every selected asset must exist in the asset registry before a validation job starts."));
    Job->Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Job->Identity = JevProject::Base(Identity);
    Job->Assets = MoveTemp(Assets);
    Job->Started = FPlatformTime::Seconds();
    Job->Timeout = Timeout;
    Prune();
    Jobs.Add(Job->Id, Job);
    ActiveJob = Job->Id;
    return JobSnapshot(*Job);
}

TSharedRef<FJsonObject> FJevProjectTools::Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Identity)
{
    check(IsInGameThread());
    if (!Params) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Parameters must be an object."));
    if (bExecutingCallbacks && (Action == TEXT("validation_start") || Action == TEXT("validation_cancel"))) return FJevEditorBridge::Error(TEXT("editor_busy"), TEXT("A running project validator cannot reenter validation job mutation."));
    if (Action == TEXT("blueprint_inspect")) return JevProject::Blueprint(Params, Identity);
    if (Action == TEXT("asset_dependencies")) return JevProject::Dependencies(Params, Identity);
    if (Action == TEXT("asset_import_info")) return JevProject::ImportInfo(Params, Identity);
    if (Action == TEXT("validation_rules"))
    {
        if (!JevProject::Only(Params, {})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("validation_rules accepts no fields."));
        return ValidationRules(Identity);
    }
    if (Action == TEXT("validation_start")) return StartValidation(Params, Identity);
    if (Action == TEXT("validation_job") || Action == TEXT("validation_cancel"))
    {
        FString Id; FGuid Guid;
        if (!JevProject::Only(Params, {TEXT("job_id")}) || !JevProject::Text(Params, TEXT("job_id"), Id, 36) || !FGuid::ParseExact(Id, EGuidFormats::DigitsWithHyphens, Guid)) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("A canonical validation job UUID is required."));
        const auto* Found = Jobs.Find(Id);
        if (!Found || ((*Found)->Finished > 0 && FPlatformTime::Seconds() - (*Found)->Finished >= JevProject::RetentionSeconds)) return FJevEditorBridge::Error(TEXT("unknown_job"), TEXT("The validation job is unknown or expired."));
        if (!JevProject::SameIdentity((*Found)->Identity, Identity)) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("This job belongs to a different project, editor session or world."));
        if (Action == TEXT("validation_cancel") && ActiveJob == Id)
        {
            (*Found)->State = TEXT("cancelled"); (*Found)->StopReason = TEXT("requested"); (*Found)->Finished = FPlatformTime::Seconds(); ActiveJob.Empty();
        }
        return JobSnapshot(**Found);
    }
    return FJevEditorBridge::Error(TEXT("unknown_action"), TEXT("Unknown project tool action."));
}

void FJevProjectTools::Tick(const TSharedRef<FJsonObject>& Identity)
{
    check(IsInGameThread());
    if (ActiveJob.IsEmpty()) return;
    const auto Found = Jobs.FindRef(ActiveJob);
    if (!Found) { ActiveJob.Empty(); return; }
    FJob& Job = *Found;
    if (bExecutingCallbacks) return;
    TGuardValue<bool> CallbackGuard(bExecutingCallbacks, true);
    auto Finish = [&](const TCHAR* State, const TCHAR* Reason)
    {
        Job.State = State; Job.StopReason = Reason; Job.Finished = FPlatformTime::Seconds(); if (ActiveJob == Job.Id) ActiveJob.Empty();
    };
    if (!JevProject::SameIdentity(Job.Identity, Identity)) { Finish(TEXT("cancelled"), TEXT("identity_changed")); return; }
    if (Job.State == TEXT("queued") && Job.Identity->GetStringField(TEXT("revision")) != Identity->GetStringField(TEXT("revision"))) { Finish(TEXT("cancelled"), TEXT("revision_changed_before_start")); return; }
    bool bPIE = false, bSimulating = false;
    Identity->TryGetBoolField(TEXT("play_in_editor"), bPIE); Identity->TryGetBoolField(TEXT("simulating"), bSimulating);
    if (bPIE || bSimulating) { Finish(TEXT("cancelled"), TEXT("editor_playing")); return; }
    if (FPlatformTime::Seconds() - Job.Started >= Job.Timeout) { Finish(TEXT("timed_out"), TEXT("time_budget")); return; }
    bool bEnabled, bValid; double Timeout;
    const auto CurrentRules = JevProject::Rules(bEnabled, bValid, Timeout);
    if (!bEnabled || !bValid) { Finish(TEXT("cancelled"), TEXT("configuration_changed")); return; }
    // Finalize on a fresh tick so even the last project validator cannot silently
    // switch the editor world and leave a completed result bound to old identity.
    if (Job.Index >= Job.Assets.Num() * Job.Rules.Num()) { Finish(TEXT("completed"), TEXT("")); return; }
    const auto& Rule = Job.Rules[Job.Index % Job.Rules.Num()];
    if (!CurrentRules.ContainsByPredicate([&Rule](const JevProject::FRule& Current) { return Current.Id == Rule.Id && Current.ClassPath == Rule.ClassPath; })) { Finish(TEXT("cancelled"), TEXT("configuration_changed")); return; }
    UClass* Class = JevProject::RuleClass(Rule.ClassPath);
    if (!Class) { Finish(TEXT("failed"), TEXT("rule_unavailable")); return; }
    const FString& Path = Job.Assets[Job.Index / Job.Rules.Num()];
    const FAssetData Data = JevProject::RegistryAsset(Path);
    if (!Data.IsValid()) { Finish(TEXT("failed"), TEXT("asset_not_found")); return; }
    Job.State = TEXT("running");
    const bool bWasLoaded = Data.IsAssetLoaded();
    TStrongObjectPtr<UObject> Asset(Data.GetAsset());
    if (ActiveJob != Job.Id || Job.State != TEXT("running")) return;
    if (!Asset.IsValid() || Asset->GetPathName() != Path) { Finish(TEXT("failed"), TEXT("asset_load_failed")); return; }
    const bool bDirtyBefore = Asset->GetOutermost()->IsDirty();
    TStrongObjectPtr<UEditorValidatorBase> Validator(NewObject<UEditorValidatorBase>(GetTransientPackage(), Class));
    if (ActiveJob != Job.Id || Job.State != TEXT("running")) return;
    FDataValidationContext Context(!bWasLoaded, EDataValidationUsecase::Manual, {});
    const double Started = FPlatformTime::Seconds();
    const bool bValidatorEnabled = Validator.IsValid() && Validator->IsEnabled();
    const auto Outcome = bValidatorEnabled ? Validator->ValidateLoadedAsset(Data, Asset.Get(), Context) : EDataValidationResult::NotValidated;
    if (ActiveJob != Job.Id || Job.State != TEXT("running")) return;
    auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("asset_path"), Path);
    Row->SetStringField(TEXT("rule_id"), Rule.Id);
    Row->SetStringField(TEXT("validator_class"), Rule.ClassPath);
    Row->SetBoolField(TEXT("validator_enabled"), bValidatorEnabled);
    Row->SetStringField(TEXT("not_validated_reason"), Outcome != EDataValidationResult::NotValidated ? TEXT("") : bValidatorEnabled ? TEXT("not_applicable_or_no_verdict") : TEXT("validator_disabled"));
    Row->SetStringField(TEXT("result"), Outcome == EDataValidationResult::Invalid || Context.GetNumErrors() > 0 ? TEXT("invalid") : Outcome == EDataValidationResult::Valid ? TEXT("valid") : TEXT("not_validated"));
    Row->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
    Row->SetNumberField(TEXT("error_count"), Context.GetNumErrors());
    Row->SetNumberField(TEXT("warning_count"), Context.GetNumWarnings());
    Row->SetBoolField(TEXT("loaded_for_validation"), !bWasLoaded);
    Row->SetBoolField(TEXT("package_dirty_before"), bDirtyBefore);
    Row->SetBoolField(TEXT("package_dirty_after"), Asset->GetOutermost()->IsDirty());
    Row->SetBoolField(TEXT("package_dirty_changed"), bDirtyBefore != Asset->GetOutermost()->IsDirty());
    TArray<TSharedPtr<FJsonValue>> Messages;
    bool bTruncated = false;
    for (const auto& Issue : Context.GetIssues())
    {
        if (Messages.Num() >= 8 || Job.Messages >= JevProject::MaxMessages) { bTruncated = true; break; }
        auto Message = MakeShared<FJsonObject>();
        Message->SetStringField(TEXT("severity"), Issue.Severity == EMessageSeverity::Error ? TEXT("error") : Issue.Severity == EMessageSeverity::Warning || Issue.Severity == EMessageSeverity::PerformanceWarning ? TEXT("warning") : TEXT("info"));
        Message->SetStringField(TEXT("text"), JevProject::Clip(Issue.TokenizedMessage ? Issue.TokenizedMessage->ToText().ToString() : Issue.Message.ToString(), 512, bTruncated));
        Messages.Add(MakeShared<FJsonValueObject>(Message)); ++Job.Messages;
    }
    Row->SetArrayField(TEXT("messages"), Messages);
    Row->SetBoolField(TEXT("messages_truncated"), bTruncated);
    Job.bTruncated |= bTruncated;
    Job.Results.Add(MakeShared<FJsonValueObject>(Row)); ++Job.Index;
    if (FPlatformTime::Seconds() - Job.Started >= Job.Timeout) Finish(TEXT("timed_out"), TEXT("time_budget"));
}

void FJevProjectTools::Shutdown()
{
    if (const auto Job = Jobs.FindRef(ActiveJob))
    {
        Job->State = TEXT("cancelled"); Job->StopReason = TEXT("editor_shutdown"); Job->Finished = FPlatformTime::Seconds();
    }
    ActiveJob.Empty();
}
