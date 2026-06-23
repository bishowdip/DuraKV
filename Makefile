# DuraKV -- Phase 1 (durable spine). No external dependencies yet;
# threads/network/crypto and their libs arrive in later phases.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude -pthread
LDFLAGS ?= -pthread

CORE    := src/storage.c src/wal.c src/recovery.c \
           src/bufferpool.c src/replacement.c \
           src/threadpool.c src/scheduler.c

.PHONY: all tests test demos crashtest crashtest_concurrent clean

all: durakv

durakv: $(CORE) src/durakv.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- unit tests ----------------------------------------------------------
tests: test_storage test_wal_recovery test_bufferpool test_belady mem_demo \
       demo_race demo_deadlock demo_scheduler loadtest

test_storage: $(CORE) tests/test_storage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_wal_recovery: $(CORE) tests/test_wal_recovery.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_bufferpool: $(CORE) tests/test_bufferpool.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_belady: $(CORE) tests/test_belady.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- standalone concept demos --------------------------------------------
mem_demo: tests/mem_demo.c
	$(CC) $(CFLAGS) -o $@ $^

# --- concurrency demos ---------------------------------------------------
demo_race: $(CORE) tests/demo_race.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

demo_deadlock: $(CORE) tests/demo_deadlock.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

demo_scheduler: $(CORE) tests/demo_scheduler.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

loadtest: $(CORE) tests/loadtest.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: tests
	@echo "== test_storage =="      && ./test_storage
	@echo "== test_wal_recovery ==" && ./test_wal_recovery
	@echo "== test_bufferpool =="   && ./test_bufferpool
	@echo "== test_belady =="       && ./test_belady
	@echo "== mem_demo =="          && ./mem_demo
	@echo "== demo_race =="         && ./demo_race
	@echo "== demo_deadlock =="     && ./demo_deadlock
	@echo "== demo_scheduler =="    && ./demo_scheduler
	@echo "== loadtest =="          && ./loadtest

crashtest: durakv
	./scripts/crashtest.sh

crashtest_concurrent: durakv
	./scripts/crashtest.sh 12 400 4    # 4 worker threads per batch

clean:
	rm -f durakv test_storage test_wal_recovery test_bufferpool test_belady
	rm -f mem_demo demo_race demo_deadlock demo_scheduler loadtest
	rm -f *.db *.log /tmp/durakv_*.db /tmp/durakv_*.log /tmp/durakv_*.snap
	rm -rf *.dSYM
