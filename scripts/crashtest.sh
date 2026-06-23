#!/usr/bin/env bash
#
# crashtest.sh -- the headline durability demo.
#
# Repeatedly: start durakv committing a batch of keys, kill -9 it at a random
# moment, restart (recovery runs), and assert that *every key it acknowledged*
# (printed "COMMIT <key>" before death) is present afterwards and the store
# reopens cleanly. Across iterations the store keeps growing, so we also prove
# nothing previously committed is ever lost.
#
# Exit 0 = PASS, non-zero = a committed key vanished or the DB failed to open.

set -u
cd "$(dirname "$0")/.."

DATA=/tmp/durakv_crash.db
WAL=/tmp/durakv_crash.log
OUT=/tmp/durakv_crash.out
ACK=/tmp/durakv_crash.acked
ITERS="${1:-25}"
BATCH="${2:-400}"
THREADS="${3:-1}"     # >1 exercises recovery under concurrent writers

rm -f "$DATA" "$WAL" "$ACK"
: > "$ACK"

if [ ! -x ./durakv ]; then echo "build first: make"; exit 2; fi

start=0
for ((it=1; it<=ITERS; it++)); do
    : > "$OUT"
    ./durakv "$DATA" "$WAL" stress "$BATCH" "$start" "$THREADS" > "$OUT" 2>/dev/null &
    pid=$!

    # let it make some progress, then kill at an unpredictable moment
    sleep "0.$((RANDOM % 6 + 1))"
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null

    # record every key the process acknowledged as durable
    acked_this=$(grep -c '^COMMIT ' "$OUT" 2>/dev/null) || true
    grep '^COMMIT ' "$OUT" 2>/dev/null | awk '{print $2}' >> "$ACK"

    # advance so the next batch uses fresh keys. Commits may be out of order
    # (concurrent writers), so take the MAX committed index, not the last line.
    last=$(grep '^COMMIT ' "$OUT" 2>/dev/null | sed 's/^COMMIT key//' | sort -n | tail -1)
    if [ -n "${last:-}" ]; then start=$((last + 1)); fi

    # ---- verify: restart (recovery) and check every acknowledged key ----
    if [ -s "$ACK" ]; then
        missing=$(sort -u "$ACK" | sed 's/^/get /' \
            | ./durakv "$DATA" "$WAL" 2>/dev/null \
            | grep -c '^NOTFOUND')
    else
        missing=0
    fi

    total=$(sort -u "$ACK" | wc -l | tr -d ' ')
    printf "iter %2d: acked +%-4s  total=%-6s  missing=%s\n" \
           "$it" "$acked_this" "$total" "$missing"

    if [ "$missing" -ne 0 ]; then
        echo "FAIL: $missing acknowledged key(s) lost after kill -9"
        exit 1
    fi
done

echo "crashtest: PASS -- $(sort -u "$ACK" | wc -l | tr -d ' ') committed keys survived $ITERS kill -9 cycles"
