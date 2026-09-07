#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
app="$(realpath "${1:-$project_dir/build/Compartilhagram-x86_64.AppImage}")"
results="${2:-$project_dir/build/archlinux-test}"
mkdir -p "$results"
results="$(realpath "$results")"
image=compartilhagram-archlinux-test
docker build --pull -t "$image" --build-arg "TEST_UID=$(id -u)" --build-arg "TEST_GID=$(id -g)" \
  -f "$project_dir/packaging/archlinux/Dockerfile" "$project_dir/packaging"
docker image inspect "$image" > "$results/container-image.json"
sha256sum "$app" > "$results/appimage.sha256"
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$app,target=/input/Compartilhagram-x86_64.AppImage,readonly" \
  --mount "type=bind,source=$results,target=/results" \
  "$image"
printf '\nArch Linux test logs: %s\n' "$results"
