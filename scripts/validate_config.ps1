$ini = Join-Path (Split-Path $PSScriptRoot) 'config\AI_NPC.ini'
if (-not (Test-Path $ini)) { throw 'AI_NPC.ini not found' }
$txt = Get-Content $ini -Raw
if ($txt -match 'api_key=$') { Write-Warning 'api_key is blank; set OPENROUTER_API_KEY or edit config locally.' }
if ($txt -match 'sk-or-v1-') { throw 'Do not place a live API key in the distributed project.' }
Write-Host 'Config syntax looks OK.'
