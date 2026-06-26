/*
 * storage.c -- Page file, slotted pages, in-memory key directory, and the
 * transactional key/value API. See include/storage.h for the contract.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"
#include "wal.h"
#include "recovery.h"
#include "bufferpool.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ====================================================================== */
/* Slotted-page primitives                                                */
/* ====================================================================== */

void page_init(uint8_t *page, uint64_t page_id)
{
    memset(page, 0, PAGE_SIZE);
    PageHeader *h = (PageHeader *)page;
    h->page_id  = page_id;
    h->page_lsn = 0;
    h->n_slots  = 0;
    h->free_end = PAGE_SIZE;     /* record area is empty; grows downward */
}

uint64_t page_get_lsn(const uint8_t *page) { return ((const PageHeader *)page)->page_lsn; }
void     page_set_lsn(uint8_t *page, uint64_t lsn) { ((PageHeader *)page)->page_lsn = lsn; }

uint16_t page_free_space(const uint8_t *page)
{
    const PageHeader *h = (const PageHeader *)page;
    size_t dir_end = sizeof(PageHeader) + (size_t)h->n_slots * sizeof(Slot);
    if (h->free_end <= dir_end) return 0;
    return (uint16_t)(h->free_end - dir_end);
}

int page_insert(uint8_t *page, const uint8_t *rec, uint16_t reclen, uint16_t *slot_out)
{
    PageHeader *h = (PageHeader *)page;
    /* one more slot must fit alongside the new record */
    size_t dir_end = sizeof(PageHeader) + ((size_t)h->n_slots + 1) * sizeof(Slot);
    if (h->free_end < reclen) return -1;
    uint16_t new_off = (uint16_t)(h->free_end - reclen);
    if (new_off < dir_end) return -1;       /* would collide with slot dir */

    memcpy(page + new_off, rec, reclen);
    Slot *slots = (Slot *)(page + sizeof(PageHeader));
    slots[h->n_slots].off = new_off;
    slots[h->n_slots].len = reclen;
    *slot_out = h->n_slots;
    h->n_slots++;
    h->free_end = new_off;
    return 0;
}

void page_tombstone(uint8_t *page, uint16_t slot)
{
    PageHeader *h = (PageHeader *)page;
    if (slot >= h->n_slots) return;
    Slot *slots = (Slot *)(page + sizeof(PageHeader));
    slots[slot].len = 0;        /* space is reclaimed only on compaction */
}

const uint8_t *page_slot_ptr(const uint8_t *page, uint16_t slot, uint16_t *len_out)
{
    const PageHeader *h = (const PageHeader *)page;
    if (slot >= h->n_slots) return NULL;
    const Slot *slots = (const Slot *)(page + sizeof(PageHeader));
    if (slots[slot].len == 0) return NULL;
    if (len_out) *len_out = slots[slot].len;
    return page + slots[slot].off;
}

/* ====================================================================== */
/* Record framing                                                         */
/* ====================================================================== */

uint16_t record_build(uint8_t *buf, const char *key, uint16_t klen,
                      const void *val, uint32_t vlen)
{
    uint8_t *p = buf;
    memcpy(p, &klen, 2);              p += 2;
    memcpy(p, key, klen);            p += klen;
    memcpy(p, &vlen, 4);             p += 4;
    memcpy(p, val, vlen);           p += vlen;
    return (uint16_t)(p - buf);
}

void record_parse(const uint8_t *rec, const char **key, uint16_t *klen,
                  const uint8_t **val, uint32_t *vlen)
{
    uint16_t kl; uint32_t vl;
    const uint8_t *p = rec;
    memcpy(&kl, p, 2);   p += 2;
    *key  = (const char *)p;  *klen = kl;   p += kl;
    memcpy(&vl, p, 4);   p += 4;
    *val  = p;  *vlen = vl;
}

/* ====================================================================== */
/* Raw page I/O                                                           */
/* ====================================================================== */

int page_read(DB *db, uint64_t page_id, uint8_t *buf)
{
    ssize_t n = pread(db->data_fd, buf, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    if (n < (ssize_t)PAGE_SIZE) {
        /* hole or past EOF -> hand back a valid empty page */
        page_init(buf, page_id);
    }
    return DK_OK;
}

int page_write(DB *db, uint64_t page_id, const uint8_t *buf)
{
    ssize_t n = pwrite(db->data_fd, buf, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    if (n != (ssize_t)PAGE_SIZE) return DK_IO;
    if (page_id + 1 > db->page_count) db->page_count = page_id + 1;
    return DK_OK;
}

/* Buffer-pool I/O callbacks: every cached read/write flows through page_read /
 * page_write, the single place that (later) applies encryption at rest. */
static int bp_io_read(void *ctx, uint64_t page_id, uint8_t *out)
{
    return page_read((DB *)ctx, page_id, out);
}
static int bp_io_write(void *ctx, uint64_t page_id, const uint8_t *in)
{
    return page_write((DB *)ctx, page_id, in);
}

/* ====================================================================== */
/* Key directory (chained hash map)                                       */
/* ====================================================================== */

static size_t hash_key(const char *s)
{
    size_t h = 5381;
    for (; *s; s++) h = ((h << 5) + h) ^ (unsigned char)*s;   /* djb2 */
    return h;
}

static void dir_init(Dir *d)
{
    d->nbuckets = 1024;
    d->count    = 0;
    d->buckets  = calloc(d->nbuckets, sizeof(DirEntry *));
}

static void dir_free(Dir *d)
{
    if (!d->buckets) return;
    for (size_t i = 0; i < d->nbuckets; i++) {
        DirEntry *e = d->buckets[i];
        while (e) { DirEntry *nx = e->next; free(e->key); free(e); e = nx; }
    }
    free(d->buckets);
    d->buckets = NULL;
}

static DirEntry *dir_get(Dir *d, const char *key)
{
    size_t b = hash_key(key) % d->nbuckets;
    for (DirEntry *e = d->buckets[b]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

static void dir_put(Dir *d, const char *key, uint64_t page_id, uint16_t slot)
{
    DirEntry *e = dir_get(d, key);
    if (e) { e->page_id = page_id; e->slot = slot; return; }
    size_t b = hash_key(key) % d->nbuckets;
    e = malloc(sizeof(*e));
    e->key = strdup(key);
    e->page_id = page_id;
    e->slot = slot;
    e->next = d->buckets[b];
    d->buckets[b] = e;
    d->count++;
}

static void dir_del(Dir *d, const char *key)
{
    size_t b = hash_key(key) % d->nbuckets;
    DirEntry **pp = &d->buckets[b];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            DirEntry *dead = *pp;
            *pp = dead->next;
            free(dead->key); free(dead);
            d->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ====================================================================== */
/* Free-space tracking                                                    */
/* ====================================================================== */

static void page_free_ensure(DB *db, uint64_t page_id)
{
    if ((size_t)page_id < db->page_free_cap) return;
    size_t ncap = db->page_free_cap ? db->page_free_cap : 16;
    while (ncap <= (size_t)page_id) ncap *= 2;
    db->page_free = realloc(db->page_free, ncap * sizeof(uint16_t));
    for (size_t i = db->page_free_cap; i < ncap; i++) db->page_free[i] = 0;
    db->page_free_cap = ncap;
}

/* Find a data page (id >= 1) with at least `need` bytes free, allocating a
 * fresh page at the end of the file if none qualifies. */
static uint64_t find_page(DB *db, uint16_t need)
{
    for (uint64_t pid = 1; pid < db->page_count; pid++)
        if (db->page_free[pid] >= need) return pid;

    /* allocate a new page at the end */
    uint64_t pid = db->page_count;
    page_free_ensure(db, pid);
    uint8_t empty[PAGE_SIZE];
    page_init(empty, pid);
    db->page_free[pid] = page_free_space(empty);
    db->page_count = pid + 1;        /* materialised on disk by page_write */
    return pid;
}

/* ====================================================================== */
/* Cached page access (live path goes through the buffer pool)            */
/* ====================================================================== */

/* Read a page's current contents through the buffer pool. Recovery and the
 * post-open index rebuild run before db->bp exists and use page_read directly. */
static void cached_read(DB *db, uint64_t page_id, uint8_t *dst)
{
    uint8_t *frame = bp_pin(db->bp, page_id);
    memcpy(dst, frame, PAGE_SIZE);
    bp_unpin(db->bp, page_id, 0);
}

/* Install a page's new contents into the pool and mark the frame dirty. The
 * page is flushed to data.db lazily (on eviction / checkpoint / close). */
static void cached_write(DB *db, uint64_t page_id, const uint8_t *src)
{
    uint8_t *frame = bp_pin(db->bp, page_id);
    memcpy(frame, src, PAGE_SIZE);
    bp_unpin(db->bp, page_id, 1);
}

/* ====================================================================== */
/* Transactional mutations                                                */
/* ====================================================================== */

/* A page touched by the current transaction. */
typedef struct {
    uint64_t page_id;
    uint8_t  before[PAGE_SIZE];
    uint8_t  after[PAGE_SIZE];
} Mod;

/* Log [BEGIN, UPDATE..., COMMIT], fsync the WAL, then write the data pages.
 * Returns DK_OK. This is the durability heartbeat of the whole system. */
static int commit_mods(DB *db, Mod *mods, int nmods)
{
    uint64_t txn = db->next_txn++;
    uint64_t prev = wal_append(db, WAL_BEGIN, txn, 0, 0, NULL, 0, NULL, 0);

    for (int i = 0; i < nmods; i++) {
        /* stamp the after-image with the LSN this UPDATE will receive, so
         * recovery's page_lsn comparison is exact and idempotent. */
        page_set_lsn(mods[i].after, db->next_lsn);
        prev = wal_append(db, WAL_UPDATE, txn, prev, mods[i].page_id,
                          mods[i].before, PAGE_SIZE,
                          mods[i].after,  PAGE_SIZE);
    }
    wal_append(db, WAL_COMMIT, txn, prev, 0, NULL, 0, NULL, 0);

    wal_fsync(db);               /* <-- the commit is durable here */

    /* Now (and only now, post-fsync) install the after-images into the buffer
     * pool. They become dirty frames flushed lazily; data.db is fsync'd only
     * at checkpoint/close. A crash that loses these unsynced pages is repaired
     * by redo, because the WAL holding their after-images is already durable. */
    for (int i = 0; i < nmods; i++) {
        cached_write(db, mods[i].page_id, mods[i].after);
        page_free_ensure(db, mods[i].page_id);
        db->page_free[mods[i].page_id] = page_free_space(mods[i].after);
    }
    return DK_OK;
}

static int db_set_unlocked(DB *db, const char *key, const void *val, uint32_t vlen)
{
    size_t klen = strlen(key);
    if (klen == 0 || klen > 0xFFFF) return DK_INVAL;

    uint8_t rec[PAGE_SIZE];
    uint16_t reclen = record_build(rec, key, (uint16_t)klen, val, vlen);
    if (reclen > MAX_RECORD) return DK_TOOBIG;

    /* a record also consumes one slot-directory entry */
    uint16_t need = (uint16_t)(reclen + sizeof(Slot));

    DirEntry *old = dir_get(&db->dir, key);

    Mod mods[2];
    int nmods = 0;
    uint64_t new_page; uint16_t new_slot = 0;

    if (old && db->page_free[old->page_id] >= need) {
        /* update in place: tombstone + insert on the same page */
        Mod *m = &mods[nmods++];
        m->page_id = old->page_id;
        cached_read(db, m->page_id, m->before);
        memcpy(m->after, m->before, PAGE_SIZE);
        page_tombstone(m->after, old->slot);
        if (page_insert(m->after, rec, reclen, &new_slot) != 0) return DK_IO;
        new_page = m->page_id;
    } else {
        if (old) {
            /* tombstone the old record on its page */
            Mod *m = &mods[nmods++];
            m->page_id = old->page_id;
            cached_read(db, m->page_id, m->before);
            memcpy(m->after, m->before, PAGE_SIZE);
            page_tombstone(m->after, old->slot);
        }
        new_page = find_page(db, need);
        Mod *m = &mods[nmods++];
        m->page_id = new_page;
        cached_read(db, m->page_id, m->before);
        memcpy(m->after, m->before, PAGE_SIZE);
        if (page_insert(m->after, rec, reclen, &new_slot) != 0) return DK_IO;
    }

    int rc = commit_mods(db, mods, nmods);
    if (rc != DK_OK) return rc;

    dir_put(&db->dir, key, new_page, new_slot);
    return DK_OK;
}

static int db_get_unlocked(DB *db, const char *key, void *valbuf, uint32_t buflen,
                           uint32_t *vlen_out)
{
    DirEntry *e = dir_get(&db->dir, key);
    if (!e) return DK_NOTFOUND;

    uint8_t page[PAGE_SIZE];
    cached_read(db, e->page_id, page);
    uint16_t rlen;
    const uint8_t *rec = page_slot_ptr(page, e->slot, &rlen);
    if (!rec) return DK_NOTFOUND;

    const char *k; uint16_t kl; const uint8_t *v; uint32_t vl;
    record_parse(rec, &k, &kl, &v, &vl);
    if (vlen_out) *vlen_out = vl;
    uint32_t n = vl < buflen ? vl : buflen;
    if (valbuf && n) memcpy(valbuf, v, n);
    return DK_OK;
}

static int db_del_unlocked(DB *db, const char *key)
{
    DirEntry *old = dir_get(&db->dir, key);
    if (!old) return DK_NOTFOUND;

    Mod m;
    m.page_id = old->page_id;
    cached_read(db, m.page_id, m.before);
    memcpy(m.after, m.before, PAGE_SIZE);
    page_tombstone(m.after, old->slot);

    int rc = commit_mods(db, &m, 1);
    if (rc != DK_OK) return rc;

    dir_del(&db->dir, key);
    return DK_OK;
}

/* ---- public, thread-safe wrappers -------------------------------------- *
 * Writers take the rwlock exclusively; readers share it. The buffer pool has
 * its own internal mutex, so concurrent readers may fault pages safely. */

int db_set(DB *db, const char *key, const void *val, uint32_t vlen)
{
    pthread_rwlock_wrlock(&db->lock);
    int rc = db_set_unlocked(db, key, val, vlen);
    pthread_rwlock_unlock(&db->lock);
    return rc;
}

int db_get(DB *db, const char *key, void *valbuf, uint32_t buflen, uint32_t *vlen_out)
{
    pthread_rwlock_rdlock(&db->lock);
    int rc = db_get_unlocked(db, key, valbuf, buflen, vlen_out);
    pthread_rwlock_unlock(&db->lock);
    return rc;
}

int db_del(DB *db, const char *key)
{
    pthread_rwlock_wrlock(&db->lock);
    int rc = db_del_unlocked(db, key);
    pthread_rwlock_unlock(&db->lock);
    return rc;
}

void db_checkpoint(DB *db)
{
    pthread_rwlock_wrlock(&db->lock);
    bp_flush_all(db->bp);        /* push dirty frames to data.db */
    fsync(db->data_fd);          /* all data pages now durable   */
    wal_append(db, WAL_CHECKPOINT, 0, 0, db->page_count, NULL, 0, NULL, 0);
    wal_fsync(db);
    pthread_rwlock_unlock(&db->lock);
}

/* ====================================================================== */
/* Open / rebuild / close                                                 */
/* ====================================================================== */

/* Scan all data pages and rebuild the in-memory directory + free map. */
static void rebuild_index(DB *db)
{
    dir_init(&db->dir);
    page_free_ensure(db, db->page_count ? db->page_count - 1 : 0);

    uint8_t page[PAGE_SIZE];
    for (uint64_t pid = 1; pid < db->page_count; pid++) {
        page_read(db, pid, page);
        const PageHeader *h = (const PageHeader *)page;
        for (uint16_t s = 0; s < h->n_slots; s++) {
            uint16_t rlen;
            const uint8_t *rec = page_slot_ptr(page, s, &rlen);
            if (!rec) continue;
            const char *k; uint16_t kl; const uint8_t *v; uint32_t vl;
            record_parse(rec, &k, &kl, &v, &vl);
            char *keyz = malloc(kl + 1);
            memcpy(keyz, k, kl); keyz[kl] = '\0';
            dir_put(&db->dir, keyz, pid, s);
            free(keyz);
        }
        db->page_free[pid] = page_free_space(page);
    }
}

DB *db_open(const char *data_path, const char *wal_path)
{
    return db_open_ex(data_path, wal_path, 64, POLICY_LRU);
}

DB *db_open_ex(const char *data_path, const char *wal_path,
               size_t nframes, PolicyKind policy)
{
    DB *db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    db->data_fd = open(data_path, O_RDWR | O_CREAT, 0600);
    if (db->data_fd < 0) { perror("open data"); free(db); return NULL; }

    off_t sz = lseek(db->data_fd, 0, SEEK_END);
    if (sz == 0) {
        /* fresh store: write the immutable header page 0 */
        uint8_t p0[PAGE_SIZE];
        memset(p0, 0, PAGE_SIZE);
        memcpy(p0, DURAKV_MAGIC, 8);
        uint32_t ver = DURAKV_VERSION, ps = PAGE_SIZE;
        memcpy(p0 + 8, &ver, 4);
        memcpy(p0 + 12, &ps, 4);
        if (pwrite(db->data_fd, p0, PAGE_SIZE, 0) != PAGE_SIZE) {
            perror("init header"); close(db->data_fd); free(db); return NULL;
        }
        fsync(db->data_fd);
        db->page_count = 1;
    } else {
        db->page_count = (uint64_t)(sz / PAGE_SIZE);
        if (db->page_count == 0) db->page_count = 1;
    }

    db->wal_fd = open(wal_path, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (db->wal_fd < 0) { perror("open wal"); close(db->data_fd); free(db); return NULL; }

    db->next_lsn = 1;
    db->next_txn = 1;
    pthread_rwlock_init(&db->lock, NULL);

    recovery_run(db);            /* analysis / redo / undo (direct page I/O) */
    rebuild_index(db);           /* directory reflects the recovered pages   */

    /* the buffer pool is created last: recovery and rebuild use direct page
     * I/O, so the pool starts cold and every later access flows through it.
     * Route its I/O through page_read/page_write (the encryption choke point). */
    db->bp = bp_create(db->data_fd, nframes, policy);
    bp_set_io(db->bp, db, bp_io_read, bp_io_write);

    return db;
}

void db_close(DB *db)
{
    if (!db) return;
    bp_flush_all(db->bp);        /* dirty frames -> data.db */
    fsync(db->data_fd);
    fsync(db->wal_fd);
    bp_destroy(db->bp);
    pthread_rwlock_destroy(&db->lock);
    dir_free(&db->dir);
    free(db->page_free);
    close(db->data_fd);
    close(db->wal_fd);
    free(db);
}
