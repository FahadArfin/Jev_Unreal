# Provenance

Jev_Unreal is an independent community integration by Fahad Arfin and contributors. It is not affiliated with or endorsed by Epic Games, TypeSafe AI, OpenRouter, or the author of cnrveysel/JevUnreal.

The project was inspired by [Veysel's JevUnreal forum post](https://forums.unrealengine.com/t/built-an-ai-decision-plugin-for-unreal-engine-useful-or-too-niche/2832808) and [MIT-licensed JevUnreal](https://github.com/cnrveysel/JevUnreal). This repository implements its own editor bridge and Python MCP server; no upstream implementation was vendored or copied. That upstream project focuses on runtime Blueprint decision nodes; this project focuses on editor workflows.

Original files in this repository use the MIT license in `LICENSE`. Dependencies retain their own licenses. Python dependencies and transitive versions are recorded in `uv.lock`; installed package metadata contains their notices. Unreal Engine is proprietary software licensed separately by Epic. No Epic engine source or binaries are distributed here. Plugin compilation requires the user's licensed engine installation.

The provider wire contract is implemented from official API documentation linked in `RESEARCH.md`. Jev model weights are not included. API availability, provider billing and data handling remain governed by the selected provider's terms.
