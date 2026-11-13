#!/usr/bin/env bash
set -euo pipefail

sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_PAD_TEST_BUILD_DIR:-${sculpt_root}/build-tests}"
sculpt_cxx="${CXX:-c++}"

if [[ ! -f "${sculpt_root}/Source/PadEngine.h" ]]; then
  echo "Il motore Source/PadEngine.h non è ancora disponibile." >&2
  exit 1
fi
if ! command -v "$sculpt_cxx" >/dev/null 2>&1; then
  echo "Serve un compilatore C++17. Puoi selezionarlo con CXX=/percorso/del/compilatore." >&2
  exit 1
fi

mkdir -p "$sculpt_build"
"$sculpt_cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "${sculpt_root}/Tests/pad_engine_test.cpp" -o "${sculpt_build}/pad_engine_test"
"${sculpt_build}/pad_engine_test"
