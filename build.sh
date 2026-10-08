#!/usr/bin/env bash
# Build the firmware inside a docker container so no toolchain is needed on the
# host. Output lands in firmware/ (firmware.elf, firmware.bin).
#
#   ./build.sh              # build firmware.bin
#   ./build.sh clean        # other arguments are passed through to make
#   ./build.sh --rebuild    # force a rebuild of the toolchain image first
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="rp2350-pacemaker-build"

REBUILD=
MAKE_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --rebuild) REBUILD=1 ;;
        *) MAKE_ARGS+=("$arg") ;;
    esac
done

if [[ -n "$REBUILD" ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -t "$IMAGE" -f "$ROOT/Dockerfile.build" "$ROOT"
fi

# Run as the host user so build outputs aren't root-owned on Linux hosts.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT:/src" \
    "$IMAGE" ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"}
