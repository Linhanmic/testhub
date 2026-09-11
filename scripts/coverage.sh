#!/usr/bin/env bash
# 用 GCC gcov 生成覆盖率摘要。在仓库根目录运行：
#   ./scripts/coverage.sh
# 可选：COVERAGE_DIR=build-cov CMAKE_CXX_COMPILER=g++
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD="${COVERAGE_DIR:-build-cov}"
COMPILER="${CMAKE_CXX_COMPILER:-g++}"

cmake -S . -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$COMPILER" \
  -DTESTHUB_ENABLE_COVERAGE=ON \
  -DTESTHUB_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD" --parallel
ctest --test-dir "$BUILD" --output-on-failure --timeout 180

if command -v gcovr >/dev/null 2>&1; then
  gcovr -r "$ROOT" "$BUILD" \
    --exclude '.*/tests/.*' \
    --exclude '.*/generated/.*' \
    --exclude '.*/runners/.*' \
    --print-summary
elif command -v lcov >/dev/null 2>&1; then
  lcov --capture --directory "$BUILD" --output-file "$BUILD/coverage.info" --gcov-tool gcov --quiet
  lcov --remove "$BUILD/coverage.info" '/usr/*' '*/tests/*' '*/generated/*' --output-file "$BUILD/coverage.info" --quiet
  lcov --list "$BUILD/coverage.info"
else
  echo "Install gcovr (pip install gcovr) or lcov to print a coverage summary." >&2
  echo "Object files with coverage notes:" >&2
  find "$BUILD" -name '*.gcda' | wc -l
fi
