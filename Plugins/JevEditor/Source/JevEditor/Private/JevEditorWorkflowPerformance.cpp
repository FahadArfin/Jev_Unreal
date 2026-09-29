#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "RenderTimer.h"
#include "RHI.h"

struct FJevWorkflowTools::FSample
{
    TSharedPtr<FJsonObject> Identity, Receipt;
    double CreatedAt = 0, LastTickAt = 0;
    int32 Requested = 0;
    bool Active = true;
    TArray<double> Intervals;
    uint64 LastEngineFrame = MAX_uint64;
    TArray<double> GameTimes, RenderTimes, RHITimes, GPUTimes;
    TArray<TSharedPtr<FJsonValue>> EngineObservations;
};

namespace
{
TSharedRef<FJsonObject> Distribution(TArray<double> Values)
{
    auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("count"), Values.Num());
    if (Values.IsEmpty()) return R;
    double Total = 0; for (double V : Values) Total += V; Values.Sort();
    R->SetNumberField(TEXT("mean_ms"), Total / Values.Num()); R->SetNumberField(TEXT("minimum_ms"), Values[0]); R->SetNumberField(TEXT("maximum_ms"), Values.Last());
    R->SetNumberField(TEXT("p50_ms"), Values[FMath::Max(0, FMath::CeilToInt(0.5 * Values.Num()) - 1)]); R->SetNumberField(TEXT("p95_ms"), Values[FMath::Max(0, FMath::CeilToInt(0.95 * Values.Num()) - 1)]);
    return R;
}
}

bool FJevWorkflowTools::HasActiveJob() const
{
    if (bApplying) return true;
    for (const auto& Pair : Samples) if (Pair.Value->Active) return true;
    return false;
}
void FJevWorkflowTools::Tick(const TSharedRef<FJsonObject>& I, double)
{
    const double Now = Clock();
    for (auto& Pair : Samples)
    {
        auto& S = Pair.Value; if (!S->Active) continue;
        if (S->Identity->GetStringField(TEXT("project_file")) != I->GetStringField(TEXT("project_file")) || !JevWorkflow::Same(S->Identity, I) || !JevWorkflow::Editing(I)) { S->Active = false; S->Receipt->SetStringField(TEXT("status"), TEXT("state_changed")); }
        else if (Now - S->CreatedAt >= 60) { S->Active = false; S->Receipt->SetStringField(TEXT("status"), TEXT("timed_out")); }
        else
        {
            // Exclude the partial start interval. Sample wall time between successive
            // game-thread ticker visits, not the API-provided DeltaTime or an FPS guess.
            if (S->LastTickAt > 0 && Now > S->LastTickAt) S->Intervals.Add((Now - S->LastTickAt) * 1000);
            S->LastTickAt = Now;
            // Engine-published timing counters are deliberately kept separate from
            // our ticker clock. Their production frames are not exposed by these
            // APIs, so observation-frame IDs must never be presented as aligned
            // game/render/GPU frame IDs.
            if (S->LastEngineFrame != GFrameCounter)
            {
                S->LastEngineFrame = GFrameCounter; auto Observation = MakeShared<FJsonObject>(); Observation->SetStringField(TEXT("observation_game_frame"), LexToString(GFrameCounter));
                const auto Add = [&Observation](const TCHAR* Name, uint32 Cycles, TArray<double>& Values)
                {
                    if (Cycles > 0) { const double Ms = FPlatformTime::ToMilliseconds(Cycles); Values.Add(Ms); Observation->SetNumberField(Name, Ms); }
                    else Observation->SetField(Name, MakeShared<FJsonValueNull>());
                };
                Add(TEXT("game_thread_ms"), GGameThreadTime, S->GameTimes); Add(TEXT("render_thread_ms"), GRenderThreadTime, S->RenderTimes); Add(TEXT("rhi_thread_ms"), GRHIThreadTime, S->RHITimes);
                Add(TEXT("gpu_ms"), GDynamicRHI && !GUsingNullRHI ? RHIGetGPUFrameCycles(0) : 0, S->GPUTimes); S->EngineObservations.Add(MakeShared<FJsonValueObject>(Observation));
            }
            if (S->Intervals.Num() >= S->Requested) { S->Active = false; S->Receipt->SetStringField(TEXT("status"), TEXT("completed")); }
        }
        S->Receipt->SetObjectField(TEXT("editor_tick_interval"), Distribution(S->Intervals));
        auto Engine = MakeShared<FJsonObject>(); Engine->SetObjectField(TEXT("game_thread"), Distribution(S->GameTimes)); Engine->SetObjectField(TEXT("render_thread"), Distribution(S->RenderTimes)); Engine->SetObjectField(TEXT("rhi_thread"), Distribution(S->RHITimes)); Engine->SetObjectField(TEXT("gpu"), Distribution(S->GPUTimes));
        Engine->SetBoolField(TEXT("frame_aligned"), false); Engine->SetBoolField(TEXT("gpu_available"), !S->GPUTimes.IsEmpty()); Engine->SetArrayField(TEXT("observations"), S->EngineObservations); Engine->SetStringField(TEXT("source"), TEXT("RenderTimer published game/render/RHI cycle counters and RHIGetGPUFrameCycles GPU0, observed at most once per game frame. Zero counters are unavailable, not zero cost. Counters may lag, repeat and refer to different production frames; no synchronized attribution or per-viewport isolation."));
        S->Receipt->SetObjectField(TEXT("engine_published_timings"), Engine);
        S->Receipt->SetNumberField(TEXT("elapsed_seconds"), FMath::Max(0.0, Now - S->CreatedAt));
        if (!S->Active) S->Receipt->SetNumberField(TEXT("process_physical_bytes_after"), static_cast<double>(FPlatformMemory::GetStats().UsedPhysical));
    }
}
TSharedRef<FJsonObject> FJevWorkflowTools::Performance(const FString& A, const TSharedPtr<FJsonObject>& P, const TSharedRef<FJsonObject>& I)
{
    using namespace JevWorkflow;
    for (auto It = Samples.CreateIterator(); It; ++It) if (!It->Value->Active && Clock() - It->Value->CreatedAt > 900) It.RemoveCurrent();
    if (A == TEXT("performance_start"))
    {
        FString Project, Protocol; double Count = 0; const TSharedPtr<FJsonObject>* State = nullptr;
        if (!Only(P, {TEXT("expected_project"), TEXT("expected_state"), TEXT("sample_count"), TEXT("protocol_id")}) || !Text(P, TEXT("expected_project"), Project, 4096) || !Text(P, TEXT("protocol_id"), Protocol, 64) || !Number(P, TEXT("sample_count"), Count, 10, 600) || Count != FMath::FloorToDouble(Count) || !P->TryGetObjectField(TEXT("expected_state"), State) || !Only(*State, {TEXT("session_id"), TEXT("world_path"), TEXT("revision")})) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply exact project/state, a protocol identifier and 10..600 samples."));
        for (TCHAR C : Protocol) if (!(FChar::IsAlnum(C) || C == '-' || C == '_')) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Protocol id uses letters, digits, hyphen or underscore."));
        if (Project != I->GetStringField(TEXT("project_file"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Project differs."));
        if (!Same(*State, I)) return FJevEditorBridge::Error(TEXT("stale_plan"), TEXT("Inspect the current editor first."));
        if (!Editing(I)) return FJevEditorBridge::Error(TEXT("play_mode"), TEXT("This sampler measures editor ticks only."));
        if (HasActiveJob()) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("A measurement is already running."));
        if (Samples.Num() >= 16) return FJevEditorBridge::Error(TEXT("job_busy"), TEXT("Bounded measurement receipts are full; retention is 15 minutes."));
        auto S = MakeShared<FSample>(); S->Identity = Base(I); S->CreatedAt = Clock(); S->Requested = static_cast<int32>(Count); S->Receipt = Base(I);
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); S->Receipt->SetStringField(TEXT("job_id"), Id); S->Receipt->SetStringField(TEXT("protocol_id"), Protocol); S->Receipt->SetStringField(TEXT("status"), TEXT("running")); S->Receipt->SetNumberField(TEXT("requested_samples"), Count);
        S->Receipt->SetNumberField(TEXT("process_physical_bytes_before"), static_cast<double>(FPlatformMemory::GetStats().UsedPhysical));
        S->Receipt->SetObjectField(TEXT("editor_tick_interval"), Distribution({})); S->Receipt->SetStringField(TEXT("metric"), TEXT("editor_game_thread_ticker_wall_interval_ms"));
        S->Receipt->SetStringField(TEXT("scope"), TEXT("Ticker intervals include editor idle/throttle/vsync, UI, bridge polling and background work. Additional engine-published game/render/RHI/GPU timing distributions are asynchronous observations, not frame-aligned measurements; absent/zero counters are unavailable. Process memory includes the entire editor. No packaged FPS, per-asset attribution or bottleneck proof. Hold workload/settings constant; the protocol name does not freeze or verify them."));
        Samples.Add(Id, S); return Success(S->Receipt.ToSharedRef());
    }
    FString Id;
    const bool Cancel = A == TEXT("performance_cancel"); FString Project;
    if (!Only(P, Cancel ? TArray<FString>{TEXT("job_id"), TEXT("expected_project")} : TArray<FString>{TEXT("job_id")}) || !Text(P, TEXT("job_id"), Id, 64) || (Cancel && (!Text(P, TEXT("expected_project"), Project, 4096) || Project != I->GetStringField(TEXT("project_file"))))) return FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Supply one owned job id and project for cancellation."));
    auto* Found = Samples.Find(Id); if (!Found) return FJevEditorBridge::Error(TEXT("unknown_job"), TEXT("Unknown measurement."));
    const auto S = *Found;
    if (S->Identity->GetStringField(TEXT("project_file")) != I->GetStringField(TEXT("project_file")) || S->Identity->GetStringField(TEXT("session_id")) != I->GetStringField(TEXT("session_id"))) return FJevEditorBridge::Error(TEXT("wrong_project"), TEXT("Measurement belongs to another project/session."));
    if (Cancel && S->Active) { S->Active = false; S->Receipt->SetStringField(TEXT("status"), TEXT("cancelled")); }
    return Success(S->Receipt.ToSharedRef());
}
