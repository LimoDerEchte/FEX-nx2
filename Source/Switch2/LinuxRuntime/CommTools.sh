#!/bin/bash
# SPDX-License-Identifier: MIT
# Debian tool acceptance for milestone 3 (threads, futexes, pipes, sockets).
# Usage: comm-tools.sh [scratch-dir]. Exits 42 when every check passes.
set -u
export LC_ALL=C
Fail=0
check() { if eval "$2"; then echo "ok $1"; else echo "not ok $1"; Fail=1; fi; }
skip() { echo "ok $1 # SKIP $2"; }
Dir=${1:-./comm-tools.scratch}
rm -rf "$Dir"; mkdir -p "$Dir"; cd "$Dir" || exit 1

# Ports differ per run so a lingering listener cannot disturb the next run.
Base=$(( 20000 + $$ % 5000 ))
# Waits until a file holds at least $2 bytes (up to 15 s).
filled() { for _ in $(seq 150); do [ "$(wc -c < "$1" 2>/dev/null || echo 0)" -ge "$2" ] && return 0; sleep 0.1; done; return 1; }
# Runs a TCP client until it connects: a refused connection sends no data.
tcp_client() { for _ in $(seq 100); do nc -N "$@" && return 0; sleep 0.2; done; return 1; }

# 1. Threaded xz: worker threads synchronize through futexes.
# The guest has no /dev/urandom, so test data comes from seq with the digits
# permuted (deterministic, not trivially compressible).
# 8 MiB in 1 MiB blocks at -1 still gives each of the 4 threads several
# blocks; the guest runs them on one host thread, so size is kept small.
seq 1 1300000 | tr 0-9 5381940276 | head -c 8388608 > data.bin
check xz_threads_roundtrip 'xz -1 -T4 --block-size=1MiB -c data.bin > data.xz && xz -dc data.xz > data.out && [ "$(wc -c < data.out)" = 8388608 ] && [ "$(sha256sum < data.bin)" = "$(sha256sum < data.out)" ]'

# 2. Parallel sort against single-threaded sort.
seq 1 200000 | tr 0-9 5381940276 > lines.txt
check sort_parallel_matches_single 'sort --parallel=4 -S 64M -n lines.txt > sorted4.txt && sort --parallel=1 -n lines.txt > sorted1.txt && [ "$(wc -l < sorted4.txt)" = 200000 ] && [ "$(sha256sum < sorted4.txt)" = "$(sha256sum < sorted1.txt)" ]'

# 3. netcat over Unix, UDP and TCP sockets.
check nc_unix_roundtrip 'echo pong > reply.txt; timeout 20 nc -lUN s.sock < reply.txt > got.txt & S=$!; for _ in $(seq 100); do [ -S s.sock ] && break; sleep 0.1; done; echo ping | timeout 20 nc -NU s.sock > resp.txt; wait $S; [ "$(cat got.txt)" = ping ] && [ "$(cat resp.txt)" = pong ]'
check nc_udp_datagram 'timeout 20 nc -lu 127.0.0.1 $Base > udp.out 2>/dev/null & S=$!; sleep 0.5; echo dgram | nc -u -w1 127.0.0.1 $Base; filled udp.out 6; R=$?; kill $S 2>/dev/null; wait $S 2>/dev/null; [ $R = 0 ] && [ "$(cat udp.out)" = dgram ]'
seq 1 700000 | tr 0-9 7290518346 | head -c 4194304 > payload.bin
check nc_tcp_4mib 'timeout 60 nc -l 127.0.0.1 $((Base+1)) > tcp.out & S=$!; tcp_client 127.0.0.1 $((Base+1)) < payload.bin; wait $S; [ "$(wc -c < tcp.out)" = 4194304 ] && [ "$(sha256sum < payload.bin)" = "$(sha256sum < tcp.out)" ]'

# 4. Name resolution through nsswitch and /etc/hosts.
# `getent hosts` prefers ::1 when glibc synthesizes it; ahostsv4 pins IPv4.
check getent_hosts_localhost 'getent ahostsv4 localhost | grep -q "^127.0.0.1 "'

# 5. Resolver: needs a nameserver and a network.
if grep -qs '^nameserver' /etc/resolv.conf; then
  check getent_ahostsv4_debian 'getent ahostsv4 deb.debian.org | grep -q "[0-9]\.[0-9]"'
else
  skip getent_ahostsv4_debian "no nameserver in /etc/resolv.conf"
fi

# 6. Bash coproc and a named pipe.
check coproc_roundtrip 'coproc CO { while read -r L; do echo "echo:$L"; done; }; echo hello >&"${CO[1]}"; read -r -t 10 R <&"${CO[0]}"; exec {CO[1]}>&-; wait $CO_PID; [ "$R" = echo:hello ]'
check fifo_roundtrip 'mkfifo f.fifo; ( echo through-fifo > f.fifo ) & read -r -t 10 R < f.fifo; wait; [ "$R" = through-fifo ]'
exit $(( Fail ? 1 : 42 ))
