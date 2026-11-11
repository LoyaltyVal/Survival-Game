#!/usr/bin/env bash
# Survival Game launcher for Linux.
#
#   ./play-linux.sh                    play (builds the game the first time, ~2 minutes)
#   ./play-linux.sh --connect IP:PORT  join a server directly
#   ./play-linux.sh server             run a dedicated server (add --port, --name, --owner ...)
#   ./play-linux.sh shortcut           add "Survival Game" to your applications menu
#   ./play-linux.sh rebuild            force a clean rebuild
#
# If the file isn't executable after unzipping: right-click > Properties > "Allow executing",
# or run it with:  bash play-linux.sh
set -euo pipefail

GAME_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
BUILD_DIR="$GAME_DIR/build-linux"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/survival-game"   # saves, settings, server list
PREBUILT="$GAME_DIR/bin"                                           # present in the ready-to-play download

say()  { printf '\033[1;33m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31mError:\033[0m %s\n' "$*" >&2; pause_if_window; exit 1; }
pause_if_window() { if [[ "${SG_IN_WINDOW:-}" == 1 ]]; then read -rp "Press Enter to close..." _; fi; }

# Double-clicked from a file manager (no terminal)? Reopen in a terminal so the
# first-time build and any questions are visible.
if [[ ! -t 0 && -z "${SG_IN_WINDOW:-}" && "${1:-}" != "--no-terminal" ]]; then
  export SG_IN_WINDOW=1
  for term in x-terminal-emulator gnome-terminal konsole xfce4-terminal kitty alacritty xterm; do
    if command -v "$term" >/dev/null 2>&1; then
      case "$term" in
        gnome-terminal) exec gnome-terminal -- bash "$0" "$@" ;;
        *) exec "$term" -e bash "$0" "$@" ;;
      esac
    fi
  done
  unset SG_IN_WINDOW  # no terminal found: carry on silently
fi
[[ "${1:-}" == "--no-terminal" ]] && shift

client_bin() { if [[ -x "$PREBUILT/sg_client" ]]; then echo "$PREBUILT/sg_client"; else echo "$BUILD_DIR/sg_client"; fi; }
server_bin() { if [[ -x "$PREBUILT/sg_server" ]]; then echo "$PREBUILT/sg_server"; else echo "$BUILD_DIR/sg_server"; fi; }

# --- dependencies -------------------------------------------------------------------
missing_deps() {
  local missing=()
  for tool in cmake g++ git make; do command -v "$tool" >/dev/null 2>&1 || missing+=("$tool"); done
  for header in X11/Xlib.h X11/extensions/Xrandr.h X11/extensions/Xinerama.h X11/Xcursor/Xcursor.h \
                X11/extensions/XInput2.h GL/gl.h; do
    [[ -f "/usr/include/$header" ]] || missing+=("$header")
  done
  echo "${missing[@]:-}"
}

install_command() {
  if command -v apt-get >/dev/null 2>&1; then
    echo "sudo apt-get update && sudo apt-get install -y build-essential cmake git xorg-dev libgl1-mesa-dev"
  elif command -v dnf >/dev/null 2>&1; then
    echo "sudo dnf install -y gcc-c++ make cmake git libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel"
  elif command -v pacman >/dev/null 2>&1; then
    echo "sudo pacman -S --needed --noconfirm base-devel cmake git libx11 libxrandr libxinerama libxcursor libxi mesa"
  elif command -v zypper >/dev/null 2>&1; then
    echo "sudo zypper install -y gcc-c++ make cmake git libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel Mesa-libGL-devel"
  else
    echo ""
  fi
}

ensure_deps() {
  local missing; missing="$(missing_deps)"
  [[ -z "$missing" ]] && return 0
  say "The first build needs a few developer packages (missing: $missing)."
  local cmd; cmd="$(install_command)"
  [[ -n "$cmd" ]] || fail "Unknown Linux distribution. Install a C++ compiler, CMake, git and the X11 + OpenGL development headers, then run this again."
  echo "    $cmd"
  read -rp "Install them now? You'll be asked for your password. [Y/n] " answer
  [[ "${answer:-y}" =~ ^[Yy]$ ]] || fail "Can't build without them."
  bash -c "$cmd"
  missing="$(missing_deps)"
  [[ -z "$missing" ]] || fail "Still missing: $missing"
}

# --- build ------------------------------------------------------------------------------
needs_build() {
  [[ -x "$PREBUILT/sg_client" ]] && return 1          # ready-made download: nothing to build
  [[ -x "$BUILD_DIR/sg_client" ]] || return 0
  # Rebuild when any source file changed since the last build (e.g. you updated the game).
  [[ -n "$(find "$GAME_DIR/client" "$GAME_DIR/server" "$GAME_DIR/shared" "$GAME_DIR/CMakeLists.txt" \
            -newer "$BUILD_DIR/sg_client" -print -quit 2>/dev/null)" ]]
}

build() {
  ensure_deps
  say "Building the game (first time takes a couple of minutes)..."
  cmake -S "$GAME_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DSG_BUILD_TESTS=OFF
  cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"
  say "Build finished."
}

install_shortcut() {
  local apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
  mkdir -p "$apps"
  cat > "$apps/survival-game.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Survival Game
Comment=Gather, craft, build and survive
Exec=bash "$GAME_DIR/play-linux.sh" --no-terminal
Path=$GAME_DIR
Icon=applications-games
Terminal=false
Categories=Game;
EOF
  chmod +x "$apps/survival-game.desktop" "$GAME_DIR/play-linux.sh" 2>/dev/null || true
  say "Added 'Survival Game' to your applications menu."
  if [[ -d "$HOME/Desktop" ]]; then
    cp "$apps/survival-game.desktop" "$HOME/Desktop/" && chmod +x "$HOME/Desktop/survival-game.desktop"
    command -v gio >/dev/null 2>&1 && gio set "$HOME/Desktop/survival-game.desktop" metadata::trusted true 2>/dev/null || true
    say "And a desktop icon."
  fi
}

# --- main ---------------------------------------------------------------------------------
case "${1:-play}" in
  shortcut)
    needs_build && build
    install_shortcut
    pause_if_window
    exit 0 ;;
  rebuild)
    rm -rf "$BUILD_DIR"
    build
    pause_if_window
    exit 0 ;;
  server)
    shift
    needs_build && build
    mkdir -p "$DATA_DIR"
    cd "$DATA_DIR"
    say "Dedicated server. Saves go to $DATA_DIR. Players join on UDP port 27015 (forward it on your router)."
    exec "$(server_bin)" "$@" ;;
  play) shift || true ;;
  *) ;;  # anything else (e.g. --connect) is passed to the game
esac

if needs_build; then build; fi
mkdir -p "$DATA_DIR"
cd "$DATA_DIR"
say "Starting Survival Game (saves and settings: $DATA_DIR)"
"$(client_bin)" "$@" || { status=$?; say "The game exited with code $status."; pause_if_window; exit $status; }
