#include "JevEditorBridge.h"

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogJevEditor, Log, All);

namespace
{
constexpr uint32 BridgePort = 9845;
constexpr int32 MaximumBodyBytes = 65536;

const TArray<FString>* Header(const FHttpServerRequest& Request, const FString& Name)
{
    for (const auto& Pair : Request.Headers)
        if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase)) return &Pair.Value;
    return nullptr;
}

bool EqualSecret(const FString& Actual, const FString& Expected)
{
    // Do not short-circuit on the contents of the secret.
    uint32 Difference = Actual.Len() ^ Expected.Len();
    for (int32 I = 0; I < Expected.Len(); ++I)
        Difference |= static_cast<uint32>((I < Actual.Len() ? Actual[I] : 0) ^ Expected[I]);
    return Difference == 0;
}

void Reply(const FHttpResultCallback& Complete, const TSharedRef<FJsonObject>& Object, int32 Status = 200)
{
    FString Body;
    const auto Writer = TJsonWriterFactory<>::Create(&Body);
    FJsonSerializer::Serialize(Object, Writer);
    auto Response = FHttpServerResponse::Create(Body, TEXT("application/json; charset=utf-8"));
    Response->Code = static_cast<EHttpServerResponseCodes>(Status);
    Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
    Response->Headers.Add(TEXT("X-Content-Type-Options"), {TEXT("nosniff")});
    Complete(MoveTemp(Response));
}
}

class FJevEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        Token = FPlatformMisc::GetEnvironmentVariable(TEXT("JEV_BRIDGE_TOKEN"));
        bool bValidToken = Token.Len() >= 32 && Token.Len() <= 256;
        for (TCHAR Character : Token) bValidToken &= Character >= 33 && Character <= 126;
        if (!bValidToken)
        {
            UE_LOG(LogJevEditor, Display, TEXT("Bridge disabled: set JEV_BRIDGE_TOKEN to 32-256 printable non-space ASCII characters before launching the editor."));
            Token.Empty();
            return;
        }

        // Fail closed if anyone already owns this port. Reusing a router could attach our
        // authenticated API to a wildcard listener created by another editor plugin.
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!Sockets) return;
        {
            auto Probe = Sockets->CreateUniqueSocket(NAME_Stream, TEXT("JevLoopbackPortProbe"));
            const auto Address = Sockets->CreateInternetAddr();
            bool bValidIp = false;
            Address->SetIp(TEXT("127.0.0.1"), bValidIp);
            Address->SetPort(BridgePort);
            if (!Probe || !bValidIp || !Probe->Bind(*Address))
            {
                UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: loopback port 9845 is already occupied or unavailable."));
                return;
            }
        }

        // HTTPServer reads per-port ListenerOverrides when it binds. Prepend our explicit
        // override and invalidate its cache; do not change any other listener's address.
        // This is an in-memory engine config override and is never flushed to disk.
        TArray<FString> Overrides;
        GConfig->GetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni);
        Overrides.Insert(TEXT("(Port=9845,BindAddress=127.0.0.1,ReuseAddressAndPort=false,MaxConnectionsAcceptPerFrame=4,ConnectionsBacklogSize=16)"), 0);
        GConfig->SetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni);
        TSet<FString> Sections = { TEXT("HTTPServer.Listeners") };
        FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, Sections);

        FHttpServerModule& Server = FHttpServerModule::Get();
        Server.StartAllListeners();
        Router = Server.GetHttpRouter(BridgePort, true);
        if (!Router)
        {
            UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: could not create the loopback listener."));
            return;
        }
        Bridge = MakeUnique<FJevEditorBridge>();
        Route = Router->BindRoute(FHttpPath(TEXT("/jev/v1/call")), EHttpServerRequestVerbs::VERB_POST,
            FHttpRequestHandler::CreateRaw(this, &FJevEditorModule::HandleRequest));
        if (!Route)
        {
            Bridge.Reset();
            UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: its route is already occupied."));
            return;
        }
        UE_LOG(LogJevEditor, Display, TEXT("Jev editor bridge listening at http://127.0.0.1:9845/jev/v1/call (authentication required)."));
    }

    virtual void ShutdownModule() override
    {
        if (Router && Route) Router->UnbindRoute(Route);
        Route.Reset();
        Router.Reset();
        Bridge.Reset();
        Token.Empty();
        // HTTPServer is shared. Never call StopAllListeners from this plugin.
    }

private:
    bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        if (!IsInGameThread() || !Bridge)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("editor_unavailable"), TEXT("Editor bridge is unavailable.")), 503);
            return true;
        }
        if (!Request.PeerAddress || Request.PeerAddress->ToString(false) != TEXT("127.0.0.1") || Header(Request, TEXT("Origin")))
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("forbidden"), TEXT("Only local non-browser clients are accepted.")), 403);
            return true;
        }
        const auto* Host = Header(Request, TEXT("Host"));
        if (!Host || Host->Num() != 1 || (*Host)[0] != TEXT("127.0.0.1:9845"))
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("forbidden"), TEXT("The Host header must identify the loopback bridge.")), 403);
            return true;
        }
        const auto* Authorization = Header(Request, TEXT("Authorization"));
        if (!Authorization || Authorization->Num() != 1 || !EqualSecret((*Authorization)[0], TEXT("Bearer ") + Token))
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("unauthorized"), TEXT("A valid bridge bearer token is required.")), 401);
            return true;
        }
        const auto* ContentType = Header(Request, TEXT("Content-Type"));
        if (!ContentType || ContentType->Num() != 1 || !(*ContentType)[0].StartsWith(TEXT("application/json"), ESearchCase::IgnoreCase) || !Request.QueryParams.IsEmpty())
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Send application/json without URL query parameters.")), 400);
            return true;
        }
        if (Request.Body.Num() == 0 || Request.Body.Num() > MaximumBodyBytes)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Request body must contain 1 to 65536 bytes.")), 413);
            return true;
        }
        const double Now = FPlatformTime::Seconds();
        if (Now - RateWindow >= 1.0) { RateWindow = Now; RequestsInWindow = 0; }
        if (++RequestsInWindow > 30)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("rate_limited"), TEXT("At most 30 authenticated requests per second are accepted.")), 429);
            return true;
        }
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
        FString Body(Converted.Length(), Converted.Get());
        TSharedPtr<FJsonObject> Object;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Object) || !Object)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Body must be a JSON object.")), 400);
            return true;
        }
        Reply(Complete, Bridge->Execute(Object));
        return true;
    }

    FString Token;
    TUniquePtr<FJevEditorBridge> Bridge;
    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle Route;
    double RateWindow = 0;
    int32 RequestsInWindow = 0;
};

IMPLEMENT_MODULE(FJevEditorModule, JevEditor)
