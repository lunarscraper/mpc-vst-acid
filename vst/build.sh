#!/usr/bin/env bash
# Build Acid as a VST2 plugin for the MPC OS plugin host (armhf).
#   vst/build/acid.so, vst/build/skin/  packaged together as ONE plugin folder in /sdcard/Synths (tools/release.py)
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
# vst.json/module.json only feed mpc-vst-plugins' tools/gen_vst.py for params.h + the
# skin (this isn't a Schwung DSP quick-start port -- see docs/PORTING.md classification
# 0, "MIDI generator"): the plugin itself is acid_vst.cpp, which links acid_core.c
# directly and does its own ALSA seq MIDI output, so it's built here, not via
# tools/build_port.sh (steps 1-2 below mirror what that script does for the skin).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-../../mpc-vst}"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer: the browser one (vst.json "art": "html" -- docs/SKIN_STUDIO.md),
# so skin.css/@font-face and images/acid_top.svg's inline SVG can be used for the redone skin.
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null

# 2. params.h, skin, pluginlist-entry.xml
docker run --rm -u "$U" -e HOME=/tmp -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst.json

cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper

# 3. the plugin (armhf, glibc 2.31 (bullseye) so it loads on MPC OS 2.x (2.32) and 3.x (2.39))
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  mkdir -p build/obj
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -I../src -c ../src/acid_core.c -o build/obj/core.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I../src -Ibuild -c acid_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/acid.so build/obj/core.o build/obj/vst.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -lm
  strip build/acid.so
  echo "-- exported --"; readelf --dyn-syms -W build/acid.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/acid.so | grep NEEDED
  echo "-- highest glibc (must be <= 2.32) --"; readelf -V build/acid.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/acid.so
