# Architecture

```text
GTA Ped Pool
   |
   +--> Identity Manager
   |       +--> gender/name/age/occupation
   |       +--> faction/personality
   |       +--> relationship/memory
   |
   +--> World/Routine Context
   |
   +--> In-game chat UI
   |
   +--> OpenRouter
             |
             +--> strict JSON
                    |
                    +--> reply
                    +--> decision
                    +--> emotion
                    +--> memory
                    +--> relationship_delta
                    +--> action
                              |
                              +--> fixed GTA action executor
```

## AI agency

The system prompt tells the NPC to preserve its identity and make its own choice. The model can refuse the player's request. Faction context changes how the NPC evaluates the player.

## Safety/stability boundary

The model cannot issue memory addresses, C++ calls, scripts or arbitrary game commands. The executor maps only known enum values to known CPed objectives.
