# Installation

### 1. Build

Run `scripts\build_windows.ps1` from PowerShell, or use the GitHub Actions workflow.

### 2. API

Use a newly generated OpenRouter key. Never paste it into GitHub or screenshots.

### 3. Game files

Copy `AI_NPC.asi`, `AI_NPC.ini`, `D3DX9_43.dll`, and `D3DCompiler_43.dll` beside `gta_sa.exe`.

### 4. First test

Start a new game or load a save. Walk within about 4.2 meters of an NPC. Press `F`, then `T`. Type a normal sentence and press Enter.

### 5. Test actions

Try a friendly civilian:
- `Come here.`
- `Follow me.`
- `Wait here.`
- `Stop following me.`

Try a faction NPC and compare the way it reasons about requests. The AI receives the faction and relationship context before deciding.
