#!/bin/bash
#
# Runs the emulator's tests: every tests/*.asm is assembled into a boot floppy of its own and booted
# headless with host/pctool. A test's "; pctool:" lines are its pctool arguments, one per line (so
# typed text may contain spaces): the checks (expect=, framesum=) and the time the run ends.
# pctool exits with 1 when a check fails.
#
#   make -C host && tests/run.sh [name ...]
#
# Needs nasm. The images and the logs go to tests/build/. On a failure the log is printed.

cd "$(dirname "$0")"
PCTOOL=../host/pctool
mkdir -p build

if [ $# -gt 0 ]; then
  tests=("$@")
else
  tests=()
  for f in *.asm; do tests+=("${f%.asm}"); done
fi

failed=0
for name in "${tests[@]}"; do
  img=build/$name.img
  if ! nasm -f bin -o build/$name.bin $name.asm; then
    echo "FAIL $name (does not assemble)"
    failed=$((failed + 1))
    continue
  fi
  dd if=/dev/zero of=$img bs=512 count=2880 status=none
  dd if=build/$name.bin of=$img conv=notrunc status=none

  args=()
  while IFS= read -r line; do
    args+=("$line")
  done < <(sed -n 's/^; pctool: *//p' $name.asm)

  if $PCTOOL run fd0=$img "${args[@]}" > build/$name.log 2>&1; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    sed 's/^/     /' build/$name.log
    failed=$((failed + 1))
  fi
done

echo "${#tests[@]} tests, $failed failed"
[ $failed -eq 0 ]
