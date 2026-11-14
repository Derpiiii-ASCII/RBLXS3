# Build blox-ps3 on Windows. Installs Docker Desktop automatically (via winget) if missing.
#   .\windows\build.ps1          build .self and .pkg
#   .\windows\build.ps1 self     build .self only (RPCS3)
#   .\windows\build.ps1 pkg      build .pkg only, skips the .self
#   .\windows\build.ps1 clean    clean
#   .\windows\build.ps1 shell    shell inside the toolchain container
# Env: PS3_IMAGE=<image> (default ghcr.io/altps3/ps3dev:latest)
#      PS3_NO_INSTALL=1  never auto-install anything
param([Parameter(Position=0)][ValidateSet("all","self","pkg","clean","shell")][string]$Action = "all")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$image = if ($env:PS3_IMAGE) { $env:PS3_IMAGE } else { "ghcr.io/altps3/ps3dev:latest" }
$jobs  = [Environment]::ProcessorCount
$dockerBin = Join-Path $env:ProgramFiles "Docker\Docker\resources\bin"
$dockerExe = Join-Path $env:ProgramFiles "Docker\Docker\Docker Desktop.exe"

function Add-DockerToPath {
    if ((Test-Path $dockerBin) -and ($env:Path -notlike "*$dockerBin*")) { $env:Path += ";$dockerBin" }
}
function Test-DockerRunning {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return $false }
    cmd /c "docker info >nul 2>&1"
    return ($LASTEXITCODE -eq 0)
}
function Fail($msg) { Write-Host "[build] $msg" -ForegroundColor Red; exit 1 }

Add-DockerToPath

# 1) Install Docker Desktop if missing
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    if ($env:PS3_NO_INSTALL -eq "1") { Fail "Docker is missing and PS3_NO_INSTALL=1 is set." }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail "Docker is missing and winget isn't available. Install Docker Desktop from docker.com, then re-run."
    }
    Write-Host "[setup] Docker not found - installing Docker Desktop with winget (approve the UAC prompt)..."
    winget install -e --id Docker.DockerDesktop --silent --accept-package-agreements --accept-source-agreements
    Add-DockerToPath
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        Fail "Docker Desktop was installed. Restart Windows (WSL2 setup needs it), open Docker Desktop once and accept its terms, then run this again."
    }
}

# 2) Start Docker Desktop if it isn't running
if (-not (Test-DockerRunning)) {
    if (Test-Path $dockerExe) {
        Write-Host "[setup] Starting Docker Desktop (can take a minute)..."
        Start-Process $dockerExe
    }
    $ok = $false
    for ($i = 0; $i -lt 90; $i++) {
        if (Test-DockerRunning) { $ok = $true; break }
        Start-Sleep -Seconds 2
    }
    if (-not $ok) { Fail "Docker isn't responding. Open Docker Desktop, finish any first-run setup, then re-run." }
}

switch ($Action) {
    "all"   { $cmd = "make -j$jobs && { make pkg KEEP_SELF=1 || echo '!! pkg step failed (the .self above is still fine, see README)'; }" }
    "self"  { $cmd = "make -j$jobs" }
    "pkg"   { $cmd = "make -j$jobs pkg" }
    "clean" { $cmd = "make clean" }
    "shell" { $cmd = "bash" }
}

Write-Host "[build] docker image $image ($jobs jobs; first run downloads ~1 GB)"
docker run --rm -it -v "${PWD}:/src" -w /src $image bash -c $cmd
exit $LASTEXITCODE
