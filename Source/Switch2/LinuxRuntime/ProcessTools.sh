#!/bin/bash
# SPDX-License-Identifier: MIT
# Debian tool acceptance for milestone 2 (processes, signals, timers, inotify).
# Usage: process-tools.sh [scratch-dir]. Exits 42 when every check passes.
set -u
export LC_ALL=C
Fail=0
check() { if eval "$2"; then echo "ok $1"; else echo "not ok $1"; Fail=1; fi; }
Dir=${1:-./process-tools.scratch}
rm -rf "$Dir"; mkdir -p "$Dir"; cd "$Dir" || exit 1

check pipeline_three_stages '[ "$(seq 1 1000 | grep 7 | wc -l)" = 271 ]'
check pipeline_status '! (false | true; exit ${PIPESTATUS[0]})'
check subshell_and_command_subst '[ "$( (echo a; echo b) | tr -d "\n")" = ab ]'
check sleep_duration 'S=$(date +%s%N); sleep 0.3; E=$(date +%s%N); [ $(( (E-S)/1000000 )) -ge 290 ]'
check timeout_kills_sleep 'timeout 0.2 sleep 5; [ $? = 124 ]'
check timeout_signal_kill 'timeout -s KILL 0.2 sleep 5; [ $? = 137 ]'
check timeout_busy_loop 'timeout 0.3 bash -c "while :; do :; done"; [ $? = 124 ]'
check kill_builtin_term 'sleep 5 & P=$!; kill $P; wait $P; [ $? = 143 ]'
check kill_binary_list '/bin/kill -l | grep -q TERM'
check kill_binary_signal 'sleep 5 & P=$!; /bin/kill -USR1 $P; wait $P; [ $? = 138 ]'
check trap_exit '[ "$(bash -c "trap \"echo bye\" EXIT; true")" = bye ]'
check trap_signal '[ "$(bash -c "trap \"echo got\" USR1; kill -USR1 \$\$; echo after")" = "got
after" ]'
check trap_err_and_return '[ "$(bash -c "trap \"echo e\" ERR; false; true")" = e ]'
check wait_n_any 'sleep 0.2 & sleep 0.1 & wait -n; [ $? = 0 ]; wait'
check background_job_completes '( sleep 0.1; echo done > bg.out ) & wait; [ "$(cat bg.out)" = done ]'
# Job control needs a terminal, which the acceptance run does not have; the
# stop and continue semantics are checked through a stopped sleep instead.
check stop_continue_job 'sleep 0.3 & P=$!; kill -STOP $P; sleep 0.6; kill -0 $P && kill -CONT $P && wait $P'
# inotifywait prints "Watches established." on stderr once it watches;
# acting only after that keeps the checks independent of startup time.
established() { for _ in $(seq 100); do grep -q established "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
check inotifywait_create 'mkdir -p watched; inotifywait -e create --format %f watched > create.out 2> create.err & W=$!; established create.err && touch watched/new; wait $W && [ "$(cat create.out)" = new ]'
check inotifywait_timeout 'mkdir -p idle; inotifywait -q -t 1 idle; [ $? = 2 ]'
check inotifywait_monitor 'mkdir -p m; inotifywait -m -e create,delete --format %e m > monitor.out 2> monitor.err & W=$!; established monitor.err && echo x > m/f && rm m/f; for _ in $(seq 100); do [ "$(wc -l < monitor.out)" -ge 2 ] && break; sleep 0.1; done; kill $W; wait $W; [ "$(head -2 monitor.out | tr "\n" " ")" = "CREATE DELETE " ]'
exit $(( Fail ? 1 : 42 ))
