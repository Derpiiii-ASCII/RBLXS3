#!/usr/bin/env bash
# Build blox-ps3 on Linux (also works on macOS / WSL).
# Installs Docker/Podman automatically if it's missing, then builds in a container.
#   ./linux/build.sh          build .self and .pkg
#   ./linux/build.sh self     build .self only (RPCS3)
#   ./linux/build.sh pkg      build .pkg only, skips the .self
#   ./linux/build.sh clean    clean
#   ./linux/build.sh shell    shell inside the toolchain container
# Env: PS3_IMAGE=<image> (default ghcr.io/altps3/ps3dev:latest)
#      PS3_NO_INSTALL=1      never auto-install anything
set -euo pipefail
cd "$(dirname "$0")/.."

ACTION="${1:-all}"
IMAGE="${PS3_IMAGE:-ghcr.io/altps3/ps3dev:latest}"
have() { command -v "$1" >/dev/null 2>&1; }
fail() { echo "[build] $*" >&2; exit 1; }

JOBS="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) | head -n1)"

case "$ACTION" in
  all)   CMD="make -j$JOBS && { make pkg KEEP_SELF=1 || echo '!! pkg step failed (the .self above is still fine, see README)'; }" ;;
  self)  CMD="make -j$JOBS" ;;
  pkg)   CMD="make -j$JOBS pkg" ;;
  clean) CMD="make clean" ;;
  shell) CMD="bash" ;;
  *) echo "usage: $0 [all|self|pkg|clean|shell]"; exit 2 ;;
esac

# 1) Native PSL1GHT toolchain already installed? Use it.
if have powerpc64-ps3-elf-gcc && [ -n "${PSL1GHT:-}" ]; then
  echo "[build] using native PSL1GHT toolchain ($JOBS jobs)"
  exec bash -c "$CMD"
fi

# 2) Container runtime (installed on demand)
run_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"
  elif have sudo; then sudo "$@"
  else echo "[setup] need root to run: $*" >&2; return 1; fi
}

RT=""; PFX=()
pick_runtime() {
  RT=""; PFX=()
  if have docker; then
    if docker info >/dev/null 2>&1; then RT=docker; return 0; fi
    if run_root docker info >/dev/null 2>&1; then
      RT=docker; [ "$(id -u)" -ne 0 ] && PFX=(sudo); return 0
    fi
  fi
  if have podman && podman info >/dev/null 2>&1; then RT=podman; return 0; fi
  return 1
}

install_runtime() {
  [ "${PS3_NO_INSTALL:-0}" = 1 ] && fail "Docker/Podman missing and PS3_NO_INSTALL=1 is set."
  echo "[setup] Docker/Podman not found - installing it now (you may be asked for your password)..."
  if [ "$(uname -s)" = Darwin ]; then
    have brew || fail "Install Homebrew (brew.sh) or Docker Desktop, then re-run."
    brew install --cask docker
    open -a Docker || true
    return 0
  fi
  if   have apt-get; then run_root apt-get update && run_root apt-get install -y docker.io
  elif have dnf;     then run_root dnf install -y podman
  elif have yum;     then run_root yum install -y podman
  elif have pacman;  then run_root pacman -S --needed --noconfirm docker
  elif have zypper;  then run_root zypper --non-interactive install docker
  elif have apk;     then run_root apk add docker
  else fail "Unknown package manager - install Docker or Podman manually, then re-run."
  fi
  if have docker; then
    run_root usermod -aG docker "${USER:-$(id -un)}" 2>/dev/null || true
    echo "[setup] Added you to the 'docker' group (takes effect next login; using sudo for now)."
  fi
}

start_daemon() {
  have docker || return 0
  if have systemctl && run_root systemctl enable --now docker >/dev/null 2>&1; then return 0; fi
  if have service && run_root service docker start >/dev/null 2>&1; then return 0; fi
  if have rc-service; then run_root rc-service docker start >/dev/null 2>&1 || true; fi
  return 0
}

if ! pick_runtime; then
  if ! have docker && ! have podman; then
    install_runtime
  fi
  echo "[setup] Starting container service..."
  start_daemon
  for _ in $(seq 1 60); do pick_runtime && break; sleep 2; done
  [ -n "$RT" ] || fail "Container runtime installed but not responding. On macOS open Docker Desktop once; otherwise try: sudo systemctl start docker"
fi

HOSTDIR="$PWD"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1; HOSTDIR="$(pwd -W 2>/dev/null || pwd)" ;;
esac

EXTRA=()
[ "$RT" = docker ] && [ "$(uname -s)" = Linux ] && EXTRA=(--user "$(id -u):$(id -g)")
VOL="$HOSTDIR:/src"
[ "$RT" = podman ] && VOL="$VOL:Z"
TTY=(); [ -t 0 ] && [ -t 1 ] && TTY=(-it)

echo "[build] using $RT, image $IMAGE ($JOBS jobs; first run downloads ~1 GB)"
exec ${PFX[@]+"${PFX[@]}"} "$RT" run --rm ${TTY[@]+"${TTY[@]}"} ${EXTRA[@]+"${EXTRA[@]}"} \
  -v "$VOL" -w /src "$IMAGE" bash -c "$CMD"
