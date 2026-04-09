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

# Run the command inside the container
docker compose -f "$COMPOSE_FILE" run -u "$MY_UID" --rm -v "$PWD:$CONTAINER_WORK_DIR" $SERVICE_NAME "$TOOL_NAME" "$@"
