#!/bin/bash
# SPDX-License-Identifier: MIT
# Debian tool acceptance for milestone 4 (namespaces, mounts, seccomp,
# scheduling). Usage: container-tools.sh [scratch-dir]. Exits 42 when every
# check passes.
set -u
export LC_ALL=C
Fail=0
check() { if eval "$2"; then echo "ok $1"; else echo "not ok $1"; Fail=1; fi; }
skip() { echo "ok $1 # SKIP $2"; }
Dir=${1:-./container-tools.scratch}
rm -rf "$Dir"; mkdir -p "$Dir"; cd "$Dir" || exit 1

# Common bubblewrap arguments: Steam's pressure-vessel shares the network, so
# the checks do too (a network namespace is future work, RemainingSyscalls.md).
Bwrap="bwrap --unshare-all --share-net --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp"

# 1. bubblewrap: user, mount, UTS, IPC, PID and cgroup namespaces, a tmpfs and a hostname.
check bwrap_hostname '[ "$($Bwrap --hostname box cat /proc/sys/kernel/hostname)" = box ]'
check bwrap_pid_namespace '[ "$($Bwrap sh -c "echo \$\$")" -le 2 ]'
check bwrap_tmpfs_writable '$Bwrap sh -c "echo data > /tmp/x && [ \"\$(cat /tmp/x)\" = data ]"'
check bwrap_ro_bind_readonly '! $Bwrap sh -c "echo data > /etc/container-tools-probe" 2>/dev/null'

# 2. bubblewrap with a seccomp filter (raw struct sock_filter, little endian):
#    x86-64 mkdir (83) and mkdirat (258) fail with EPERM, everything else runs.
#    ld arch; jeq x86-64 -> ld nr; (other arch: allow); jeq 83 / 258 -> errno; allow.
printf '\x20\x00\x00\x00\x04\x00\x00\x00\x15\x00\x01\x00\x3e\x00\x00\xc0\x06\x00\x00\x00\x00\x00\xff\x7f\x20\x00\x00\x00\x00\x00\x00\x00\x15\x00\x02\x00\x53\x00\x00\x00\x15\x00\x01\x00\x02\x01\x00\x00\x06\x00\x00\x00\x00\x00\xff\x7f\x06\x00\x00\x00\x01\x00\x05\x00' > mkdir.bpf
check bwrap_unfiltered_mkdir '$Bwrap mkdir /tmp/d'
check bwrap_seccomp_blocks_mkdir '! $Bwrap --seccomp 9 mkdir /tmp/d 9< mkdir.bpf 2>/dev/null'
check bwrap_seccomp_errno_is_eperm '$Bwrap --seccomp 9 mkdir /tmp/d 9< mkdir.bpf 2>&1 | grep -q "Operation not permitted"'
check bwrap_seccomp_allows_others '$Bwrap --seccomp 9 true 9< mkdir.bpf'

# 3. util-linux unshare: user namespace with root mapped, a tmpfs mount, PID namespace.
check unshare_user_root 'unshare -Ur sh -c "[ \"\$(id -u)\" = 0 ]"'
check unshare_mount_tmpfs 'mkdir -p mnt && unshare -Urm sh -c "mount -t tmpfs none mnt && touch mnt/x && [ -e mnt/x ]" && [ ! -e mnt/x ]'
check unshare_pid_proc 'N=$(unshare -Urpf --mount-proc sh -c "set -- /proc/[0-9]*; echo \$#"); [ -n "$N" ] && [ "$N" -le 2 ]'
check unshare_uts_hostname 'H=$(hostname); unshare -Uru sh -c "hostname boxy && [ \"\$(hostname)\" = boxy ]" && [ "$(hostname)" = "$H" ]'

# 4. nsenter into a running user namespace. The namespace denied setgroups,
#    so nsenter must keep the caller's credentials.
unshare -Ur sleep 30 & P=$!
for _ in $(seq 100); do [ "$(readlink /proc/$P/ns/user)" != "$(readlink /proc/self/ns/user)" ] && break; sleep 0.1; done
check nsenter_user_namespace 'U=$(nsenter -U --preserve-credentials -t $P sh -c "id -u"); [ "$U" = 0 ]'
check nsenter_same_namespace 'N=$(nsenter -U --preserve-credentials -t $P readlink /proc/self/ns/user); [ -n "$N" ] && [ "$N" = "$(readlink /proc/$P/ns/user)" ]'
kill $P 2>/dev/null; wait $P 2>/dev/null

# 5. Scheduling: chrt, taskset, ionice.
check chrt_batch_runs 'chrt -b 0 true'
check chrt_batch_policy 'chrt -b 0 sh -c "chrt -p \$\$" | grep -q "policy: SCHED_BATCH"'
check chrt_default_policy 'chrt -p $$ | grep -q "policy: SCHED_OTHER"'
check taskset_single_cpu 'taskset -c 0 sh -c "taskset -p \$\$" | grep -q "mask: 1\$"'
check ionice_best_effort 'ionice -c 2 -n 7 sh -c "ionice -p \$\$" | grep -q "best-effort: prio 7"'
exit $(( Fail ? 1 : 42 ))
