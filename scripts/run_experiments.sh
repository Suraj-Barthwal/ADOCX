#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS_DIR="${ROOT_DIR}/results"
mkdir -p "${RESULTS_DIR}"

"${ROOT_DIR}/scripts/build.sh"

"${ROOT_DIR}/build/adocx_benchmark" \
  --keys 10000 \
  --ops 200000 \
  --read-ratio 0.8 \
  --seed 7 \
  --out "${RESULTS_DIR}/adoc_reproducible.csv"

echo "Reproducible experiment finished: ${RESULTS_DIR}/adoc_reproducible.csv"
