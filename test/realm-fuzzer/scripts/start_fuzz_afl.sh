#!/usr/bin/env bash

SCRIPT=$(basename "${BASH_SOURCE[0]}")
ROOT_DIR=$(git rev-parse --show-toplevel)
BUILD_DIR="build.realm.fuzzer.afl"

build_mode="Debug"
num_fuzzers="1"
fuzz_test="realm-afl++"


if [ "$#" -ne 2 ]; then
    echo "Usage: ${SCRIPT} <num_fuzzers> <build_mode>"
    echo "          num_fuzzers : the number of fuzzers to run in parallel. Default ${num_fuzzers}."
    echo "          build mode  : either Debug or Release. Default ${build_mode}."
fi

if ! [[ -z "$1" ]]; then 
    num_fuzzers = "$1"
fi
if ! [[ -z "$2" ]]; then 
    build_mode = "$2"
fi

if [ "$(uname)" = "Darwin" ]; then
    # FIXME: Consider detecting if ReportCrash was already unloaded and skip this message
    #        or print and don't try to run AFL.
    echo "----------------------------------------------------------------------------------------"
    echo "Make sure you have unloaded the OS X crash reporter:"
    echo
    echo "launchctl unload -w /System/Library/LaunchAgents/com.apple.ReportCrash.plist"
    echo "sudo launchctl unload -w /System/Library/LaunchDaemons/com.apple.ReportCrash.Root.plist"
    echo "----------------------------------------------------------------------------------------"
else
    # FIXME: Check if AFL works if the core pattern is different, but does not start with | and test for that
    if [ "$(cat /proc/sys/kernel/core_pattern)" != "core" ]; then
        echo "----------------------------------------------------------------------------------------"
        echo "AFL might mistake crashes with hangs if the core is outputed to an external process"
        echo "Please run:"
        echo
        echo "sudo sh -c 'echo core > /proc/sys/kernel/core_pattern'"
        echo "----------------------------------------------------------------------------------------"
        exit 1
    fi
fi

echo "Building..."

cd "${ROOT_DIR}" || exit

mkdir -p "${BUILD_DIR}"
    
cd "${BUILD_DIR}" || exit
if [ -z ${REALM_MAX_BPNODE_SIZE} ]; then
    REALM_MAX_BPNODE_SIZE=$(python -c "import random; print ((random.randint(4,999), 1000)[bool(random.randint(0,1))])")
fi

cmake -D CMAKE_BUILD_TYPE=${build_mode} \
      -D CMAKE_C_COMPILER=afl-cc \
      -D CMAKE_CXX_COMPILER=afl-c++ \
      -D REALM_MAX_BPNODE_SIZE="${REALM_MAX_BPNODE_SIZE}" \
      -D REALM_ENABLE_ENCRYPTION=OFF \
      -G Ninja \
      ..

ninja "${fuzz_test}"

echo "Cleaning up the findings directory"

FINDINGS_DIR="findings"
EXEC=$(find . -name ${fuzz_test})

pkill afl-fuzz
rm -rf "${FINDINGS_DIR}"
mkdir -p "${FINDINGS_DIR}"

# see also stop_parallel_fuzzer.sh
time_out="1000" # ms
memory="1000" # MB

echo "Going to fuzz with AFL++: ${PWD}/${EXEC}"

# if we have only one fuzzer
if [ "${num_fuzzers}" -eq 1 ]; then
    afl-fuzz -t "$time_out" \
        -m "$memory" \
        -i "${ROOT_DIR}/test/realm-fuzzer/testcases" \
        -o "${FINDINGS_DIR}" \
        ${EXEC} @@
    exit 0
fi

# start the fuzzers in parallel
echo "Starting $num_fuzzers fuzzers in parallel"
for i in $(seq 1 ${num_fuzzers}); do
    [[ $i -eq 1 ]] && flag="-M" || flag="-S"
   afl-fuzz -t "$time_out" \
       -m "$memory" \
       -i "${ROOT_DIR}/test/realm-fuzzer/testcases" \
       -o "${FINDINGS_DIR}" \
       "${flag}" "fuzzer$i" \
       ${EXEC} @@ --name "fuzzer$i" >/dev/null 2>&1 &
done

echo
echo "Use 'afl-whatsup ${ROOT_DIR}/${BUILD_DIR}/${FINDINGS_DIR}' to check progress"
echo