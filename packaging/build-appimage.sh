#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(cd -- "${1:-$project_dir/build}" && pwd)"
[[ $(uname -sm) == 'Linux x86_64' ]] || { echo 'AppImage packaging requires Linux x86_64.' >&2; exit 1; }
tools_dir="$build_dir/appimage-tools"
mkdir -p "$tools_dir"
fetch_tool() {
  local repo="$1" filename="$2" checksum="$3"
  if [[ ! -f "$tools_dir/$filename" ]]; then
    curl --fail --location --retry 3 "https://github.com/linuxdeploy/$repo/releases/download/continuous/$filename" -o "$tools_dir/$filename.download"
    printf '%s  %s\n' "$checksum" "$tools_dir/$filename.download" | sha256sum --check
    mv "$tools_dir/$filename.download" "$tools_dir/$filename"
  fi
  printf '%s  %s\n' "$checksum" "$tools_dir/$filename" | sha256sum --check
  chmod +x "$tools_dir/$filename"
}
# Continuous release URLs can move; fail closed if the reviewed tools change.
fetch_tool linuxdeploy linuxdeploy-x86_64.AppImage 36a2d7e274d12e1050d0e9ecfe11d339ed54720b2bec464c286d53f8b07f5c62
fetch_tool linuxdeploy-plugin-qt linuxdeploy-plugin-qt-x86_64.AppImage cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617
# Use a fresh staging directory, so removed dependencies cannot leak into releases.
stage="$(mktemp -d "$build_dir/appimage-stage.XXXXXX")"
appdir="$stage/AppDir"
DESTDIR="$appdir" cmake --install "$build_dir" --prefix /usr

# PipeWire loads these client modules and SPA plugins with dlopen, so dependency
# scanning alone cannot discover them. Keep their directory structure intact.
module_dir="$(pkg-config --variable=moduledir libpipewire-0.3)"
spa_dir="$(pkg-config --variable=plugindir libspa-0.2)"
mkdir -p "$appdir/usr/lib/pipewire-0.3" "$appdir/usr/lib/spa-0.2" "$appdir/usr/share/pipewire"
for module in rt protocol-native client-node client-device adapter metadata session-manager; do
  cp -L "$module_dir/libpipewire-module-$module.so" "$appdir/usr/lib/pipewire-0.3/"
done
for plugin in support audioconvert videoconvert; do
  cp -a "$spa_dir/$plugin" "$appdir/usr/lib/spa-0.2/"
done
cp "$(pkg-config --variable=prefix libpipewire-0.3)/share/pipewire/client.conf" "$appdir/usr/share/pipewire/"
mkdir -p "$appdir/apprun-hooks"
cat > "$appdir/apprun-hooks/pipewire.sh" <<'HOOK'
export PIPEWIRE_MODULE_DIR="$APPDIR/usr/lib/pipewire-0.3"
export SPA_PLUGIN_DIR="$APPDIR/usr/lib/spa-0.2"
export PIPEWIRE_CONFIG_DIR="$APPDIR/usr/share/pipewire"
HOOK

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6)}"
qt_plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
plugin_dependency_args=()
for plugin in platforms tls wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
  mkdir -p "$appdir/usr/plugins/$plugin"
  cp -a "$qt_plugins/$plugin/." "$appdir/usr/plugins/$plugin/"
  # linuxdeploy scans only the immediate contents of each supplied directory.
  plugin_dependency_args+=(--deploy-deps-only "$appdir/usr/plugins/$plugin")
done
# Qt's OpenSSL backend loads these libraries at runtime on some distributions.
ssl_dir="$(pkg-config --variable=libdir openssl)"
pipewire_libdir="$(pkg-config --variable=libdir libpipewire-0.3)"
sm_libdir="$(pkg-config --variable=libdir sm)"
ice_libdir="$(pkg-config --variable=libdir ice)"
export NO_STRIP=1
export OUTPUT="$stage/Compartilhagram-x86_64.AppImage"
cd "$stage"
"$tools_dir/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
  "${plugin_dependency_args[@]}" \
  --library "$ssl_dir/libssl.so" --library "$ssl_dir/libcrypto.so" \
  --library "$pipewire_libdir/libpipewire-0.3.so.0" \
  --library "$sm_libdir/libSM.so.6" --library "$ice_libdir/libICE.so.6" \
  --plugin qt --output appimage
python3 "$project_dir/packaging/verify-appdir.py" "$appdir"
mv "$OUTPUT" "$build_dir/Compartilhagram-x86_64.AppImage"
OUTPUT="$build_dir/Compartilhagram-x86_64.AppImage"
printf '\nSingle-file application: %s\nStaging directory: %s\n' "$OUTPUT" "$appdir"
