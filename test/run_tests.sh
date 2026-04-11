#!/usr/bin/env bash
# Build and run host-side unit tests for the pure-C modules in main/.
#
# No ESP-IDF toolchain required — just the system cc. Runs in milliseconds.
# Invoke from anywhere: ./test/run_tests.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/host-tests"
mkdir -p "${BUILD_DIR}"

CFLAGS=(
    -std=c11
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -O0
    -g
    -I"${REPO_ROOT}/main"
)

echo "Building gate_sm host tests..."
cc "${CFLAGS[@]}" \
    "${REPO_ROOT}/test/test_gate_sm.c" \
    "${REPO_ROOT}/main/gate_sm.c" \
    -o "${BUILD_DIR}/test_gate_sm"

echo "Running gate_sm host tests..."
"${BUILD_DIR}/test_gate_sm"
