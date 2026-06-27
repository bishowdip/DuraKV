#!/usr/bin/env bash
#
# run_demo.sh -- a guided showcase of DuraKV, grouped by coursework task.
# Builds everything, then runs each demonstration with a clear heading so the
# whole system can be shown end to end with a single command.
#
#   ./scripts/run_demo.sh
#
cd "$(dirname "$0")/.."

bold=$'\033[1m'; cyan=$'\033[1;36m'; green=$'\033[1;32m'; dim=$'\033[2m'; off=$'\033[0m'

section() { printf '\n%s======================================================%s\n' "$cyan" "$off"
            printf '%s  %s%s\n' "$cyan$bold" "$1" "$off"
            printf '%s======================================================%s\n' "$cyan" "$off"; }

printf '%sBuilding DuraKV and its demos...%s\n' "$dim" "$off"
if ! make tests >/tmp/durakv_build.log 2>&1; then
    echo "build failed -- see /tmp/durakv_build.log"; exit 1
fi
printf '%sdone.%s\n' "$dim" "$off"

section "TASK 1  Concurrency: a race condition and how to fix it"
./demo_race
section "TASK 1  Concurrency: deadlock vs. strict lock ordering"
./demo_deadlock
section "TASK 1  Concurrency: round-robin fairness (anti-starvation)"
./demo_scheduler

section "TASK 2  Memory: paging + FIFO replacement (pointer level)"
./mem_demo
section "TASK 2  Memory: Belady's anomaly (FIFO) vs. monotonic LRU"
./test_belady
section "TASK 2  Memory: buffer-pool hit ratios"
./test_bufferpool

section "TASK 3  Security: POSIX file permissions"
./file_demo
section "TASK 3  Security: encryption at rest (data + WAL)"
./demo_encrypt
section "TASK 3  Security: Argon2 login + rwx permissions"
./demo_auth
section "TASK 3  Security: tamper-evident audit log"
./demo_audit

section "TASK 4  IPC: message queue (message sharing)"
./demo_mqueue
section "TASK 4  IPC: concurrent clients over a Unix-domain socket"
./test_ipc
section "TASK 4  Security over IPC: auth gate + permissions + audit"
./test_secure

section "DURABILITY  the headline: data survives kill -9"
./scripts/crashtest.sh 5 200 1

printf '\n%sAll demonstrations complete.%s\n' "$green" "$off"
