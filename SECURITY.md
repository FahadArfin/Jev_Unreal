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

Provider API keys belong in the local process environment or supported local secret
storage. The Windows helper uses Windows DPAPI tied to the current user account;
this protects the stored file but does not protect against malware running as that
user. Provider calls may send the request content outside your machine and incur
charges on your account. Do not include confidential content unless authorized.

If a key is exposed, revoke it with the provider and replace it locally. Deleting a
Git commit does not revoke a secret. If a bridge token is exposed, stop the bridge,
rotate its token, update local configuration, and restart the bridge.
