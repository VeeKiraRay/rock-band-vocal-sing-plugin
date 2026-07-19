# scripts\deploy_plugin.ps1 — build the plugin and (re)install it into the system
# VST3 folder in one step, so testing never runs against a stale copy again (a
# nested VST3\Release\VST3\... duplicate once shadowed the real install for a day).
#
# Flow: build (current cache settings) -> close REAPER gracefully (it locks the
# installed .vst3; any save prompt is answered by the USER, never auto-clicked) ->
# elevated file stage (UAC prompt) that removes EVERY copy of the bundle under the
# VST3 root and installs the fresh one -> verify -> restart REAPER unelevated.
#
# Usage: scripts\deploy_plugin.bat [-Config Debug] [-NoRestart]

param(
    [ValidateSet('Release', 'Debug')]
    [string] $Config = 'Release',   # Release default: profile/test in REAPER at real speed
    [switch] $NoRestart
)

$ErrorActionPreference = 'Stop'

$repoRoot    = Split-Path -Parent $PSScriptRoot
$vst3Root    = 'C:\Program Files\Common Files\VST3'
$bundleName  = 'Rock Band Vocal Sing - Reaper.vst3'
$builtBundle = Join-Path $repoRoot "build\RBVocalSing_artefacts\$Config\VST3\$bundleName"
$installed   = Join-Path $vst3Root $bundleName
$dllRelPath  = "Contents\x86_64-win\$bundleName"

function Fail ([string] $msg)
{
    Write-Host "DEPLOY FAILED: $msg" -ForegroundColor Red
    exit 1
}

# Locate reaper.exe for the restart when no running process told us the path:
# installer-registered App Path first, then the default install locations.
function Find-ReaperExe
{
    $appPath = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\reaper.exe'
    if (Test-Path $appPath)
    {
        $exe = (Get-ItemProperty -LiteralPath $appPath).'(default)'
        if ($exe -and (Test-Path $exe)) { return $exe }
    }
    foreach ($candidate in @('C:\Program Files\REAPER (x64)\reaper.exe',
                             'C:\Program Files\REAPER\reaper.exe'))
    {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

# -- 1. Build ------------------------------------------------------------------
Write-Host "== Building ($Config)..."
Push-Location $repoRoot
try
{
    cmake -B build | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail 'cmake configure failed' }
    cmake --build build --config $Config --target RBVocalSing_VST3 | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail 'build failed' }
}
finally { Pop-Location }

if (-not (Test-Path (Join-Path $builtBundle $dllRelPath))) { Fail "built bundle not found: $builtBundle" }

# -- 2. Preflight --------------------------------------------------------------
if (-not (Test-Path $vst3Root)) { Fail "VST3 folder not found: $vst3Root" }

# -- 3. Close REAPER (it locks the installed plugin DLL) -----------------------
$reaperExe = $null
$reaper = Get-Process -Name reaper -ErrorAction SilentlyContinue
if ($reaper)
{
    $reaperExe = ($reaper)[0].Path
    Write-Host '== Closing REAPER - if it asks about unsaved changes, answer the prompt yourself...'
    $reaper | ForEach-Object { $null = $_.CloseMainWindow() }

    $waited = 0
    while ($waited -lt 30 -and (Get-Process -Name reaper -ErrorAction SilentlyContinue))
    {
        Start-Sleep -Seconds 1
        $waited++
    }
    if (Get-Process -Name reaper -ErrorAction SilentlyContinue)
    {
        Fail 'REAPER is still running after 30 s (unanswered dialog?) - close it manually and rerun'
    }
    Write-Host '   REAPER closed.'
}
else
{
    Write-Host '== REAPER not running.'
}

# -- 4. Elevated file stage ----------------------------------------------------
# Writing under Program Files needs admin, but neither the build above nor the
# REAPER restart below should run elevated - so only this stage elevates, as a
# child process running a temp script (paths baked in; result reported through a
# status file because an elevated child's exit code is unreliable to read).
$statusFile  = Join-Path $env:TEMP 'rbvs_deploy_status.txt'
$stageScript = Join-Path $env:TEMP 'rbvs_deploy_stage.ps1'
Remove-Item $statusFile -Force -ErrorAction SilentlyContinue

@"
`$ErrorActionPreference = 'Stop'
try
{
    # Remove EVERY bundle copy under the VST3 root, wherever it is nested - a stray
    # nested copy with the same plugin ID silently wins REAPER's scan.
    Get-ChildItem -LiteralPath '$vst3Root' -Recurse -Directory -Filter '$bundleName' -ErrorAction SilentlyContinue |
        Sort-Object { `$_.FullName.Length } -Descending |
        ForEach-Object { Remove-Item -LiteralPath `$_.FullName -Recurse -Force }

    Copy-Item -LiteralPath '$builtBundle' -Destination '$installed' -Recurse -Force
    Set-Content -LiteralPath '$statusFile' -Value 'OK' -Encoding ascii
}
catch
{
    Set-Content -LiteralPath '$statusFile' -Value ('ERR: ' + `$_.Exception.Message) -Encoding ascii
}
"@ | Set-Content -LiteralPath $stageScript -Encoding utf8

Write-Host '== Installing (a UAC prompt will appear)...'
try
{
    Start-Process powershell -Verb RunAs -Wait -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$stageScript`"")
}
catch
{
    Fail 'elevation was cancelled - the plugin was not installed'
}
finally
{
    Remove-Item $stageScript -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $statusFile)) { Fail 'install stage reported no status - the plugin may not be installed' }
$status = (Get-Content -LiteralPath $statusFile -Raw).Trim()
Remove-Item $statusFile -Force -ErrorAction SilentlyContinue
if ($status -ne 'OK') { Fail "install stage: $status" }

# -- 5. Verify -----------------------------------------------------------------
$builtDll = Join-Path $builtBundle $dllRelPath
$instDll  = Join-Path $installed  $dllRelPath
if (-not (Test-Path $instDll)) { Fail "installed binary missing: $instDll" }

$builtTime = (Get-Item $builtDll).LastWriteTimeUtc
$instTime  = (Get-Item $instDll).LastWriteTimeUtc
if ($builtTime -ne $instTime) { Fail "installed binary timestamp ($instTime) differs from the build ($builtTime)" }

# Anything plugin-named outside the canonical bundle is a leftover that could
# confuse a future session - warn, don't fail.
$strays = Get-ChildItem -LiteralPath $vst3Root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '*Rock Band Vocal Sing*' -and $_.FullName -notlike "$installed\*" }
if ($strays)
{
    Write-Host 'WARNING: plugin-named files exist outside the installed bundle:' -ForegroundColor Yellow
    $strays | ForEach-Object { Write-Host "   $($_.FullName)" -ForegroundColor Yellow }
}

Write-Host "== Installed $Config build (linked $builtTime UTC) to:"
Write-Host "   $installed"

# -- 6. Start REAPER (unelevated, from this non-admin process) -----------------
# Runs whether or not the script closed REAPER itself — the whole point is ending
# with REAPER up and scanning the fresh plugin.
if ($NoRestart)
{
    Write-Host '== -NoRestart given; REAPER left closed.'
}
else
{
    if (-not $reaperExe) { $reaperExe = Find-ReaperExe }
    if ($reaperExe)
    {
        Write-Host "== Starting REAPER ($reaperExe)..."
        Start-Process $reaperExe
    }
    else
    {
        Write-Host 'Could not locate reaper.exe (not running, no App Path registry entry, not in the default install folders) - start REAPER manually.' -ForegroundColor Yellow
    }
}

exit 0
