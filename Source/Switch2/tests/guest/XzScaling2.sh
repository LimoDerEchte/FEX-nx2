#!/bin/bash
# EXPERIMENT (multithreading scaling): narrows down why xz -T4 is slow. Each step prints its time; a step that takes
# over 120 s is cut off by timeout (exit 124).
Input=/tmp/xz-scaling2
cat /usr/bin/perl /usr/lib/x86_64-linux-gnu/libc.so.6 | head -c 2097152 > "$Input"
head -c 524288 "$Input" > "$Input.0"
for Part in 1 2 3; do tail -c +$((Part * 524288 + 1)) "$Input" | head -c 524288 > "$Input.$Part"; done
Ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }

Start=$(date +%s%N); timeout 120 xz -6 -T1 -c "$Input.0" > /dev/null; echo "a) one xz -6 on 512 KiB: $(Ms $Start) ms (exit $?)"
Start=$(date +%s%N)
for Part in 0 1 2 3; do timeout 120 xz -6 -T1 -c "$Input.$Part" > /dev/null & done
wait
echo "b) four xz -6 processes on 512 KiB each, in parallel: $(Ms $Start) ms"
Start=$(date +%s%N); timeout 120 xz -6 -T2 --block-size=512KiB -c "$Input.0" "$Input.1" > /dev/null; echo "c) xz -6 -T2 on 1 MiB (two blocks): $(Ms $Start) ms"
Start=$(date +%s%N); timeout 120 xz -1 -T1 --block-size=512KiB -c "$Input" > /dev/null; echo "d) xz -1 -T1 on 2 MiB: $(Ms $Start) ms"
Start=$(date +%s%N); timeout 120 xz -1 -T4 --block-size=512KiB -c "$Input" > /dev/null; echo "e) xz -1 -T4 on 2 MiB: $(Ms $Start) ms"
rm -f "$Input" "$Input".?
