#!/bin/bash

# Multi-call wrapper: runs the tool it is invoked as (bin/openFPGALoader)
# inside the locally built image (make openfpgaloader-image).

CONTAINER_WORK_DIR="/work"

# Wrapper script dir
SCRIPT_DIR=$(dirname "$0")/..
. "$SCRIPT_DIR/image.env"

TOOL_NAME=$(basename "$0")

# Stopping this wrapper -- a `timeout`, a Ctrl-C -- must not leave the
# tool running with nothing attached. Name the container so it can be
# reached, and kill it on the way out.
CONTAINER_NAME="ofl-$TOOL_NAME-$$"
docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
trap 'docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true; exit 143' TERM INT HUP

# USB without --privileged: the host's /dev/bus/usb, and a device rule
# admitting only USB device nodes (major 189). Mounted rather than
# passed as --device, so a board that re-enumerated after a power cycle
# is visible too. Opening a node still needs the host's permission for
# it (a udev rule; see DEVELOP.md). No network: programming needs none.
# Backgrounded, because a trap does not run while the shell waits on a
# foreground child.
docker run --rm --network none --name "$CONTAINER_NAME" \
    -u "$(id -u):$(id -g)" \
    --device-cgroup-rule 'c 189:* rmw' \
    -v /dev/bus/usb:/dev/bus/usb \
    -v "$PWD:$CONTAINER_WORK_DIR" -w "$CONTAINER_WORK_DIR" \
    "$OFL_IMAGE" "$TOOL_NAME" "$@" &
wait $!
