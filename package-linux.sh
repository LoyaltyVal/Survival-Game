#!/usr/bin/env bash
# Builds a ready-to-play Linux download: dist/survival-game-linux-x86_64.tar.gz
# Run this once on any Linux PC (or let GitHub Actions do it, see .github/workflows/build.yml).
# Players extract it and run ./play-linux.sh; no compiler needed on their side.
set -euo pipefail
GAME_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
BUILD_DIR="$GAME_DIR/build-package"
NAME="survival-game-linux-$(uname -m)"
OUT="$GAME_DIR/dist/$NAME"

cmake -S "$GAME_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DSG_BUILD_TESTS=OFF -DSG_PORTABLE=ON
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$BUILD_DIR/sg_client" "$BUILD_DIR/sg_server" "$OUT/bin/"
strip "$OUT/bin/sg_client" "$OUT/bin/sg_server" 2>/dev/null || true
cp "$GAME_DIR/play-linux.sh" "$OUT/"
chmod +x "$OUT/play-linux.sh" "$OUT/bin/"*
cat > "$OUT/README.txt" <<'EOF'
Survival Game for Linux

  Play:              ./play-linux.sh        (or double-click it)
  Menu/desktop icon: ./play-linux.sh shortcut
  Dedicated server:  ./play-linux.sh server --port 27015 --name "My Server" --owner YourName

Saves, settings and the server list live in ~/.local/share/survival-game
Needs OpenGL 3.3 graphics drivers (any GPU from the last ~12 years).
EOF
tar -C "$GAME_DIR/dist" -czf "$GAME_DIR/dist/$NAME.tar.gz" "$NAME"
echo "Done: dist/$NAME.tar.gz"
