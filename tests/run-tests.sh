#!/usr/bin/env sh
# Builds and runs the bridge's desktop tests with AddressSanitizer and UndefinedBehaviorSanitizer.
#   tests/run-tests.sh          unit tests
#   tests/run-tests.sh fuzz     unit tests, then 60 s of fuzzing (needs clang); FUZZ_SECONDS=600 for longer
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
out="$root/obj/tests"
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
flags="-std=c++17 -g -O1 -Wall -Wextra -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined"

echo "Building tests with $cxx"
"$cxx" $flags "$here/BridgeTests.cpp" "$root/src/BridgeCore.cpp" -o "$out/bridge-tests"
"$out/bridge-tests"

if [ "$1" = "fuzz" ]; then
  echo "Fuzzing for ${FUZZ_SECONDS:-60} s"
  clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined "$here/Fuzz.cpp" "$root/src/BridgeCore.cpp" -o "$out/bridge-fuzz"
  mkdir -p "$out/corpus"
  cd "$out"
  ./bridge-fuzz -max_total_time="${FUZZ_SECONDS:-60}" -max_len=1100 -print_final_stats=1 "$out/corpus" "$here/corpus"
fi
