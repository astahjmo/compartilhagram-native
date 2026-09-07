#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$project_dir/build/hardware-sdk}"
engine_revision=30d5e63ae91da483e06577b5c35ee91cc5e5c3db
wrapper_revision=557808919a58fb93b0569d7e0d149312db637a91
depot_revision=4bf5898c4b96e8f35b9912a2d65417ff7d8dcfce
pkg-config --exists libavcodec libavutil libavformat
ffmpeg_include="$(pkg-config --variable=includedir libavcodec)"
mkdir -p "$build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"
checkout() {
  local url="$1" revision="$2" destination="$3"
  if [[ ! -d "$destination/.git" ]]; then
    git init "$destination"
    git -C "$destination" remote add origin "$url"
    git -C "$destination" fetch --depth 1 origin "$revision"
    git -C "$destination" checkout --detach FETCH_HEAD
  fi
  if [[ "$(git -C "$destination" rev-parse HEAD)" != "$revision" ]]; then
    echo "Unexpected revision in $destination; use a separate empty build directory." >&2
    exit 1
  fi
}
checkout https://chromium.googlesource.com/chromium/tools/depot_tools.git "$depot_revision" "$build_dir/depot_tools"
checkout https://github.com/webrtc-sdk/webrtc.git "$engine_revision" "$build_dir/src"
export PATH="$build_dir/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0
if [[ ! -f "$build_dir/.compartilhagram-deps-synced" ]]; then
  cat > "$build_dir/.gclient" <<CONFIG
solutions = [{
  'name': 'src',
  'url': 'https://github.com/webrtc-sdk/webrtc.git@$engine_revision',
  'deps_file': 'DEPS', 'managed': False, 'custom_deps': {},
  'custom_vars': {'checkout_android': False, 'checkout_ios': False},
}]
target_os = ['linux']
CONFIG
  (cd "$build_dir" && gclient sync --no-history --shallow --nohooks -j8)
  touch "$build_dir/.compartilhagram-deps-synced"
fi
checkout https://github.com/webrtc-sdk/libwebrtc.git "$wrapper_revision" "$build_dir/src/libwebrtc"
python3 "$project_dir/sdk/prepare.py" "$build_dir/src" --ffmpeg-include "$ffmpeg_include"
mkdir -p "$build_dir/src/out/Compartilhagram"
cp "$project_dir/sdk/args.gn" "$build_dir/src/out/Compartilhagram/args.gn"
(cd "$build_dir/src" && ./buildtools/linux64/gn gen out/Compartilhagram --script-executable=python3)
ninja -C "$build_dir/src/out/Compartilhagram" libwebrtc -j"${COMPARTILHAGRAM_BUILD_JOBS:-16}"
output="$build_dir/linux-x64-system"
mkdir -p "$output/lib"
cp -a "$build_dir/src/libwebrtc/include" "$output/"
cp "$build_dir/src/out/Compartilhagram/libwebrtc.so" "$output/lib/"
cp "$build_dir/src/libwebrtc/LICENSE" "$output/"
printf 'engine=%s\nwrapper=%s\ndepot_tools=%s\n' "$engine_revision" "$wrapper_revision" "$depot_revision" > "$output/build-revisions.txt"
# Keep the system driver and FFmpeg libraries external; use the distribution's
# updates and library ABI checks rather than bundling GPU drivers.
printf '\nSDK ready: %s\n' "$output"
printf 'Configure the app with -DWEBRTC_ROOT=%s -DDOWNLOAD_WEBRTC=OFF\n' "$output"
