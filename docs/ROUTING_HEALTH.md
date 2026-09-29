# Selective routing and provider health

Use native Unreal tools directly when the next action is known. The optional
`jev_route_selective` tool can avoid a classifier request for two explicit cases:

- `explicit_tool` exactly matches an ID in the supplied candidate list (or the
  built-in routing catalog when candidates are omitted).
- The supplied list contains exactly one candidate.

These are local recommendations based on caller input, not measured semantic
accuracy. An explicit ID takes precedence over the goal's words. A single
candidate is not proof that the candidate fits. Both paths return `executed: false`
and retain the normal inspection, preview, authorization and verification steps.
There is no keyword score or confidence-based permission shortcut.

For two or more candidates, selective routing defers by default. Set
`allow_cloud: true` to consult Jev using the existing Decisions contract. Only
the explicit goal and candidate descriptions are sent. A cached answer can avoid
transport; missing uncertainty or a provider failure causes an explicit deferral.
There is no automatic retry or hidden fallback recommendation.

```json
{
  "goal": "Inspect this actor's bounds",
  "explicit_tool": "unreal_actor_details"
}
```

Every result includes `routing.method`, `routing.cloud_requested`,
`routing.provider_request_sent` and `routing.cache_hit`. `cloud_requested` means
the classifier path was requested; a cache hit or locally refused request can
still have `provider_request_sent: false`. Transport attempts are not billing
receipts. `jev_route` remains available with its original behavior for callers
that explicitly want a provider recommendation every time, subject to caching.

## Local observation versus fresh probe

`jev_provider_health()` performs no cloud request. It reports:

- `key_present`: whether a key was loaded into this server process.
- `authentication.status`: `unknown`, `authenticated`, or `rejected`, with the
  age of the observation. A validated successful decision establishes
  `authenticated`; HTTP 401 establishes `rejected`.
- `last_request`: the latest attempted provider request's outcome, HTTP status,
  sanitized error code and observation age.

`jev_status` includes these same local facts under `decisions.health`, separately
from the editor connection. Its existing `configured` field continues to mean
only that a key is present. A provider can authenticate while the Unreal editor
is disconnected, or the editor can work while provider authentication fails.

HTTP 403, rate limits, malformed responses and network failures are recorded as
request failures; they do not establish whether the key is valid. A previous
authentication observation remains dated evidence, not a guarantee of current
access. Cached decisions do not refresh that evidence or erase a later rejection.
Observations live only in memory and reset when the MCP server restarts.

Set `probe: true` only when a fresh provider check is wanted. This sends one fixed
synthetic question through the configured Decisions endpoint, bypassing the
decision cache and keeping the configured timeout, request budget and circuit
breaker. It contains no scene data and performs no editor operation. It can incur
provider charges. The result reports whether a request was attempted and whether
a valid response arrived; the tool never retries automatically. A blocked circuit,
missing key or exhausted session budget prevents the probe from reaching transport.

Update credentials locally using `scripts/Set-OpenRouterKey.ps1`; never paste a
key into chat or commit it. Restart/reconnect the MCP process to load a changed
key, then explicitly request a probe if desired.

An OpenRouter 401 with the recognized expiry reason returns fixed guidance to
replace the expired key. Error-body inspection is limited to 16 KiB and two seconds;
unknown, malformed, oversized or unreadable errors keep the generic HTTP status.
Raw messages and metadata are never returned. If a newly saved key still appears
old, check [credential source and Windows file redirection](SETUP.md#openrouter-authentication).

The implementation is covered with synthetic HTTP fixtures, including 401
recovery, cache behavior, 403/rate limits/network failures, bounded probes and
local routing without transport. It does not establish live provider availability
or measured workflow efficiency. Live provider testing remains separately opt-in.
