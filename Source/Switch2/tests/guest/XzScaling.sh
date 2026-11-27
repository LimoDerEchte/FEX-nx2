#!/bin/bash
# EXPERIMENT (multithreading scaling): compresses the same 2 MiB with xz using 1 and 4 threads (2 MiB in 512 KiB
# blocks: four blocks, so -T4 can use four workers). Launched as xz-scaling.elf and, with the .no-tso sidecar, as
# xz-scaling-no-tso.elf.
set -e
Input=/tmp/xz-scaling-input
cat /usr/bin/perl /usr/lib/x86_64-linux-gnu/libc.so.6 | head -c 2097152 > "$Input"
for Threads in 1 4; do
  Start=$(date +%s%N)
  xz -6 -T"$Threads" --block-size=512KiB -c "$Input" > /dev/null
  End=$(date +%s%N)
  echo "xz -T$Threads: $(( (End - Start) / 1000000 )) ms"
done
rm -f "$Input"
