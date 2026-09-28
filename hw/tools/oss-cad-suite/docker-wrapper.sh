#!/bin/bash

# Configuration
SERVICE_NAME="fpga-dev"
CONTAINER_WORK_DIR="/work"

# Wrapper script dir
SCRIPT_DIR=$(dirname "$0")/..
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

# Get the name the script was called as (e.g., 'yosys' or 'ecppack')
TOOL_NAME=$(basename "$0")

# Your UID and GID: given only a UID, Docker runs the tool in the root
# group, with that group's access to anything group-owned by root.
MY_USER="$(id -u):$(id -g)"

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
docker compose -f "$COMPOSE_FILE" run -u "$MY_USER" --rm --name "$CONTAINER_NAME" \
    -v "$PWD:$CONTAINER_WORK_DIR" $SERVICE_NAME "$TOOL_NAME" "$@" &
wait $!
