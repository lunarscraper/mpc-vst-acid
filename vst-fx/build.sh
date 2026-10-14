#!/usr/bin/env bash
# Build Acid FX: the insert-effect variant of ../vst (same sources, see ../vst/build.sh).
#   vst-fx/build/acid_fx.so, vst-fx/build/skin/  packaged together as ONE plugin folder in /sdcard/Synths
#   vst-fx/build/pluginlist-entry.xml             the <PLUGIN ... category="Effect"> line
# Only vst.json differs ("effect": true -> PLUG_EFFECT in params.h); acid_vst.cpp, module.json,
# layout.conf, skin.css, images/ and fonts/ are taken from ../vst.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer: the browser one, as for ../vst
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null

# 2. params.h (with PLUG_EFFECT), skin, pluginlist-entry.xml. The whole repo is mounted, since the
# layout and its artwork live in ../vst. SHADOW_SKIN_MPC_OS=2 (set by the release workflow) as for ../vst.
docker run --rm -u "$U" -e HOME=/tmp ${SHADOW_SKIN_MPC_OS:+-e SHADOW_SKIN_MPC_OS="$SHADOW_SKIN_MPC_OS"} \
  -v "$PWD/..":/w -v "$MPC_VST":/mv:ro -w /w/vst-fx mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst.json
grep -q "PLUG_EFFECT" build/params.h || { echo "params.h has no PLUG_EFFECT: this would build the instrument"; exit 1; }
grep -q 'category="Effect"' build/pluginlist-entry.xml || { echo "plugin-list entry is not an Effect"; exit 1; }

cp "$MPC_VST/wrapper/popup.h" build/

# 3. the plugin (armhf, glibc 2.31 (bullseye) as ../vst, so it loads on MPC OS 2.x and 3.x)
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst-fx arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  mkdir -p build/obj
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -I../src -c ../src/acid_core.c -o build/obj/core.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I../src -Ibuild -c ../vst/acid_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/acid_fx.so build/obj/core.o build/obj/vst.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -lm -Wl,--no-undefined
  strip build/acid_fx.so
  echo "-- exported --"; readelf --dyn-syms -W build/acid_fx.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/acid_fx.so | grep NEEDED
  echo "-- highest glibc (must be <= 2.32) --"; readelf -V build/acid_fx.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/acid_fx.so
