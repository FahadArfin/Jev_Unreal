# Jev and Unreal: research and product boundaries

Research date: **2026-09-21**. Model availability and API details were checked against primary sources and the current official OpenRouter SDK. Prices, aliases, limits, and alpha endpoints can change. This document distinguishes provider statements from our engineering judgment; it is not a claim that we have independently benchmarked Jev.

## What Jev actually does

Jev is TypeSafe's hosted decision model. It consumes text or JSON state and returns typed judgments: a choice among supplied candidates, a score against ordered descriptions, or a Noul probability for a yes/no question. It does not generate C++, Python, Blueprints, dialogue text, explanations, or tool arguments with unrestricted values. It cannot inspect a viewport image directly. [TypeSafe System One](https://docs.typesafe.ai/concepts/system-one)

TypeSafe explicitly explains that Jev does not replace the model behind a coding agent. Our product should let Codex or another reasoning agent plan, generate code, and verify results, while offering Jev as an optional helper for narrow decisions. [Jev with coding agents](https://docs.typesafe.ai/introduction/coding-agents)

The phrase "tool calling" needs qualification. TypeSafe demonstrates selecting a function and its **closed-set** arguments with Choice and Noul questions. Its example excludes unrestricted numbers, dates, and free text from model-filled arguments. The application still dispatches and executes the function. That is useful routing, but does not mean Jev can author arbitrary Unreal commands. [Function-calling cookbook](https://docs.typesafe.ai/cookbooks/function_calling)

TypeSafe describes reinforcement learning for calibrated decisions (RLCD), not an API for retrieving language-model log probabilities or a generic JSON-schema constrained text generator. Public documentation does not establish enough implementation detail to independently verify the underlying model architecture or training claims. [AI primer](https://docs.typesafe.ai/introduction/machine-learning-primer)

## Verified provider integration

| Provider | HTTP endpoint | Versioned model | Credential |
| --- | --- | --- | --- |
| OpenRouter | `POST https://openrouter.ai/api/alpha/decisions` | `typesafe/jev-1.13` | OpenRouter bearer key |
| TypeSafe direct | `POST https://api.typesafe.ai/v1/systemone` | `jev-1.13.0` | TypeSafe bearer key |

Both accept `{model, state, questions}`; `questions` is an object keyed by application-owned question IDs. Answers use those same IDs. The direct endpoint and request contract are documented in [TypeSafe's API reference](https://docs.typesafe.ai/api). OpenRouter's actual Decisions endpoint is established by its [official SDK implementation](https://github.com/OpenRouterTeam/typescript-sdk/blob/38a849581f3a531909f7f0b50b4efec94ccf5d86/src/funcs/alphaDecisionsCreate.ts) and [server URL definition](https://github.com/OpenRouterTeam/typescript-sdk/blob/38a849581f3a531909f7f0b50b4efec94ccf5d86/src/models/operations/createapialphadecisions.ts). The alpha status is a compatibility risk: keep a small adapter and contract tests around it.

Do not send Jev requests to `/chat/completions`. OpenRouter's Jev examples use `openRouter.alpha.decisions.create`, including a two-stage example where a separate generative model prepares questions and Jev answers them. [OpenRouter prompt-to-questions recipe](https://openrouter.ai/labs/jev/compile)

The live [OpenRouter model API with the Decisions filter](https://openrouter.ai/api/v1/models?output_modalities=decisions) returned `typesafe/jev-1.13` and `~typesafe/jev-latest`, both with `text->decisions`, a 32,000-token advertised context, and an empty `supported_parameters` array. The default model list filters to text output, so an ordinary list can omit Jev. The [model page](https://openrouter.ai/typesafe/jev-1.13) listed $0.042 per million input tokens and free output tokens. This is API inference cost, not the cost of the entire game-development workflow.

Direct TypeSafe documents a 64k total request budget and a 32k limit for state plus the longest question. It currently lists 250,000 tokens/second and 1,200 requests/minute, explicitly subject to change. These direct-service limits should not be assumed for an OpenRouter account. Pin a version for evaluations: aliases can change behavior without a code change. [TypeSafe models](https://docs.typesafe.ai/models)

### Parsing and validation

The direct contract uses these shapes:

- Choice question: `type: "choice"`, `instructions`, and `criteria` mapping candidate IDs to descriptions. Maximum 255 options. Answer: `choice`, `probabilities`, `confidence`.
- Noul question: `type: "noul"`, `instructions`, optional `criteria` with `true` and `false` descriptions. Answer: `noul` in `[0, 1]`.
- Score question: `type: "score"`, `instructions`, and an ordered `criteria` array of 2–10 descriptions. Answer: a potentially fractional `score`, index-keyed `probabilities`, `legend`, and `confidence`.

The question ID itself is not supplied to the model for inference: include the subject in the instructions. [TypeSafe API contract](https://docs.typesafe.ai/api)

OpenRouter's SDK marks Choice `confidence` and `probabilities` optional, and Score `confidence`, `probabilities`, and `legend` optional. A missing confidence must remain unknown; a confidence-gated router must abstain or return a clear error rather than invent certainty. The SDK's Score legend permits structured descriptions as well as strings. [Choice response type](https://github.com/OpenRouterTeam/typescript-sdk/blob/38a849581f3a531909f7f0b50b4efec94ccf5d86/src/models/decisionschoiceanswer.ts), [Score response type](https://github.com/OpenRouterTeam/typescript-sdk/blob/38a849581f3a531909f7f0b50b4efec94ccf5d86/src/models/decisionsscoreanswer.ts)

Our validation policy should require exact answer IDs and matching types, finite bounded numbers, known candidate IDs, and coherent probability keys and totals when present. It should reject malformed responses instead of treating HTTP success as a valid decision. Do not search recursively for the first matching field in a batched response.

Noul has **no provider confidence field**. Choice and Score confidence summarize a probability distribution; confidence is not simply the winning probability or a guarantee that an individual answer is correct. Do not label a locally derived `max(p, 1-p)` value as provider confidence. Thresholds need domain-specific evaluation. [TypeSafe confidence](https://docs.typesafe.ai/confidence)

## Where this helps Unreal development

The following are engineering recommendations to evaluate, not established performance claims.

| Workflow | Potential benefit | Required boundary |
| --- | --- | --- |
| Tool-family routing | Select a small relevant subset from many editor capabilities | Keep exact arguments and execution in validated code |
| Asset shortlist ranking | Choose among existing mesh/material/Blueprint candidates using metadata and design intent | Query the real asset registry first; never invent asset paths |
| Log and validation triage | Classify diagnostics by subsystem, severity, and next investigation | Preserve original evidence; run actual compiler or validation checks |
| Repeated semantic checks | Batch many independent checks on names, descriptions, or design constraints | Deterministic checks own arithmetic, collision, transforms, references, and budgets |
| Import preset selection | Match a source asset to a known validated import profile | Preserve provenance, explicit dimensions, and source files |
| Dialogue or quest branch selection | Select authored branches from supplied world-state facts | A generative model or author writes the content |
| High-level gameplay direction | Occasional semantic choices at checkpoints or events | Keep local fallback, server authority, and a strict time budget |

Batching several independent questions over shared state is particularly promising because TypeSafe evaluates them in parallel. A router can ask both "which candidate fits?" and "does any candidate fit?" together, then let code decide what to do. [Speculative fan-out](https://docs.typesafe.ai/patterns/fan-out)

Adding Jev before every tool call may make development slower: it adds a network round trip even when the calling agent already knows the correct command. Its value must come from removing larger searches, repeated reasoning, or many separate judgments. This is why direct editor tools should remain usable without any provider key.

It is a poor fit for per-frame movement, collision, physics, pathfinding, animation timing, exact geometry, numeric tuning, or authoritative game rules. TypeSafe documents unreliable counting, numerical precision, date comparisons, complex indirection, and sensitivity to irrelevant context. It also warns that adversarial state can steer answers. Therefore Jev must never be the security boundary for deciding whether arbitrary code or destructive actions are permitted. [Jev 1.13 limitations](https://docs.typesafe.ai/model-jaggedness/jev-1.13)

OpenRouter publishes live examples with subsecond-to-second results, including a chaser at two to three moves per second. These are provider demonstrations, not guarantees for this plugin or evidence of 60 Hz gameplay suitability. [OpenRouter Jev Lab](https://openrouter.ai/labs/jev)

## Recommended architecture

1. **MCP server:** expose typed editor operations plus optional decisions, routing, and triage. Keep provider credentials in the local server environment or a local secret store.
2. **Unreal editor bridge:** run a bounded set of engine operations on the game thread, report the connected project and engine version, and return structured evidence. Authenticate the local bridge and bind locally by default.
3. **Provider adapters:** share typed question/result models; isolate OpenRouter alpha transport from TypeSafe direct transport. Bound request sizes, concurrency, timeouts, and retry attempts. Record provider/model, latency, and usage without logging credentials.
4. **Deterministic execution:** validate paths, object types, transforms, enum values, and project identity. Prefer transactions and idempotent operations; separate decision suggestions from executing mutations.
5. **Evaluation layer:** compare Jev-assisted workflows with direct MCP and deterministic routing on the same tasks. Keep fixtures and actual provider results distinct.
6. **Future Blender adapter:** reuse the decision/provider/MCP layers, but implement Blender-specific tools and validation. Do not make Unreal module types part of the provider interface.

A standalone runtime Blueprint integration is a separate product surface from an editor MCP bridge. A packaged client must never contain a shared provider key; runtime use needs an authenticated backend and offline behavior. The linked upstream plugin already documents proxy mode for this reason. [JevUnreal upstream](https://github.com/cnrveysel/JevUnreal)

## Upstream and licensing

The inspected upstream commit was [`042103febd5283b411bcbe73f203918d688262c8`](https://github.com/cnrveysel/JevUnreal/tree/042103febd5283b411bcbe73f203918d688262c8). It is a small async runtime Blueprint integration, not an editor automation MCP server. It provides Yes/No, Choose, Probability, timeout/error paths, and a raw advanced request. Its scope is useful inspiration, but it does not supply the editor discovery, execution, lifecycle, audit, and distribution infrastructure this project needs. [Upstream README](https://github.com/cnrveysel/JevUnreal/blob/042103febd5283b411bcbe73f203918d688262c8/README.md)

Upstream is MIT licensed, copyright 2026 Veysel. Copying or adapting substantial code requires preserving its copyright and permission notice. Both official TypeSafe SDKs are MIT licensed; the OpenRouter TypeScript SDK is Apache-2.0. Retain applicable third-party notices if these are redistributed. API compatibility and independently implemented adapters do not require vendoring an entire SDK. [Upstream license](https://github.com/cnrveysel/JevUnreal/blob/042103febd5283b411bcbe73f203918d688262c8/LICENSE), [TypeSafe JavaScript SDK license](https://github.com/typesafe-ai/typesafe-sdk-js/blob/main/LICENSE), [TypeSafe Python SDK license](https://github.com/typesafe-ai/typesafe-sdk-python/blob/main/LICENSE), [OpenRouter SDK license](https://github.com/OpenRouterTeam/typescript-sdk/blob/38a849581f3a531909f7f0b50b4efec94ccf5d86/LICENSE.md)

These software licenses do not grant rights to redistribute Jev model weights or Unreal Engine. This project integrates hosted Jev APIs and should distribute its own plugin source, not engine source or provider credentials. Hosted-service use remains subject to the provider's account terms. [TypeSafe legal documents](https://docs.typesafe.ai/legal)

## Evidence required before strong efficiency claims

Use a held-out set of representative tasks: ambiguous asset references, explicit no-match requests, log excerpts, malformed provider responses, disconnected editors, stale object paths, and repeated calls. Measure task success, wrong-action rate, abstention, p50/p95 latency, total model cost, total tool calls, and recovery behavior. Compare at least direct MCP, simple deterministic routing, and Jev-assisted routing. Pin model versions and preserve the input dataset so changes can be replayed.

Mocked responses establish transport and execution correctness; they do not establish Jev's judgment quality. Unreal compilation establishes compatibility; it does not establish end-to-end editor behavior. A successful local smoke test establishes the exercised operations; it does not establish support for every project or readiness for thousands of users.

**Assessment:** an excellent Unreal MCP bridge can be highly useful on its own. Jev is a plausible low-cost enhancement for routing, ranking, and batch judgments. Its incremental benefit remains an empirical question. Building the reliable editor bridge first, keeping Jev optional, and publishing reproducible evaluations is a stronger product direction than promising that Jev itself will create games.
