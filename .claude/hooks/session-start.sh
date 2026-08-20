#!/bin/bash
#
# Provision a Claude Code on the web container for the three builds in CLAUDE.md.
# The container is ephemeral and starts with none of this; its state is cached
# after the hook completes, so a second session gets the fast path throughout.
#
# What each piece is for:
#   SDL2, libpng   the native build -- the reference target, and the one
#                  verify-corpora.sh replays the goldens against
#   Node 24        web/, and the browser build, which bundles web/ beside the
#                  page (Main/CMakeLists.txt)
#   emsdk 6.0.6    the two WASM targets. SDL2, SDL2_mixer and libpng come from
#                  Emscripten's own ports inside the SDK, not from apt
#
# Versions are read from the files that already pin them rather than repeated
# here: .nvmrc for node, deploy.yml's EMSDK_VERSION for the SDK (docs/port-log.md
# §9.9 -- "latest" is not a toolchain).

set -euo pipefail

# A developer's own machine has its own toolchain and its own ~/emsdk; this
# provisions the throwaway container only.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

repo="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$repo"

node_version="$(tr -d '[:space:]' < .nvmrc)"
emsdk_version="$(sed -n 's/^ *EMSDK_VERSION: *//p' .github/workflows/deploy.yml | head -1)"
emsdk_dir="$HOME/emsdk"

if [ -z "$node_version" ] || [ -z "$emsdk_version" ]; then
  echo "session-start: could not read the pinned node/emsdk versions" >&2
  exit 1
fi

say() { printf '\n== %s\n' "$1"; }

# ---------------------------------------------------------------- native deps
say "SDL2 and libpng (native build)"
apt_pkgs=(libsdl2-dev libsdl2-mixer-dev libpng-dev)
if ! dpkg -s "${apt_pkgs[@]}" >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${apt_pkgs[@]}"
else
  echo "already installed"
fi

# ----------------------------------------------------------------- node 24
say "node $node_version (web/ and the browser build)"
export NVM_DIR="${NVM_DIR:-/opt/nvm}"
# shellcheck disable=SC1091
. "$NVM_DIR/nvm.sh"
nvm install "$node_version" >/dev/null
nvm alias default "$node_version" >/dev/null
node_bin="$(dirname "$(nvm which "$node_version")")"
export PATH="$node_bin:$PATH"
echo "$(node --version) at $node_bin"

# ------------------------------------------------------------------- web deps
# npm ci rather than npm install: the lockfile is committed and CI installs from
# it, so a session must not be the thing that rewrites it. The stamp makes the
# cached container skip this entirely when the lockfile has not moved.
# npm 11 warns that esbuild's postinstall was not run. It does not matter here:
# the linux-x64 binary arrives as an optional dependency and install.js is only
# the fallback for when that did not resolve -- checked, esbuild --version works.
say "web/node_modules"
stamp="web/node_modules/.session-start-lock-hash"
lock_hash="$(sha256sum web/package-lock.json | cut -d' ' -f1)"
if [ -d web/node_modules ] && [ "$(cat "$stamp" 2>/dev/null || true)" = "$lock_hash" ]; then
  echo "up to date with package-lock.json"
else
  (cd web && npm ci --no-audit --no-fund)
  echo "$lock_hash" > "$stamp"
fi

# ------------------------------------------------------------------- chromium
# The container has one pre-installed Chromium and PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD
# set, so when its revision is not the one @playwright/test pins there is nothing
# to install -- point the suite at what is here instead (web/playwright.config.ts
# reads IVAN_CHROMIUM). Revisions matching, this stays unset and Playwright uses
# its own.
say "chromium (npm run e2e)"
chromium_path=""
pw_rev="$(python3 -c "
import json
try:
    d = json.load(open('web/node_modules/playwright-core/browsers.json'))
    print(next(b['revision'] for b in d['browsers'] if b['name'] == 'chromium'))
except Exception:
    print('')
" 2>/dev/null || true)"
pw_dir="${PLAYWRIGHT_BROWSERS_PATH:-$HOME/.cache/ms-playwright}"
if [ -n "$pw_rev" ] && [ -d "$pw_dir/chromium-$pw_rev" ]; then
  echo "playwright's own chromium-$pw_rev is present"
elif [ -x "$pw_dir/chromium" ]; then
  chromium_path="$pw_dir/chromium"
  echo "pinned revision ${pw_rev:-?} absent; using $chromium_path"
else
  echo "no usable chromium found -- npm run e2e will not run" >&2
fi

# ---------------------------------------------------------------------- emsdk
say "emsdk $emsdk_version (WASM targets)"
if [ "$(cat "$emsdk_dir/.session-start-version" 2>/dev/null || true)" = "$emsdk_version" ]; then
  echo "already installed"
else
  rm -rf "$emsdk_dir"
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$emsdk_dir"
  "$emsdk_dir/emsdk" install "$emsdk_version"
  "$emsdk_dir/emsdk" activate "$emsdk_version"
  echo "$emsdk_version" > "$emsdk_dir/.session-start-version"
fi

# emsdk_env.sh sets EMSDK and EMSDK_NODE and prepends three PATH entries, one of
# them the SDK's own bundled node. That node is what find_program(IVAN_NODE)
# would pick for the browser build's esbuild step, so .nvmrc's node goes back in
# front of it afterwards -- the two agree at 24.19.0 today and the day emsdk
# bumps its bundle they would not.
# shellcheck disable=SC1091
. "$emsdk_dir/emsdk_env.sh" >/dev/null 2>&1
PATH="$(printf '%s' "$node_bin:$PATH" | awk -v RS=: -v ORS=: '!seen[$0]++' | sed 's/:$//')"
export PATH
echo "emcc $(emcc -dumpversion)"

# -------------------------------------------------------------- emscripten ports
# -sUSE_SDL=2 / -sUSE_SDL_MIXER=2 / -sUSE_LIBPNG=1 (CMakeLists.txt:165) are not in
# the SDK: emcc fetches each port's source the first time it links one. Building
# them here puts the sources and the built libraries in the container's cached
# state, so later WASM builds need no network at all.
#
# On Claude Code on the web the github.com archive endpoint answers 403 for these,
# so the helper clones the refused repos at the tag emcc asked for instead -- see
# its docstring for why that is the mechanism that works. It reports and returns
# non-zero if a port still cannot be built; that must not fail the session, since
# the native build and web/ do not depend on any of it.
say "emscripten ports (SDL2, SDL2_mixer, libpng)"
"$repo/.claude/hooks/seed-emscripten-ports.py" || true

# ------------------------------------------------------- persist for the session
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  {
    echo "export NVM_DIR=\"$NVM_DIR\""
    echo "export EMSDK=\"$EMSDK\""
    echo "export EMSDK_NODE=\"$EMSDK_NODE\""
    echo "export PATH=\"$PATH\""
  } >> "$CLAUDE_ENV_FILE"
  if [ -n "$chromium_path" ]; then
    echo "export IVAN_CHROMIUM=\"$chromium_path\"" >> "$CLAUDE_ENV_FILE"
  fi
fi

say "ready"
printf 'node %s | emcc %s | cmake %s\n' \
  "$(node --version)" "$(emcc -dumpversion)" \
  "$(cmake --version | head -1 | cut -d' ' -f3)"
