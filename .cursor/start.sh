#!/usr/bin/env bash
set -euo pipefail

WORKSPACE_ROOT=/opt/zephyr-workspace
ATT_DIR="${WORKSPACE_ROOT}/asset-tracker-template"

sudo mkdir -p "${ATT_DIR}"
if ! mountpoint -q "${ATT_DIR}"; then
  sudo mount --bind /workspace "${ATT_DIR}"
fi
