#!/usr/bin/env bash
# Build and run every pure-logic unit test of the fork (no AzerothCore needed). Usage: bash tests/unit/run.sh
# On macOS the newest Command Line Tools SDK can fail to link; then the newest older SDK that links is used.
# SDKROOT, if set, is respected as is.
set -uo pipefail
cd "$(dirname "$0")"
mkdir -p bin
CXX=${CXX:-c++}

if [ -z "${SDKROOT:-}" ] && [ "$(uname)" = Darwin ]; then
  probe=$(mktemp -d)
  echo 'int main() { return 0; }' > "$probe/p.cpp"
  if ! "$CXX" -std=c++20 -o "$probe/p" "$probe/p.cpp" 2>/dev/null; then
    for sdk in $(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.sdk 2>/dev/null | sort -rV); do
      if SDKROOT=$sdk "$CXX" -std=c++20 -o "$probe/p" "$probe/p.cpp" 2>/dev/null; then
        export SDKROOT=$sdk; echo "note: default SDK does not link, using $sdk"; break
      fi
    done
  fi
  rm -rf "$probe"
fi

failed=0
for t in test_*.cpp; do
  name=${t%.cpp}
  if ! "$CXX" -std=c++20 -Wall -Wextra -Werror -O0 -g -o "bin/$name" "$t"; then
    echo "UNIT BUILD FAILED $name"; failed=$((failed + 1)); continue
  fi
  if "./bin/$name"; then echo "PASS $name"; else echo "FAIL $name"; failed=$((failed + 1)); fi
done
if [ "$failed" -eq 0 ]; then echo "UNIT ALL PASS"; exit 0; fi
echo "UNIT $failed FAILED"; exit 1
