#!/usr/bin/env bash
cmake_args=()
if [[ -n "${CONDA_PREFIX:-}" ]]
then
    cmake_args+=("-DCMAKE_PREFIX_PATH=${CONDA_PREFIX}")
elif [[ -d "$HOME/anaconda3" ]]
then
    cmake_args+=("-DCMAKE_PREFIX_PATH=$HOME/anaconda3")
fi

if [[ "$#" -ne 0 && $1 == "debug" ]]
then
    mkdir -p build_debug;
    cd build_debug;
    cmake -DCMAKE_BUILD_TYPE=Debug "${cmake_args[@]}" ..;
else
    mkdir -p build;
    cd build;
    cmake -DCMAKE_BUILD_TYPE=Release "${cmake_args[@]}" ..;
fi
make;
cd ..;
