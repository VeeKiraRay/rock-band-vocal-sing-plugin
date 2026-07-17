# scripts/run_pluginval.ps1
# Smoke-tests the built VST3 with Tracktion's pluginval (black-box validator: loads
# the plugin, exercises processBlock/editor lifecycle/state save-restore across
# buffer sizes and sample rates). See TEST_PLAN.md section 3.
#
# Usage: scripts/run_pluginval.ps1   (run after building the RBVocalSing target)

$ErrorActionPreference = 'Stop'

$repoRoot   = Resolve-Path (Join-Path $PSScriptRoot '..')
$toolsDir   = Join-Path $repoRoot 'build/tools'
$pluginval  = Join-Path $toolsDir 'pluginval.exe'

if (-not (Test-Path $pluginval)) {
    Write-Host 'pluginval not found — downloading the latest Windows release...'
    New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null

    $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/Tracktion/pluginval/releases/latest'
    $asset   = $release.assets | Where-Object { $_.name -imatch 'win' } | Select-Object -First 1
    if (-not $asset) { throw 'Could not find a Windows asset in the latest pluginval release.' }

    $zipPath = Join-Path $toolsDir $asset.name
    Write-Host "Downloading $($asset.browser_download_url)..."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $toolsDir -Force
    Remove-Item $zipPath -Force

    $found = Get-ChildItem -Path $toolsDir -Recurse -Filter 'pluginval.exe' | Select-Object -First 1
    if (-not $found) { throw 'pluginval.exe was not found after extracting the release archive.' }
    $pluginval = $found.FullName
}

$vst3 = Get-ChildItem -Path (Join-Path $repoRoot 'build') -Recurse -Directory -Filter '*.vst3' |
        Select-Object -First 1
if (-not $vst3) { throw 'No .vst3 bundle found under build/ — build the RBVocalSing target first.' }
Write-Host "Validating: $($vst3.FullName)"

$logDir = Join-Path $repoRoot 'build/pluginval-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

& $pluginval --strictness-level 5 --validate-in-process --output-dir $logDir $vst3.FullName

exit $LASTEXITCODE
