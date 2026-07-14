#!/usr/bin/env bash

cmake_repo_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

cmake_default_config_args() {
  if [[ -n "${CONDA_PREFIX:-}" ]]; then
    printf '%s\0' "-DCMAKE_PREFIX_PATH=${CONDA_PREFIX}"
  elif [[ -d "$HOME/anaconda3" ]]; then
    printf '%s\0' "-DCMAKE_PREFIX_PATH=$HOME/anaconda3"
  fi
}

cmake_configure_build_dir() {
  local build_dir="$1"
  local repo_root
  repo_root="$(cmake_repo_root)"

  mkdir -p "$build_dir"
  local build_abs
  build_abs="$(cd "$build_dir" && pwd)"
  local cache="$build_abs/CMakeCache.txt"

  if [[ -f "$cache" ]]; then
    local cached_home=""
    local cached_build=""
    cached_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | tail -n 1)"
    cached_build="$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$cache" | tail -n 1)"
    if [[ "$cached_home" != "$repo_root" || "$cached_build" != "$build_abs" ]]; then
      echo "CMake cache belongs to a different source/build directory; reconfiguring $build_dir." >&2
      echo "  cached source: ${cached_home:-<unknown>}" >&2
      echo "  current source: $repo_root" >&2
      rm -f "$cache" "$build_abs/Makefile" "$build_abs/cmake_install.cmake" "$build_abs/CTestTestfile.cmake"
      rm -rf "$build_abs/CMakeFiles"
    fi
  fi

  local cmake_args=()
  while IFS= read -r -d '' arg; do
    cmake_args+=("$arg")
  done < <(cmake_default_config_args)

  cmake -S "$repo_root" -B "$build_abs" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" "${cmake_args[@]}"
}
