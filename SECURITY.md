# Security policy

## Supported versions

Security fixes target the latest commit on the default branch until versioned
releases and a support window are published. This is an early project; it has not
received an independent security audit.

## Report a vulnerability

Use the repository's **Security > Advisories > Report a vulnerability** option if
private reporting is enabled. If that option is unavailable, open a public issue
titled "Private security contact requested" without exploit details, secrets,
project data, or a proof of concept. A maintainer will arrange a private channel.
There is no guaranteed response time or bug bounty.

Include the affected revision, operating system and Unreal version, preconditions,
expected behavior, actual behavior, and a minimal reproduction using a disposable
project. Redact API keys and tokens. Do not test against someone else's machine or
provider account.

## Deployment boundaries

Run the MCP process and Unreal bridge on your own development machine. The editor
bridge should remain bound to loopback and use its authentication token. It is not
a hosted, multi-user service and must not be exposed through port forwarding or a
public reverse proxy. Treat access to it as access to the open editor project.

Keep an independent backup or version-controlled copy before allowing editor
mutations. Structured model output still requires deterministic validation; model
responses are untrusted input. Only explicitly supported operations should run.

The 0.3 alpha exposes 27 MCP tools. Existing-actor edits are restricted to supported
exact native StaticMeshActors without attachment or editability blockers. Transform,
existing material-slot assignment, and actor label/folder operations pass through
the same native preview/apply boundary. The bridge provides no arbitrary Python,
shell, console, C++, Blueprint execution or generic property setter. Capability
advertisements indicate protocol support; they grant no permission and do not
replace project binding, actor validation or user authorization.

Use fresh actor inspection and, where relevant, an `expected_state` containing the
measured session, world and revision when creating a preview. Spatial recipes do
this automatically. Native plans expire after 120 seconds, are consumed once, and
reject changed tracked state. The fingerprint covers supported actor state; it is
not a complete hash of every property or asset byte. Geometry checks use world
AABBs and do not establish terrain contact, collision safety or gameplay behavior.

An apply timeout, cancellation or incomplete rollback can leave the outcome
unknown. Do not retry an ambiguous plan. Read `unreal_plan`, inspect current actors
and use fresh verification/diff before making a new plan. Successful native apply
and verification are separate observations; a failed check does not undo an edit.
Only Unreal's editor transaction provides Undo, and no workflow automatically
saves packages.

Plan records and selected-actor snapshots are held in MCP process memory, returned
only when requested, and bounded by count, serialized bytes and a 15-minute
lifetime. Plan records allow up to 64 entries/2 MiB (64 KiB per record, with details
omitted when necessary); snapshots allow 32 entries/2 MiB. Python object overhead
is additional. Records can be evicted and disappear on restart. Receipt writes are
best effort; neither store is a durable audit log, crash recovery service, backup,
or replay queue. The separate local attempted-plan guard is also bounded; native
single-use plans remain the execution boundary.

Catalog discovery is restricted to explicitly configured literal loopback HTTP(S)
endpoints, with redirects, compression and external `tools/call` rejected. Exact
schemas and annotations remain untrusted metadata, not authorization. A discovered
tool may target a different editor; use its existing client and identity checks.
Credential environment-variable names may be stored in catalog configuration;
credential values may not. Failed refreshes preserve the prior snapshot and expose
their failure status.

Asset selection and diagnostic grouping stay local unless `use_jev` is explicitly
enabled. Redaction is best effort and does not make an arbitrary private log safe
to upload. Viewport images are returned to the requesting MCP client. That client
may forward them to its own model/provider for visual review; Jev_Unreal does not
control the client's data policy. This server sends no images to Jev.
PNG structure validation is not a general-purpose image-decoder security audit.
Actor details, snapshots, checks and plan records also contain project metadata
visible to the requesting MCP client; this server does not automatically send that
data to Jev. Camera framing changes the current editor view, including explicit
perspective presets, but does not edit actor geometry.

The supported workflow is inspect, snapshot the intended selection, preview,
apply once, then verify/diff fresh data and review a viewport capture. Missing or
incomplete evidence must remain unverifiable. See the
[verification contract](docs/VERIFICATION.md),
[spatial workflow](docs/SPATIAL_WORKFLOWS.md), and [roadmap](docs/ROADMAP.md) for
current boundaries and work that is not yet implemented.

Provider API keys belong in the local process environment or supported local secret
storage. The Windows helper uses Windows DPAPI tied to the current user account;
this protects the stored file but does not protect against malware running as that
user. Provider calls may send the request content outside your machine and incur
charges on your account. Do not include confidential content unless authorized.

If a key is exposed, revoke it with the provider and replace it locally. Deleting a
Git commit does not revoke a secret. If a bridge token is exposed, stop the bridge,
rotate its token, update local configuration, and restart the bridge.
