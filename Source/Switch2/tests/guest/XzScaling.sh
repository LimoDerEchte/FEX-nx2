#!/bin/bash
# Multithreaded workload timing (multithreading Task 4): xz and sort, each with 1 and 4 threads.
# xz compresses 2 MiB in 512 KiB blocks (four blocks, so -T4 can use four workers); sort sorts 400,000 numbers.
# Launched as xz-scaling.elf and, with the EXPERIMENT sidecars, as xz-scaling-no-tso.elf and xz-scaling-profile.elf.
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

Numbers=/tmp/sort-scaling-input
seq 400000 | shuf > "$Numbers"
for Threads in 1 4; do
  Start=$(date +%s%N)
  sort -n --parallel="$Threads" "$Numbers" > /dev/null
  End=$(date +%s%N)
  echo "sort --parallel=$Threads: $(( (End - Start) / 1000000 )) ms"
done
rm -f "$Numbers"
