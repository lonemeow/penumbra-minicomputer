#!/bin/bash

# Configuration
SERVICE_NAME="fpga-dev"
CONTAINER_WORK_DIR="/work"

# Wrapper script dir
SCRIPT_DIR=$(dirname "$0")/..
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

# Get the name the script was called as (e.g., 'yosys' or 'fujprog')
TOOL_NAME=$(basename "$0")

MY_UID=$(id -u)

# Compose passes no signal through to the tool, so stopping this wrapper -- a
# `timeout`, a Ctrl-C -- would leave the tool running with nothing attached.
# Name the container so it can be reached, and stop it on the way out; a caller
# then only has to signal the wrapper, and needs to know nothing about how the
# tool is packaged.
CONTAINER_NAME="ocs-$TOOL_NAME-$$"
docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
trap 'docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true; exit 143' TERM INT HUP

# Backgrounded, because a trap does not run while the shell waits on a
# foreground child.
docker compose -f "$COMPOSE_FILE" run -u "$MY_UID" --rm --name "$CONTAINER_NAME" \
    -v "$PWD:$CONTAINER_WORK_DIR" $SERVICE_NAME "$TOOL_NAME" "$@" &
wait $!
