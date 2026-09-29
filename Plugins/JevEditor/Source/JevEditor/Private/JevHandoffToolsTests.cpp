#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorHandoffTools.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace JevHandoffTests
{
const TCHAR* Section = TEXT("JevEditor.Handoff");

TSharedRef<FJsonObject> Identity()
{
    auto Value = MakeShared<FJsonObject>();
    Value->SetStringField(TEXT("project_file"), TEXT("/fixture/JevSandbox.uproject"));
    Value->SetStringField(TEXT("session_id"), TEXT("handoff-fixture"));
    Value->SetStringField(TEXT("world_path"), TEXT("/Game/FixtureWorld"));
    Value->SetStringField(TEXT("revision"), TEXT("fixture-revision"));
    Value->SetBoolField(TEXT("play_in_editor"), false);
    Value->SetBoolField(TEXT("simulating"), false);
    return Value;
}

TSharedRef<FJsonObject> Preview()
{
    auto Params = MakeShared<FJsonObject>();
    auto State = MakeShared<FJsonObject>();
    const auto Current = Identity();
    for (const TCHAR* Key : {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})
        State->SetStringField(Key, Current->GetStringField(Key));
    Params->SetStringField(TEXT("alias"), TEXT("fixture"));
    Params->SetStringField(TEXT("operation"), TEXT("import"));
    Params->SetStringField(TEXT("expected_project"), Current->GetStringField(TEXT("project_file")));
    Params->SetObjectField(TEXT("expected_state"), State);
    return Params;
}

TSharedRef<FJsonObject> Apply(const TSharedRef<FJsonObject>& Plan)
{
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("plan_id"), Plan->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")));
    Params->SetStringField(TEXT("expected_project"), Identity()->GetStringField(TEXT("project_file")));
    return Params;
}

FString ErrorCode(const TSharedRef<FJsonObject>& Value)
{
    return Value->GetBoolField(TEXT("ok")) ? TEXT("unexpected_success") : Value->GetObjectField(TEXT("error"))->GetStringField(TEXT("code"));
}

struct FFixture
{
    bool bHadEnabled = false, bEnabled = false, bHadBundles = false, bReady = false;
    TArray<FString> Bundles;
    FString Source, Hash, Asset;
    TArray<uint8> Bytes;
    FFixture()
    {
        bHadEnabled = GConfig->GetBool(Section, TEXT("bEnabled"), bEnabled, GGameIni);
        bHadBundles = GConfig->GetArray(Section, TEXT("Bundles"), Bundles, GGameIni) > 0;
        const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("JevHandoffTests"));
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Source = Directory / (Id + TEXT(".fbx"));
        Asset = TEXT("/Game/JevHandoffFixture_") + Id + TEXT(".JevHandoffFixture_") + Id;
        // Header-only fixture deliberately cannot become imported geometry. Tests never enter import.
        const ANSICHAR* Header = "Kaydara FBX Binary  ";
        Bytes.Append(reinterpret_cast<const uint8*>(Header), 19);
        Bytes.AddZeroed(13);
        Hash = FJevHandoffTools::SourceSha256(Bytes);
        bReady = IFileManager::Get().MakeDirectory(*Directory, true) &&
            FFileHelper::SaveArrayToFile(Bytes, *Source) &&
            Hash.Len() == 64;
        GConfig->SetBool(Section, TEXT("bEnabled"), true, GGameIni);
        Approve();
    }
    FString Entry() const { return TEXT("fixture|") + Source + TEXT("|") + Hash + TEXT("|") + Asset; }
    void Approve() const { GConfig->SetArray(Section, TEXT("Bundles"), {Entry()}, GGameIni); }
    ~FFixture()
    {
        if (bHadEnabled) GConfig->SetBool(Section, TEXT("bEnabled"), bEnabled, GGameIni);
        else GConfig->RemoveKey(Section, TEXT("bEnabled"), GGameIni);
        if (bHadBundles) GConfig->SetArray(Section, TEXT("Bundles"), Bundles, GGameIni);
        else GConfig->RemoveKey(Section, TEXT("Bundles"), GGameIni);
        // Delete only our exact GUID-named synthetic file, never a directory or project asset.
        if (!Source.IsEmpty()) IFileManager::Get().Delete(*Source);
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevHandoffPolicyGuards, "Jev.Editor.HandoffPolicyGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevHandoffPolicyGuards::RunTest(const FString&)
{
    using namespace JevHandoffTests;
    FFixture Fixture; FJevHandoffTools Tools;
    if (!TestTrue(TEXT("synthetic pinned source ready"), Fixture.bReady)) return false;
    const auto Manifest = Tools.Execute(TEXT("handoff_manifest"), MakeShared<FJsonObject>(), Identity());
    if (!TestTrue(TEXT("configured alias discovery"), Manifest->GetBoolField(TEXT("ok")))) return false;
    const auto Row = Manifest->GetObjectField(TEXT("result"))->GetArrayField(TEXT("aliases"))[0]->AsObject();
    TestEqual(TEXT("manifest exposes approved alias"), Row->GetStringField(TEXT("alias")), FString(TEXT("fixture")));
    TestFalse(TEXT("manifest never exposes source filesystem path"), Row->HasField(TEXT("source")));
    auto Params = Preview(); Params->SetStringField(TEXT("alias"), TEXT("unapproved"));
    TestEqual(TEXT("unknown alias refused"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Params, Identity())), FString(TEXT("target_not_allowed")));
    Params = Preview(); Params->SetStringField(TEXT("expected_project"), TEXT("/other/Game.uproject"));
    TestEqual(TEXT("wrong explicit project refused"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Params, Identity())), FString(TEXT("wrong_project")));
    Params = Preview(); Params->GetObjectField(TEXT("expected_state"))->SetStringField(TEXT("revision"), TEXT("old"));
    TestEqual(TEXT("stale expected state refused"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Params, Identity())), FString(TEXT("stale_plan")));
    Params = Preview(); Params->SetStringField(TEXT("source"), Fixture.Source);
    TestEqual(TEXT("request cannot inject source paths"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Params, Identity())), FString(TEXT("bad_request")));
    Params = Preview(); Params->SetStringField(TEXT("operation"), TEXT("reimport"));
    TestEqual(TEXT("reimport requires existing clean loaded target"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Params, Identity())), FString(TEXT("asset_unavailable")));
    auto Playing = Identity(); Playing->SetBoolField(TEXT("play_in_editor"), true);
    TestEqual(TEXT("PIE refused"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Preview(), Playing)), FString(TEXT("play_mode")));
    GConfig->SetBool(Section, TEXT("bEnabled"), false, GGameIni);
    TestEqual(TEXT("disabled project import policy refused"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Preview(), Identity())), FString(TEXT("handoff_disabled")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevHandoffPolicyValidation, "Jev.Editor.HandoffPolicyValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevHandoffPolicyValidation::RunTest(const FString&)
{
    using namespace JevHandoffTests;
    FFixture Fixture; FJevHandoffTools Tools;
    if (!TestTrue(TEXT("synthetic source ready"), Fixture.bReady)) return false;
    const TArray<FString> Invalid = {
        TEXT("fixture|relative.fbx|") + Fixture.Hash + TEXT("|") + Fixture.Asset,
        TEXT("fixture|//server/share/source.fbx|") + Fixture.Hash + TEXT("|") + Fixture.Asset,
        TEXT("fixture|C:/safe/../source.fbx|") + Fixture.Hash + TEXT("|") + Fixture.Asset,
        TEXT("fixture|") + Fixture.Source + TEXT("|not-a-hash|") + Fixture.Asset,
        TEXT("fixture|") + Fixture.Source + TEXT("|") + Fixture.Hash + TEXT("|/Engine/Forbidden.Forbidden"),
        TEXT("bad.alias|") + Fixture.Source + TEXT("|") + Fixture.Hash + TEXT("|") + Fixture.Asset,
    };
    for (const FString& Entry : Invalid)
    {
        GConfig->SetArray(Section, TEXT("Bundles"), {Entry}, GGameIni);
        TestEqual(TEXT("malformed/escaping policy rejected"), ErrorCode(Tools.Execute(TEXT("handoff_manifest"), MakeShared<FJsonObject>(), Identity())), FString(TEXT("policy_invalid")));
    }
    GConfig->SetArray(Section, TEXT("Bundles"), {Fixture.Entry(), Fixture.Entry()}, GGameIni);
    TestEqual(TEXT("duplicate alias rejected"), ErrorCode(Tools.Execute(TEXT("handoff_manifest"), MakeShared<FJsonObject>(), Identity())), FString(TEXT("policy_invalid")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevHandoffOneShotGuards, "Jev.Editor.HandoffOneShotGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevHandoffOneShotGuards::RunTest(const FString&)
{
    using namespace JevHandoffTests;
    FFixture Fixture; double Now = 0; FJevHandoffTools Tools([&Now] { return Now; });
    if (!TestTrue(TEXT("synthetic source ready"), Fixture.bReady)) return false;
    auto Plan = Tools.Execute(TEXT("handoff_preview"), Preview(), Identity());
    if (!TestTrue(TEXT("review accepts pinned binary header"), Plan->GetBoolField(TEXT("ok")))) return false;
    TestFalse(TEXT("preview never imports"), Tools.HasActiveJob());
    auto Stale = Identity(); Stale->SetStringField(TEXT("revision"), TEXT("changed"));
    auto Params = Apply(Plan);
    TestEqual(TEXT("changed editor state consumes without importing"), ErrorCode(Tools.Execute(TEXT("handoff_apply"), Params, Stale)), FString(TEXT("stale_plan")));
    TestEqual(TEXT("stale attempt cannot replay"), ErrorCode(Tools.Execute(TEXT("handoff_apply"), Params, Identity())), FString(TEXT("unknown_plan")));

    Plan = Tools.Execute(TEXT("handoff_preview"), Preview(), Identity());
    if (!TestTrue(TEXT("second review"), Plan->GetBoolField(TEXT("ok")))) return false;
    Params = Apply(Plan); Now = 120;
    // Independent source mismatch keeps this test out of the importer even if expiry regresses.
    Fixture.Bytes.Last() = 2;
    if (!TestTrue(TEXT("secondary expiry safety guard"), FFileHelper::SaveArrayToFile(Fixture.Bytes, *Fixture.Source))) return false;
    TestEqual(TEXT("exact advertised expiry boundary refuses"), ErrorCode(Tools.Execute(TEXT("handoff_apply"), Params, Identity())), FString(TEXT("unknown_plan")));
    Fixture.Bytes.Last() = 0;
    if (!TestTrue(TEXT("restore synthetic source"), FFileHelper::SaveArrayToFile(Fixture.Bytes, *Fixture.Source))) return false;

    Plan = Tools.Execute(TEXT("handoff_preview"), Preview(), Identity());
    if (!TestTrue(TEXT("third review"), Plan->GetBoolField(TEXT("ok")))) return false;
    Params = Apply(Plan); Fixture.Bytes.Last() = 1;
    if (!TestTrue(TEXT("change only approved synthetic file"), FFileHelper::SaveArrayToFile(Fixture.Bytes, *Fixture.Source))) return false;
    TestEqual(TEXT("changed source rejected before staging/import"), ErrorCode(Tools.Execute(TEXT("handoff_apply"), Params, Identity())), FString(TEXT("source_changed")));
    TestEqual(TEXT("source mismatch attempt consumed"), ErrorCode(Tools.Execute(TEXT("handoff_apply"), Params, Identity())), FString(TEXT("unknown_plan")));
    TestEqual(TEXT("changed bytes rejected for new preview"), ErrorCode(Tools.Execute(TEXT("handoff_preview"), Preview(), Identity())), FString(TEXT("source_changed")));
    TestFalse(TEXT("guard tests never invoked importer"), Tools.HasActiveJob());
    return true;
}

#endif
