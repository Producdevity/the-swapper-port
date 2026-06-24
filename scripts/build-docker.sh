#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

image="${SWAPPER_BUILD_IMAGE:-theswapper-aarch64-builder:debian-buster}"

docker build -t "$image" .
docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$PWD":/src \
  -w /src \
  "$image" \
  make clean all

file build/libs.aarch64/libwindows_stub.so \
  build/libs.aarch64/libsteam_api.so \
  build/libs.aarch64/libsdkencryptedappticket.so \
  build/libs.aarch64/libfmodex.so
