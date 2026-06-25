# DuraKV -- Phase 1 (durable spine). No external dependencies yet;
# threads/network/crypto and their libs arrive in later phases.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude -pthread
LDFLAGS ?= -pthread

CORE    := src/storage.c src/wal.c src/recovery.c \
           src/bufferpool.c src/replacement.c \
           src/threadpool.c src/scheduler.c
NET     := src/protocol.c

# libsodium (security layer only) -- located via Homebrew, fallback /usr/local
SODIUM_PREFIX ?= $(shell brew --prefix libsodium 2>/dev/null || echo /usr/local)
SODIUM_CFLAGS := -I$(SODIUM_PREFIX)/include
SODIUM_LIBS   := -L$(SODIUM_PREFIX)/lib -lsodium

.PHONY: all tests test demos crashtest crashtest_concurrent clean

all: durakv durakv-server durakv-client

durakv: $(CORE) src/durakv.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- network/IPC binaries (AF_UNIX) --------------------------------------
durakv-server: $(CORE) $(NET) src/server.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

durakv-client: $(NET) src/client.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- unit tests ----------------------------------------------------------
tests: test_storage test_wal_recovery test_bufferpool test_belady mem_demo \
       demo_race demo_deadlock demo_scheduler loadtest test_ipc demo_mqueue \
       file_demo demo_crypto demo_audit

test_storage: $(CORE) tests/test_storage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_wal_recovery: $(CORE) tests/test_wal_recovery.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_bufferpool: $(CORE) tests/test_bufferpool.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_belady: $(CORE) tests/test_belady.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- network/IPC tests ---------------------------------------------------
test_ipc: $(CORE) $(NET) src/server.c tests/test_ipc.c
	$(CC) $(CFLAGS) -DDURAKV_SERVER_NO_MAIN -o $@ $^ $(LDFLAGS)

demo_mqueue: tests/demo_mqueue.c
	$(CC) $(CFLAGS) -o $@ $^

# --- standalone concept demos --------------------------------------------
mem_demo: tests/mem_demo.c
	$(CC) $(CFLAGS) -o $@ $^

file_demo: tests/file_demo.c
	$(CC) $(CFLAGS) -o $@ $^

# --- security demos (need libsodium) -------------------------------------
demo_crypto: src/crypto.c tests/demo_crypto.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(SODIUM_LIBS)

demo_audit: src/audit.c tests/demo_audit.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(SODIUM_LIBS)

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
	@echo "== demo_mqueue =="       && ./demo_mqueue
	@echo "== test_ipc =="          && ./test_ipc
	@echo "== file_demo =="         && ./file_demo
	@echo "== demo_crypto =="       && ./demo_crypto
	@echo "== demo_audit =="        && ./demo_audit

crashtest: durakv
	./scripts/crashtest.sh

crashtest_concurrent: durakv
	./scripts/crashtest.sh 12 400 4    # 4 worker threads per batch

clean:
	rm -f durakv durakv-server durakv-client
	rm -f test_storage test_wal_recovery test_bufferpool test_belady
	rm -f mem_demo demo_race demo_deadlock demo_scheduler loadtest test_ipc demo_mqueue
	rm -f file_demo demo_crypto demo_audit
	rm -f *.db *.log *.sock /tmp/durakv_*.db /tmp/durakv_*.log /tmp/durakv_*.snap /tmp/durakv_*.sock
	rm -rf *.dSYM
