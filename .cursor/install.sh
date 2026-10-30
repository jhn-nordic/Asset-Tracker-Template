#!/usr/bin/env bash
set -euo pipefail

export PATH="/usr/local/bin:${HOME}/.local/bin:${PATH}"
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

if ! command -v west >/dev/null; then
  pip install --user west
  sudo ln -sf "${HOME}/.local/bin/west" /usr/local/bin/west
fi

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  git wget cmake ninja-build gperf ccache dfu-util device-tree-compiler \
  python3-dev python3-pip python3-setuptools python3-wheel xz-utils file make \
  gcc g++ gcc-multilib g++-multilib build-essential libc6-dev \
  libsdl2-dev libmagic1 ruby curl

if [ ! -x "${ZEPHYR_SDK_INSTALL_DIR}/setup.sh" ]; then
  SDK_VERSION=0.17.0
  wget -q "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VERSION}/zephyr-sdk-${SDK_VERSION}_linux-x86_64.tar.xz" \
    -O /tmp/zephyr-sdk.tar.xz
  sudo tar xf /tmp/zephyr-sdk.tar.xz -C /opt
  yes | sudo "${ZEPHYR_SDK_INSTALL_DIR}/setup.sh" -t all -h -c
fi

WORKSPACE_ROOT=/opt/zephyr-workspace
ATT_DIR="${WORKSPACE_ROOT}/asset-tracker-template"

sudo mkdir -p "${ATT_DIR}"
if ! mountpoint -q "${ATT_DIR}"; then
  sudo mount --bind /workspace "${ATT_DIR}"
fi
sudo chown -R ubuntu:ubuntu "${WORKSPACE_ROOT}"

cd "${ATT_DIR}"
if [ ! -d "${WORKSPACE_ROOT}/.west" ]; then
  west init -l .
fi
west config manifest.group-filter +bsec
west config build.sysbuild True
west update -o=--depth=1 -n

pip install --user -r "${WORKSPACE_ROOT}/nrf/scripts/requirements-build.txt"
pip install --user -r "${WORKSPACE_ROOT}/zephyr/scripts/requirements.txt"
pip install --user requests

west blobs fetch hal_nordic
