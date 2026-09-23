#!/bin/bash

# Multi-call wrapper: runs the tool it is invoked as (bin/vivado) inside
# the locally built Vivado image (make vivado-image).

CONTAINER_WORK_DIR="/work"

# Wrapper script dir
SCRIPT_DIR=$(dirname "$0")/..
. "$SCRIPT_DIR/image.env"

TOOL_NAME=$(basename "$0")

# Vivado writes ~/.Xilinx and expects HOME to exist. A scratch HOME
# under build/ keeps that out of the real home directory, and means the
# host user needs no account inside the image.
HOME_DIR="build/vivado-home"
mkdir -p "$PWD/$HOME_DIR"

# Stopping this wrapper -- a `timeout`, a Ctrl-C -- must not leave the
# tool running with nothing attached. Name the container so it can be
# reached, and kill it on the way out; a caller then only has to signal
# the wrapper, and needs to know nothing about how the tool is packaged.
CONTAINER_NAME="vivado-$TOOL_NAME-$$"
docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
trap 'docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true; exit 143' TERM INT HUP

# No network: nothing in the flow may depend on anything outside the
# image. --init reaps the worker processes Vivado spawns.
# Backgrounded, because a trap does not run while the shell waits on a
# foreground child.
docker run --rm --init --network none --name "$CONTAINER_NAME" \
    -u "$(id -u):$(id -g)" \
    -e HOME="$CONTAINER_WORK_DIR/$HOME_DIR" \
    -v "$PWD:$CONTAINER_WORK_DIR" -w "$CONTAINER_WORK_DIR" \
    "$VIVADO_IMAGE" "$TOOL_NAME" "$@" &
wait $!
