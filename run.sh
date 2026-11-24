#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ ${NATIVE_ARM_REQUIRED:-1} == 1 ]]; then
  [[ $(uname -m) == aarch64 ]]
fi
cpu=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
mkdir -p results
{
  lscpu
  printf 'Pinned CPU: %s\n' "$cpu"
} | tee results/machine.txt

revision=e95b8cd969b0dc14a71d9c61665d3092b7b0fbcd
folly_version=v2026.09.07.00
if [[ -z ${VELOX_SOURCE:-} ]]; then
  VELOX_SOURCE=$PWD/source
  subdir=velox/dwio/parquet/reader
  mkdir -p "$VELOX_SOURCE/$subdir"
  for file in BooleanDecoder.h BooleanDecoder.cpp; do
    curl --fail --silent --show-error --location --retry 3 \
      "https://raw.githubusercontent.com/HappenLee/velox/$revision/$subdir/$file" \
      --output "$VELOX_SOURCE/$subdir/$file"
  done
fi
if [[ -z ${FOLLY_SOURCE:-} ]]; then
  FOLLY_SOURCE=$PWD/folly
  archive=folly.tar.gz
  url="https://github.com/facebook/folly/releases/download/$folly_version/folly-$folly_version.tar.gz"
  curl --fail --silent --show-error --location --retry 3 "$url" --output "$archive"
  printf '%s  %s\n' 'f114713009738faa49961aeaa050592a09a4c8a7b46ba9a0780d4aaba468173b' "$archive" | sha256sum -c
  mkdir -p "$FOLLY_SOURCE"
  tar -xzf "$archive" --strip-components=1 -C "$FOLLY_SOURCE"
fi
printf 'Velox revision: %s\nFolly release: %s\n' "$revision" "$folly_version" | tee results/sources.txt
flags=(-O3 -DNDEBUG -std=c++20 -DFOLLY_NO_CONFIG -Icompat -I"$VELOX_SOURCE" -isystem "$FOLLY_SOURCE")
if [[ $(uname -m) == aarch64 ]]; then
  flags+=(-march=armv8-a)
fi
decoder="$VELOX_SOURCE/velox/dwio/parquet/reader/BooleanDecoder.cpp"
for compiler in "$@"; do
  compiler_name=$(basename "$compiler")
  mkdir -p "results/$compiler_name"
  "$compiler" --version | tee "results/$compiler_name/compiler.txt"
  "$compiler" "${flags[@]}" -c "$decoder" -o decoder.o
  "$compiler" "${flags[@]}" benchmark.cpp decoder.o -o benchmark
  objdump -dC decoder.o > "results/$compiler_name/decoder.asm"
  taskset -c "$cpu" stdbuf -oL ./benchmark | tee "results/$compiler_name/timings.txt"
done
