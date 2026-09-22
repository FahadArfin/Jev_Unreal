#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorBridge.h"
#include "JevEditorReviewPanel.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Selection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"

namespace JevReviewTests
{
TSharedRef<FJsonObject> Call(FJevEditorBridge& Bridge, const TCHAR* Action, const FString& Json = TEXT("{}"))
{
    TSharedPtr<FJsonObject> Params;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Params);
    auto Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("action"), Action);
    Request->SetObjectField(TEXT("params"), Params);
    return Bridge.Execute(Request);
}

TSharedRef<FJsonObject> ById(FJevEditorBridge& Bridge, const TCHAR* Action, const FString& Id)
{
    return Call(Bridge, Action, FString::Printf(TEXT("{\"plan_id\":\"%s\"}"), *Id));
}

FString Preview(FJevEditorBridge& Bridge)
{
    const auto Response = Call(Bridge, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"HistoryCube\"}]}"));
    return Response->GetBoolField(TEXT("ok")) ? Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id")) : FString();
}

int32 Count(UWorld* World)
{
    int32 Count = 0;
    for (TActorIterator<AActor> It(World); It; ++It) ++Count;
    return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevNativePlanHistoryTest, "Jev.Editor.NativePlanHistory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevNativePlanHistoryTest::RunTest(const FString& Parameters)
{
    using namespace JevReviewTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated world"), World)) return false;
    double Now = 1000;
    FJevEditorBridge Bridge([&Now] { return Now; });
    for (const TCHAR* Invalid : {TEXT("{\"limit\":true}"), TEXT("{\"limit\":0}"), TEXT("{\"limit\":65}"), TEXT("{\"limit\":1.5}"), TEXT("{\"limit\":\"2\"}"), TEXT("{\"extra\":1}")})
        TestFalse(TEXT("Pending-list schema rejects invalid inputs"), Call(Bridge, TEXT("pending_plans"), Invalid)->GetBoolField(TEXT("ok")));
    for (const TCHAR* Invalid : {TEXT("{}"), TEXT("{\"plan_id\":true}"), TEXT("{\"plan_id\":\"\"}"), TEXT("{\"plan_id\":\"x\",\"extra\":1}")})
        TestFalse(TEXT("Plan status schema rejects invalid inputs"), Call(Bridge, TEXT("plan_status"), Invalid)->GetBoolField(TEXT("ok")));
    const int32 Before = Count(World);
    const FString Id = Preview(Bridge);
    if (!TestFalse(TEXT("Plan ID exists"), Id.IsEmpty())) return false;
    const auto Pending = ById(Bridge, TEXT("plan_status"), Id)->GetObjectField(TEXT("result"));
    TestEqual(TEXT("Native record pending"), Pending->GetStringField(TEXT("status")), FString(TEXT("pending")));
    TestEqual(TEXT("History scope is memory only"), Pending->GetStringField(TEXT("scope")), FString(TEXT("editor_session_memory")));
    TestFalse(TEXT("Preview was not executed"), Pending->GetBoolField(TEXT("executed")));
    TestEqual(TEXT("Review contains normalized operation"), Pending->GetObjectField(TEXT("review"))->GetArrayField(TEXT("operations")).Num(), 1);
    TestEqual(TEXT("New actor has explicit absent before state"), Pending->GetObjectField(TEXT("review"))->GetArrayField(TEXT("before"))[0]->Type, EJson::Null);
    TestEqual(TEXT("Listing and preview do not edit the scene"), Count(World), Before);
    TestEqual(TEXT("Pending summary count"), Call(Bridge, TEXT("pending_plans"))->GetObjectField(TEXT("result"))->GetNumberField(TEXT("pending_count")), 1.0);
    TestTrue(TEXT("Native apply succeeds"), ById(Bridge, TEXT("apply"), Id)->GetBoolField(TEXT("ok")));
    const auto Applied = ById(Bridge, TEXT("plan_status"), Id)->GetObjectField(TEXT("result"));
    TestEqual(TEXT("Applied outcome retained"), Applied->GetStringField(TEXT("status")), FString(TEXT("applied")));
    TestTrue(TEXT("Fresh verification explicitly required"), Applied->GetBoolField(TEXT("requires_fresh_verification")));
    TestFalse(TEXT("History does not claim saving"), Applied->GetBoolField(TEXT("saved")));
    TestEqual(TEXT("Changed actor paths retained"), Applied->GetArrayField(TEXT("actor_paths")).Num(), 1);
    TestFalse(TEXT("Replay fails"), ById(Bridge, TEXT("apply"), Id)->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Replay does not overwrite success history"), ById(Bridge, TEXT("plan_status"), Id)->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("applied")));
    TestTrue(TEXT("Unreal Undo succeeds"), GEditor->UndoTransaction());
    TestEqual(TEXT("History remains historical after Undo"), ById(Bridge, TEXT("plan_status"), Id)->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("applied")));

    const FString StaleId = Preview(Bridge);
    World->SpawnActor<AStaticMeshActor>();
    TestFalse(TEXT("Stale apply rejected"), ById(Bridge, TEXT("apply"), StaleId)->GetBoolField(TEXT("ok")));
    const auto Stale = ById(Bridge, TEXT("plan_status"), StaleId)->GetObjectField(TEXT("result"));
    TestEqual(TEXT("Stale outcome recorded"), Stale->GetStringField(TEXT("status")), FString(TEXT("rejected")));
    TestEqual(TEXT("Stale error code retained"), Stale->GetStringField(TEXT("outcome_code")), FString(TEXT("stale_plan")));
    TestFalse(TEXT("Rejected record confirms no execution"), Stale->GetBoolField(TEXT("executed")));

    const FString ExpiredId = Preview(Bridge);
    Now += 121;
    TestEqual(TEXT("Expired record remains inspectable"), ById(Bridge, TEXT("plan_status"), ExpiredId)->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("expired")));
    TestEqual(TEXT("Expired plans no longer listed"), Call(Bridge, TEXT("pending_plans"))->GetObjectField(TEXT("result"))->GetNumberField(TEXT("pending_count")), 0.0);
    TestFalse(TEXT("Expired record is not executable"), ById(Bridge, TEXT("apply"), ExpiredId)->GetBoolField(TEXT("ok")));

    const FString RollbackId = Preview(Bridge);
    const int32 BeforeRollback = Count(World);
    Bridge.FailApplyAfterOperationsForTesting(1);
    const auto RollbackResponse = ById(Bridge, TEXT("apply"), RollbackId);
    TestFalse(TEXT("Injected operation failure reported"), RollbackResponse->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Failed transaction restores actor count"), Count(World), BeforeRollback);
    TestEqual(TEXT("Verified rollback distinguished from unknown outcome"), ById(Bridge, TEXT("plan_status"), RollbackId)->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("rolled_back")));
    FJevEditorBridge OtherSession;
    TestFalse(TEXT("Another editor session cannot recover records"), ById(OtherSession, TEXT("plan_status"), Id)->GetBoolField(TEXT("ok")));
    Now += 901;
    TestFalse(TEXT("History retention expires"), ById(Bridge, TEXT("plan_status"), Id)->GetBoolField(TEXT("ok")));

    FJevEditorBridge Capacity([&Now] { return Now; });
    TArray<FString> PlanIds;
    for (int32 Index = 0; Index < 64; ++Index) PlanIds.Add(Preview(Capacity));
    TestFalse(TEXT("64th pending plan retained"), PlanIds.Last().IsEmpty());
    TestTrue(TEXT("65th pending plan rejected"), Preview(Capacity).IsEmpty());
    const auto Limited = Call(Capacity, TEXT("pending_plans"), TEXT("{\"limit\":2}"))->GetObjectField(TEXT("result"));
    TestEqual(TEXT("List is bounded"), Limited->GetArrayField(TEXT("plans")).Num(), 2);
    TestTrue(TEXT("Truncation explicit"), Limited->GetBoolField(TEXT("truncated")));
    TestTrue(TEXT("Oldest plan applies"), ById(Capacity, TEXT("apply"), PlanIds[0])->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Completed-first eviction admits new plan"), Preview(Capacity).IsEmpty());
    TestFalse(TEXT("Oldest completed record evicted"), ById(Capacity, TEXT("plan_status"), PlanIds[0])->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Pending records preserved during eviction"), ById(Capacity, TEXT("plan_status"), PlanIds[1])->GetBoolField(TEXT("ok")));

    FJevEditorBridge Reentrant([&Now] { return Now; });
    TArray<FString> ReentrantIds;
    for (int32 Index = 0; Index < 64; ++Index) ReentrantIds.Add(Preview(Reentrant));
    const FString InFlightId = ReentrantIds[0];
    Reentrant.OnApplyConsumedForTesting([&]
    {
        const auto InFlight = ById(Reentrant, TEXT("plan_status"), InFlightId);
        TestTrue(TEXT("Consumed in-flight plan remains inspectable"), InFlight->GetBoolField(TEXT("ok")));
        if (InFlight->GetBoolField(TEXT("ok")))
        {
            const auto Record = InFlight->GetObjectField(TEXT("result"));
            TestEqual(TEXT("In-flight state is explicit"), Record->GetStringField(TEXT("status")), FString(TEXT("applying")));
            TestTrue(TEXT("In-flight execution outcome is unknown"), Record->TryGetField(TEXT("executed"))->Type == EJson::Null);
        }
        const auto NestedPreview = Call(Reentrant, TEXT("preview"), TEXT("{\"operations\":[{\"op\":\"spawn_primitive\",\"shape\":\"Cube\",\"label\":\"Nested\"}]}"));
        TestEqual(TEXT("Nested preview cannot evict applying record"), NestedPreview->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("editor_busy")));
        const auto NestedApply = ById(Reentrant, TEXT("apply"), ReentrantIds[1]);
        TestEqual(TEXT("Nested apply is blocked without consuming another plan"), NestedApply->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")), FString(TEXT("editor_busy")));
        TestEqual(TEXT("Other plan remains pending"), ById(Reentrant, TEXT("plan_status"), ReentrantIds[1])->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("pending")));
        const double OriginalNow = Now;
        Now += 901;
        TestTrue(TEXT("Retention pruning cannot erase applying record"), ById(Reentrant, TEXT("plan_status"), InFlightId)->GetBoolField(TEXT("ok")));
        Now = OriginalNow;
    });
    TestTrue(TEXT("Original apply survives nested reads and blocked mutations"), ById(Reentrant, TEXT("apply"), InFlightId)->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Original completion updates the retained record"), ById(Reentrant, TEXT("plan_status"), InFlightId)->GetObjectField(TEXT("result"))->GetStringField(TEXT("status")), FString(TEXT("applied")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewSelectionTest, "Jev.Editor.ReviewSelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewSelectionTest::RunTest(const FString& Parameters)
{
    using namespace JevReviewTests;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Isolated world"), World)) return false;
    FJevEditorBridge Bridge;
    GEditor->SelectNone(false, true, false);
    ON_SCOPE_EXIT { GEditor->SelectNone(false, true, false); };
    TestFalse(TEXT("Empty selection rejected"), FJevEditorReviewPanel::InspectSelection(Bridge)->GetBoolField(TEXT("ok")));
    AStaticMeshActor* First = World->SpawnActor<AStaticMeshActor>();
    First->SetActorLocation(FVector(100, 200, 300));
    First->SetActorLabel(TEXT("Original label"));
    GEditor->SelectActor(First, true, false);
    const auto Details = FJevEditorReviewPanel::InspectSelection(Bridge);
    TestTrue(TEXT("Selected actor inspected without Slate"), Details->GetBoolField(TEXT("ok")));
    const FVector Delta(10, -20, 30);
    const auto Previewed = FJevEditorReviewPanel::PreviewSelection(Bridge, &Delta, nullptr, nullptr);
    if (!TestTrue(TEXT("Selection translation previews"), Previewed->GetBoolField(TEXT("ok")))) return false;
    const FString Id = Previewed->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"));
    TestTrue(TEXT("Preview does not translate actor"), First->GetActorLocation().Equals(FVector(100, 200, 300)));
    const auto Record = ById(Bridge, TEXT("plan_status"), Id)->GetObjectField(TEXT("result"));
    const FString Review = FJevEditorReviewPanel::DescribeReview(Record);
    TestTrue(TEXT("Human review includes before state"), Review.Contains(TEXT("100.00, 200.00, 300.00")));
    TestTrue(TEXT("Human review includes after state"), Review.Contains(TEXT("110.00, 180.00, 330.00")));
    TestTrue(TEXT("Panel plans use the same native apply endpoint"), ById(Bridge, TEXT("apply"), Id)->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Exact translation applied"), First->GetActorLocation().Equals(FVector(110, 180, 330)));

    const FString Label(TEXT("Human reviewed label")), Folder(TEXT("Jev/HumanReview"));
    const auto Metadata = FJevEditorReviewPanel::PreviewSelection(Bridge, nullptr, &Label, &Folder);
    if (!TestTrue(TEXT("Label/folder previews"), Metadata->GetBoolField(TEXT("ok")))) return false;
    const FString MetadataId = Metadata->GetObjectField(TEXT("result"))->GetStringField(TEXT("plan_id"));
    TestTrue(TEXT("Metadata applies through same path"), ById(Bridge, TEXT("apply"), MetadataId)->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Label applied"), First->GetActorLabel(), Label);
    TestEqual(TEXT("Folder applied"), First->GetFolderPath().ToString(), Folder);
    TestTrue(TEXT("Human metadata supports Undo"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores original label"), First->GetActorLabel(), FString(TEXT("Original label")));
    TestFalse(TEXT("Mixed transform/metadata rejected"), FJevEditorReviewPanel::PreviewSelection(Bridge, &Delta, &Label, nullptr)->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("No edit fields rejected"), FJevEditorReviewPanel::PreviewSelection(Bridge, nullptr, nullptr, nullptr)->GetBoolField(TEXT("ok")));
    const FVector Excess(1000001, 0, 0);
    TestFalse(TEXT("Out-of-bounds delta rejected"), FJevEditorReviewPanel::PreviewSelection(Bridge, &Excess, nullptr, nullptr)->GetBoolField(TEXT("ok")));
    AStaticMeshActor* Second = World->SpawnActor<AStaticMeshActor>();
    GEditor->SelectActor(Second, true, false);
    TestFalse(TEXT("Multi-actor label edit rejected"), FJevEditorReviewPanel::PreviewSelection(Bridge, nullptr, &Label, nullptr)->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Multi-actor folder preview supported"), FJevEditorReviewPanel::PreviewSelection(Bridge, nullptr, nullptr, &Folder)->GetBoolField(TEXT("ok")));
    AActor* Unsupported = World->SpawnActor<AActor>();
    GEditor->SelectActor(Unsupported, true, false);
    TestTrue(TEXT("Unsupported selection still inspectable"), FJevEditorReviewPanel::InspectSelection(Bridge)->GetBoolField(TEXT("ok")));
    TestFalse(TEXT("Unsupported actor blocks whole preview"), FJevEditorReviewPanel::PreviewSelection(Bridge, &Delta, nullptr, nullptr)->GetBoolField(TEXT("ok")));
    GEditor->SelectNone(false, true, false);
    for (int32 Index = 0; Index < 21; ++Index) GEditor->SelectActor(World->SpawnActor<AStaticMeshActor>(), true, false);
    TestFalse(TEXT("Selection over 20 rejects instead of silently truncating"), FJevEditorReviewPanel::InspectSelection(Bridge)->GetBoolField(TEXT("ok")));
    if (!FApp::CanEverRender())
    {
        FJevEditorReviewPanel Panel;
        Panel.SetBridge(&Bridge);
        Panel.Register();
        TestFalse(TEXT("NullRHI does not register or construct a review tab"), FGlobalTabmanager::Get()->HasTabSpawner(FName(TEXT("JevReview"))));
        Panel.Unregister();
    }
    return true;
}

#endif
