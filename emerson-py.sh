#!/usr/bin/env bash
# Run one of this repo's Python host tools inside the emerson-server container.
#
# The `emerson` Python module exists only in that container, and the container
# mounts nothing from this repo but flash.bin and peripherals.yaml. So this
# copies programmer/ and scenarios/ into the container, runs the command there,
# and copies any *.html the run wrote back into the current directory.
#
#   ./emerson-py.sh programmer/pacemaker_programmer.py
#   ./emerson-py.sh scenarios/accolade_safety_mode.py --no-harm
#   ./emerson-py.sh -m pytest -q programmer
#
# Each run gets its own directory, /tmp/emerson-py.XXXXXX, removed afterwards,
# so several can run at once.
#
# Needs a running container (`emerson load firmware/firmware.bin`). The
# programmer also needs a session (`emerson start`); the scenarios start their
# own.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(emerson exec mktemp -d /tmp/emerson-py.XXXXXX | tr -d '\r')"
trap 'emerson exec rm -rf "$WORK"' EXIT

# --no-xattrs (bsdtar) keeps macOS metadata out of the archive.
COPYFILE_DISABLE=1 tar -C "$ROOT" --no-xattrs -cf - --exclude=__pycache__ --exclude='.pytest_cache' \
        --exclude='pacemaker-interrogation*.html' programmer scenarios \
    | emerson exec tar -xf - -C "$WORK"

status=0
emerson exec bash -c "cd $WORK && exec env PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 python3 \"\$@\"" python3 "$@" || status=$?

# Bring back any report the run wrote (e.g. pacemaker-interrogation.html).
emerson exec bash -c "cd $WORK && find . -maxdepth 1 -name '*.html' -newer programmer/live_view.html -print0 \
    | xargs -0r tar -cf -" | tar -xf - -C "$PWD" 2>/dev/null || true

exit $status
