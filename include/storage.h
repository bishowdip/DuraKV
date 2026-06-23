/*
 * storage.h -- Page file + slotted-page key/value storage (DuraKV Phase 1).
 *
 * OS/systems primitive: file I/O, on-disk data structures (slotted pages).
 *
 * The page file (data.db) is an array of fixed-size pages:
 *   - page 0 is the immutable header page (magic / version / page_size).
 *   - pages 1..N-1 are slotted data pages holding {key,value} records.
 *
 * Phase 1 stores pages in plaintext (encryption-at-rest arrives in Phase 5).
 * Durability is provided by the write-ahead log (see wal.h) and crash
 * recovery (see recovery.h): a SET is only acknowledged after its COMMIT
 * record is fsync()'d to the WAL.
 */
#ifndef DURAKV_STORAGE_H
#define DURAKV_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "replacement.h"        /* PolicyKind */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096          /* logical page size (configurable at compile time) */
#endif

#define DURAKV_MAGIC "DURAKVDB"   /* 8 bytes, page-0 header */
#define DURAKV_VERSION 1u

/* ---- on-disk page layout ----------------------------------------------- */

/* Slotted-page header, stored at the very start of every data page. */
typedef struct __attribute__((packed)) {
    uint64_t page_id;   /* this page's id (sanity/debugging)                */
    uint64_t page_lsn;  /* LSN of the last WAL record that modified it      */
    uint16_t n_slots;   /* number of slot-directory entries                 */
    uint16_t free_end;  /* offset where the record area begins (grows down) */
} PageHeader;

/* One slot-directory entry. len == 0 means the slot is empty/tombstoned. */
typedef struct __attribute__((packed)) {
    uint16_t off;       /* byte offset of the record within the page        */
    uint16_t len;       /* record length in bytes (0 == tombstone)          */
} Slot;

/* Largest record that fits in an empty page (one header + one slot). */
#define MAX_RECORD (PAGE_SIZE - (int)sizeof(PageHeader) - (int)sizeof(Slot))

/* ---- in-memory key directory (hash map: key -> (page_id, slot)) -------- */

typedef struct DirEntry {
    char            *key;
    uint64_t         page_id;
    uint16_t         slot;
    struct DirEntry *next;
} DirEntry;

typedef struct {
    DirEntry **buckets;
    size_t     nbuckets;
    size_t     count;
} Dir;

/* ---- the database handle ----------------------------------------------- */

struct BufferPool;              /* defined in bufferpool.h                   */

typedef struct DB {
    int       data_fd;          /* data.db                                   */
    int       wal_fd;           /* wal.log (O_APPEND for writes)             */
    uint64_t  page_count;       /* number of pages in the data file          */
    uint64_t  next_lsn;         /* next log sequence number to hand out      */
    uint64_t  next_txn;         /* next transaction id                       */
    Dir       dir;              /* key -> location index                     */
    uint16_t *page_free;        /* free bytes per page (index by page id)    */
    size_t    page_free_cap;    /* capacity of page_free[]                   */
    struct BufferPool *bp;      /* page cache (NULL during recovery)         */
    pthread_rwlock_t lock;      /* many concurrent readers, exclusive writers*/
} DB;

/* ---- return codes ------------------------------------------------------ */
enum {
    DK_OK        =  0,
    DK_NOTFOUND  = -1,
    DK_TOOBIG    = -2,
    DK_IO        = -3,
    DK_INVAL     = -4
};

/* ---- public key/value API ---------------------------------------------- */

/* Open (or create) the store, run crash recovery, and return a handle.
 * db_open uses sensible defaults; db_open_ex configures the buffer pool. */
DB  *db_open(const char *data_path, const char *wal_path);
DB  *db_open_ex(const char *data_path, const char *wal_path,
                size_t nframes, PolicyKind policy);
void db_close(DB *db);

int  db_set(DB *db, const char *key, const void *val, uint32_t vlen);
/* Copies up to buflen bytes of the value into valbuf; *vlen_out gets the
 * true length. Returns DK_OK / DK_NOTFOUND. */
int  db_get(DB *db, const char *key, void *valbuf, uint32_t buflen,
            uint32_t *vlen_out);
int  db_del(DB *db, const char *key);

void db_checkpoint(DB *db);     /* fsync data + write CHECKPOINT marker */

/* ---- page-level helpers (used by storage.c, wal.c, recovery.c) --------- */

void        page_init(uint8_t *page, uint64_t page_id);
int         page_insert(uint8_t *page, const uint8_t *rec, uint16_t reclen,
                        uint16_t *slot_out);   /* 0 ok, -1 no space */
void        page_tombstone(uint8_t *page, uint16_t slot);
const uint8_t *page_slot_ptr(const uint8_t *page, uint16_t slot,
                             uint16_t *len_out);  /* NULL if empty/oob */
uint16_t    page_free_space(const uint8_t *page);
uint64_t    page_get_lsn(const uint8_t *page);
void        page_set_lsn(uint8_t *page, uint64_t lsn);

/* Raw page I/O against data.db. page_read returns a valid empty page for
 * ids past EOF; page_write extends the file as needed. */
int  page_read(DB *db, uint64_t page_id, uint8_t *buf);
int  page_write(DB *db, uint64_t page_id, const uint8_t *buf);

/* record framing: [key_len u16][key][val_len u32][value] */
uint16_t record_build(uint8_t *buf, const char *key, uint16_t klen,
                      const void *val, uint32_t vlen);
void     record_parse(const uint8_t *rec, const char **key, uint16_t *klen,
                      const uint8_t **val, uint32_t *vlen);

#endif /* DURAKV_STORAGE_H */
