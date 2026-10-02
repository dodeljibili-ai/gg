# GTA San Andreas AI NPC — Final Source v1.0

A serious foundation for an AI-driven NPC system for classic GTA San Andreas PC 1.0 US using DK22Pac/plugin-sdk and OpenRouter.

## What this package contains

- In-game NPC names above nearby pedestrians (configurable).
- `F` selects the closest NPC; `T` opens a GTA-style chat input overlay.
- The conversation feed is rendered inside GTA; the input is a small borderless child window attached to the game window so typing works reliably without requiring an external console.
- OpenRouter integration using `qwen/qwen3.8-27b:free` by default.
- Structured JSON AI decisions.
- Stable session identity: name, gender, age, occupation, faction, personality, neighborhood, relationship and memory.
- Vanilla GTA SA model-ID faction detection for major gangs/groups.
- The AI is explicitly allowed to accept, refuse, or remain neutral. It is not forced to agree with the player.
- Faction context is part of the NPC identity. A Ballas member does not receive the same social context as a civilian.
- Action allow-list: `FOLLOW_PLAYER`, `STOP_FOLLOW`, `WAIT`, `GO_TO_PLAYER`, `GO_HOME`, `GO_TO_WORK`, `NONE`.
- Follow/stop/wait/come/home/work actions are executed by GTA ped objectives rather than being displayed as text-only promises.
- Relationship changes and important memory snippets are persisted to `AI_NPC/memory/`.
- Optional simple daily routine state is exposed to the AI and can drive home/work actions.
- No weapon-control actions are included in this project.
- API key is loaded at runtime from `AI_NPC.ini` or `OPENROUTER_API_KEY`; it is not embedded in the binary.

## Compatibility

Target: classic GTA San Andreas PC 1.0 US with ASI Loader + DK22Pac/plugin-sdk.

The plugin-sdk is intentionally not vendored in this archive. The build scripts pin a known plugin-sdk commit so a future SDK change does not unexpectedly break the build. The build also downloads Microsoft.DXSDK.D3DX 9.29.952.8 for the x86 D3DX9 linker/runtime files required by plugin-sdk Shader support.

## Install prerequisites on Windows

1. A classic GTA SA PC 1.0 US installation.
2. An ASI Loader that matches your GTA SA setup.
3. Visual Studio 2026 Build Tools with Desktop C++ and Windows SDK (the pinned current plugin-sdk generator targets VS2026).
4. Git.
5. CMake 3.20+.

## Build

Open PowerShell in this project folder and run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build_windows.ps1
```

The script downloads DK22Pac/plugin-sdk into `third_party\plugin-sdk` if it is missing and configures a 32-bit Visual Studio build.

If your machine does not have the requested Visual Studio generator, run CMake manually using an installed x86 generator.

## Configure the API key

Use a NEW OpenRouter key locally. The API key previously pasted into chat should be rotated because it was exposed.

Edit:

`config\AI_NPC.ini`

and put:

```ini
[OpenRouter]
api_key=YOUR_NEW_KEY
model=qwen/qwen3.8-27b:free
endpoint=https://openrouter.ai/api/v1/chat/completions
```

Or set the Windows environment variable `OPENROUTER_API_KEY` and leave `api_key=` blank.

## Copy into the game

After the build succeeds, copy:

```text
build\Release\AI_NPC.asi
release\AI_NPC.asi
release\AI_NPC.ini
release\D3DX9_43.dll
release\D3DCompiler_43.dll
```

to the folder containing `gta_sa.exe`.

The plugin creates:

```text
GTA San Andreas\AI_NPC\memory\
GTA San Andreas\AI_NPC\logs\
```

## Controls

- `F`: select nearest NPC
- `T`: open chat with selected NPC
- `Enter`: send
- `Esc`: close input
- `F10`: enable/disable AI NPC

## Important design rule

The model never executes arbitrary GTA code. It can only select values from the strict action list returned by the JSON schema. GTA executes the corresponding known action.

## Identity examples

A civilian might receive a profile such as:

```text
Name: Maya Carter
Gender: female
Age: 27
Occupation: shop clerk
Faction: Civilian
Personality: observant, calm, slightly sarcastic
```

A gang pedestrian can receive a different profile:

```text
Name: Darnell
Gender: male
Age: 31
Occupation: gang associate
Faction: Ballas
Personality: suspicious, territorial, blunt
Relationship: -20
```

The profile itself is not displayed as a giant label over the NPC. Only the name is displayed when name rendering is enabled.

The occupation system is data-driven. You can add more non-explicit fictional occupations in `config\occupation_pool.ini` without changing the core AI protocol.

## Current boundary

This package is a complete source/build project rather than a precompiled binary. I cannot run a Windows x86 GTA/plugin-sdk build inside this Linux environment, so the `.asi` binary itself is not falsely presented as tested. The Windows build script is included specifically to produce it on a Windows machine with the correct GTA/plugin-sdk toolchain.

## Build compatibility note

The project matches the plugin-sdk static MSVC runtime (`/MT`) and links the x86 D3DX9 import library required by plugin-sdk Shader support. CI uploads a ready-to-install artifact containing the `.asi`, `AI_NPC.ini`, `D3DX9_43.dll`, and `D3DCompiler_43.dll`.
