#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if [[ "${1:-}" == "--clean" ]]; then
	rm -rf build
fi

cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -j"$(nproc)"