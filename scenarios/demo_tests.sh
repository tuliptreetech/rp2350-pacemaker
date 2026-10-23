#!/bin/sh
# Run scenarios/test_demo_pacemaker.py inside the emerson-server container with `emerson exec`.
#
# The tests start and stop their own sessions, so they need no session of yours (`emerson run` would
# insist on one; it is only needed to publish a port, like the programmer's live view). The server
# container does not mount the repo, so the test script, the scenario it imports and the programmer are
# copied into a temporary directory there and removed afterwards. Inside the container the emerson
# library reaches the server on localhost:10314, its default.
#
#     scenarios/demo_tests.sh -q                          # every condition
#     scenarios/demo_tests.sh -q -m "not slow" --jobs 4   # any arguments go to the test script
#
# Needs the emulator up on the firmware (`emerson load firmware/firmware.bin`).
set -eu
cd "$(dirname "$0")/.."

dest=/tmp/pacemaker-demo-tests-$$
tar -cf - programmer/pacemaker_programmer.py scenarios/accolade_safety_mode.py scenarios/accolade_communicator.py \
    scenarios/test_demo_pacemaker.py |
    emerson exec sh -c "mkdir -p $dest && tar -xf - -C $dest"
emerson exec sh -c "cd $dest && python3 scenarios/test_demo_pacemaker.py \"\$@\"; s=\$?; rm -rf $dest; exit \$s" sh "$@"
