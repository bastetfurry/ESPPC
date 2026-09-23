#!/bin/sh
#
# Rebuilds src/biosrom.h from bios.asm. Requires nasm and xxd.
# Only needed after editing bios.asm - biosrom.h is checked in.

set -e
cd "$(dirname "$0")"

nasm bios.asm
xxd -i -c 256 bios | sed '1d;$d' | sed '$d' > ../src/biosrom.h
rm bios
