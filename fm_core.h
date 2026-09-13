/*
   FASTMEM storage engine - lock-free row-atomic concurrency core
   ================================================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   DESIGN OVERVIEW
   ---------------
   FASTMEM is a memory-only storage engine aimed at workloads that MEMORY
   handles badly because of its table-level lock (THR_LOCK): hot rows that
   are updated continuously while many connections read them.

   The engine has NO table lock and NO transactions.  The concurrency
   model, in one sentence: every row lives in a fixed-size slot; readers
   copy the row image with a seqlock (they never block and never wait on
   a writer), writers atomically publish a full new image of the row.

   Readers (rnd_pos / scans / index point lookups):
     - never take a spinlock;
     - load the slot sequence counter, copy the record image, load the
       counter again and retry if it changed (torn-read protection);
     - a slot in the middle of being updated shows an odd sequence, so a
       reader simply retries until it observes a stable even sequence.

   Writers:
     - a per-slot spinlock (wlock) serializes writers of the SAME row;
       different rows never contend;
     - an update is a memcpy of the new image into the slot under the
       seqlock protocol (odd seq -> copy -> even seq).  No allocation,
       no garbage, no memory reclamation on the hot path;
     - hash-index chains are guarded by per-bucket spinlocks; a key
       change locks both involved buckets in ascending order, so
       concurrent writers cannot deadlock;
     - slot allocation (INSERT), slot recycling (DELETE) and table clear
       are serialized by one per-table struct_mutex, which the hot
       UPDATE/read paths never touch.

   Semantics notes (by design, no transactions):
     - Scans are "loose": rows inserted during a scan may appear, rows
       deleted during a scan are skipped, a recycled slot may reappear
       with a different row.
     - Concurrent writers to the same row: last writer wins.
     - A unique-key violation is detected atomically per bucket; when two
       transactions-less writers race to insert the same unique key,
       exactly one wins and the other gets FM_ERR_DUP_KEY.

   This header has NO dependency on MariaDB server headers so it can be
   unit-tested standalone (see standalone-test/main.cpp).
*/

#ifndef FM_CORE_H_INCLUDED
#define FM_CORE_H_INCLUDED

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>

/* ------------------------------------------------------------------ */
/* Basic types & constants                                            */
/* ------------------------------------------------------------------ */

typedef std::uint32_t fm_u32;
typedef std::uint64_t fm_u64;
typedef std::uint16_t fm_u16;
typedef std::int32_t  fm_i32;
typedef std::int64_t  fm_i64;
typedef std::uint8_t  fm_u8;

#define FM_CHUNK_SHIFT 12u
#define FM_CHUNK_SIZE  (1u << FM_CHUNK_SHIFT)

#define FM_NULL_SLOT   ((fm_i32) -1)

#define FM_MAX_KEYS    64
#define FM_MAX_CHANGED_KEYS 64

/* Return codes (mapped to HA_ERR_* by ha_fastmem) */
enum
{
  FM_ERR_OK= 0,
  FM_ERR_NOT_FOUND= 1,    /* key lookup miss                            */
  FM_ERR_EOF= 2,          /* end of scan                                */
  FM_ERR_DELETED= 3,      /* row absent / stale generation / deleted    */
  FM_ERR_DUP_KEY= 4,      /* unique key violation (errkey out param)    */
  FM_ERR_FULL= 5,         /* max_records / max_heap_table_size reached  */
  FM_ERR_NOMEM= 6,        /* allocation failure                          */
  FM_ERR_WRONG_KEY= 7     /* defensive: inconsistency                   */
};

enum
{
  FM_SLOT_FREE= 0,
  FM_SLOT_ACTIVE= 1,
  FM_SLOT_DELETED= 2,
  FM_SLOT_DELETING= 3     /* delete in progress: writers must refuse     */
};

/* ------------------------------------------------------------------ */
/* Spinlock (writers only; readers never touch it)                    */
/* ------------------------------------------------------------------ */

struct FM_SPIN
{
  std::atomic<fm_u8> f{0};
  void lock()
  {
    while (f.exchange(1, std::memory_order_acquire))
      std::this_thread::yield();
  }
  void unlock() { f.store(0, std::memory_order_release); }
  bool try_lock() { return f.exchange(1, std::memory_order_acquire) == 0; }
};

/* ------------------------------------------------------------------ */
/* Row slot                                                            */
/* ------------------------------------------------------------------ */

/*
  Layout:
    state(1) wlock(1) pad(2) seq(4) gen(4) free_next(4)  -> 16 bytes
    followed by the record image at offset FM_SLOT_DATA_OFFSET

  state     FM_SLOT_* (atomics, acquire/release)
  wlock     serializes writers of this slot
  seq       seqlock: odd while an update is in progress
  gen       generation, bumped on delete and on recycle; validates refs
  free_next freelist chain (only under the share struct_mutex)

  Invariant: a reader only ever copies an image of a slot whose state is
  FM_SLOT_ACTIVE and whose sequence is even and unchanged across the
  copy.  Writers publish by: wlock -> seq odd -> memcpy -> seq even ->
  wlock release; recycling publishes through state=ACTIVE (release),
  insertion through state=ACTIVE (release).
*/
struct FM_SLOT
{
  std::atomic<fm_u8>  state;
  FM_SPIN             wlock;
  std::atomic<fm_u32> seq;
  std::atomic<fm_u32> gen;
  fm_i32              free_next;

  FM_SLOT() : state(FM_SLOT_FREE), seq(0), gen(0), free_next(FM_NULL_SLOT)
  {
    wlock.f.store(0, std::memory_order_relaxed);
  }
};

#define FM_SLOT_DATA_OFFSET ((sizeof(FM_SLOT) + 7u) & ~7u)

/* ------------------------------------------------------------------ */
/* Hash keys                                                           */
/* ------------------------------------------------------------------ */

/*
  The core is key-format agnostic; the server glue (fm_hash.cc) supplies
  these callbacks per key.  "rec" is a full record image, "packed_key" a
  packed search key as produced by the server.
*/
struct FM_KEY_OPS
{
  const void *ud;
  fm_u32 (*hash_rec)    (const void *ud, const fm_u8 *rec);
  fm_u32 (*hash_key)    (const void *ud, const fm_u8 *packed_key);
  int    (*cmp_rec_rec) (const void *ud, const fm_u8 *a, const fm_u8 *b);
  int    (*cmp_rec_key) (const void *ud, const fm_u8 *rec,
                         const fm_u8 *packed_key);
  /* returns true when the record has any NULL part (unique keys with
     nullable parts must skip the duplicate check for such records) */
  int    (*has_null_part)(const void *ud, const fm_u8 *rec);
};

struct FM_KEY_CORE
{
  FM_KEY_OPS ops;
  bool unique;            /* set when the key has HA_NOSAME (glue)       */
  fm_u32 nbuckets;        /* power of two                                */
  fm_i32 *head;           /* nbuckets chain heads (slot id or FM_NULL)   */
  FM_SPIN *locks;         /* nbuckets bucket spinlocks                   */
};

/* ------------------------------------------------------------------ */
/* Chunk & share                                                       */
/* ------------------------------------------------------------------ */

struct FM_CHUNK
{
  fm_u8   *slots;         /* FM_CHUNK_SIZE slots of slot_size bytes     */
  fm_i32 **key_next;      /* [keys] arrays of FM_CHUNK_SIZE int32        */
};

struct FM_SHARE_CORE_OPS
{
  void *(*alloc)(std::size_t);
  void (*dealloc)(void *);
};

struct FM_SHARE_CORE
{
  FM_SHARE_CORE_OPS ops;

  fm_u32 reclength;       /* record image bytes                          */
  fm_u32 slot_size;       /* stride between slots                        */
  fm_u32 keys;            /* number of hash keys                         */
  fm_u32 max_records;     /* hard row limit                              */
  fm_u32 chunk_cap;       /* capacity of the chunks pointer array        */

  FM_CHUNK *chunks;                 /* fixed-size pointer array          */
  std::atomic<fm_u32> chunk_cnt;    /* chunks allocated so far           */

  fm_i32  free_head;                /* freelist, under struct_mutex      */
  std::atomic<fm_i64> records;      /* live row count                    */

  FM_SPIN struct_mutex;   /* INSERT alloc / DELETE push / clear          */

  FM_KEY_CORE *keydef;    /* [keys] or 0                                 */

  /* auto_increment: monotonically increasing counter = last value
     handed out; never reused */
  std::atomic<fm_u64> auto_inc;
  fm_u16 auto_key;        /* 0 = none                                    */
  fm_u8  auto_key_type;
};

/* ------------------------------------------------------------------ */
/* Inline helpers                                                      */
/* ------------------------------------------------------------------ */

static inline fm_u32 fm_align8(fm_u32 x) { return (x + 7u) & ~7u; }

static inline FM_SLOT *fm_slot(FM_SHARE_CORE *s, fm_i32 idx)
{
  return (FM_SLOT *)(s->chunks[idx >> FM_CHUNK_SHIFT].slots +
                     (fm_u32)(idx & (FM_CHUNK_SIZE - 1)) * s->slot_size);
}

static inline const FM_SLOT *fm_slot(const FM_SHARE_CORE *s, fm_i32 idx)
{
  return (const FM_SLOT *)(s->chunks[idx >> FM_CHUNK_SHIFT].slots +
                           (fm_u32)(idx & (FM_CHUNK_SIZE - 1)) * s->slot_size);
}

static inline fm_u8 *fm_slot_data(const FM_SHARE_CORE *s, FM_SLOT *sl)
{
  (void)s;
  return (fm_u8 *)sl + FM_SLOT_DATA_OFFSET;
}

static inline fm_i32 *fm_key_next(FM_SHARE_CORE *s, fm_u32 key, fm_i32 idx)
{
  return &s->chunks[idx >> FM_CHUNK_SHIFT].key_next[key]
                    [idx & (FM_CHUNK_SIZE - 1)];
}

static inline fm_u32 fm_bucket(const FM_KEY_CORE *k, fm_u32 hash)
{
  return hash & (k->nbuckets - 1);
}

/* Number of slots visible to scans = chunk_cnt * CHUNK_SIZE. */
static inline fm_u32 fm_core_slot_count(const FM_SHARE_CORE *s)
{
  return s->chunk_cnt.load(std::memory_order_acquire) << FM_CHUNK_SHIFT;
}

static inline fm_u32 fm_core_key_bucket(const FM_KEY_CORE *kc,
                                        const fm_u8 *rec)
{
  return fm_bucket(kc, kc->ops.hash_rec(kc->ops.ud, rec));
}

/* ------------------------------------------------------------------ */
/* Share lifecycle                                                     */
/* ------------------------------------------------------------------ */

static inline void fm_core_init_share(FM_SHARE_CORE *s,
                                      fm_u32 reclength, fm_u32 keys,
                                      fm_u32 max_records, fm_u32 nbuckets,
                                      const FM_SHARE_CORE_OPS *ops)
{
  std::memset(s, 0, sizeof(*s));
  s->ops= ops ? *ops : FM_SHARE_CORE_OPS{ ::malloc, ::free };
  s->reclength= reclength;
  s->keys= keys;
  s->max_records= max_records ? max_records : 1;
  s->slot_size= fm_align8(FM_SLOT_DATA_OFFSET + reclength);
  s->chunk_cap= (s->max_records + FM_CHUNK_SIZE - 1) / FM_CHUNK_SIZE;
  s->free_head= FM_NULL_SLOT;
  s->records.store(0, std::memory_order_relaxed);
  s->chunk_cnt.store(0, std::memory_order_relaxed);

  s->chunks= (FM_CHUNK *)s->ops.alloc(sizeof(FM_CHUNK) * s->chunk_cap);
  if (!s->chunks)
    return;
  std::memset(s->chunks, 0, sizeof(FM_CHUNK) * s->chunk_cap);

  if (keys)
  {
    s->keydef= (FM_KEY_CORE *)s->ops.alloc(sizeof(FM_KEY_CORE) * keys);
    if (!s->keydef)
      return;
    std::memset(s->keydef, 0, sizeof(FM_KEY_CORE) * keys);
    for (fm_u32 k= 0; k < keys; k++)
    {
      FM_KEY_CORE *kc= &s->keydef[k];
      kc->nbuckets= nbuckets ? nbuckets : 1024;
      kc->head= (fm_i32 *)s->ops.alloc(sizeof(fm_i32) * kc->nbuckets);
      kc->locks= (FM_SPIN *)s->ops.alloc(sizeof(FM_SPIN) * kc->nbuckets);
      if (!kc->head || !kc->locks)
        continue;
      for (fm_u32 b= 0; b < kc->nbuckets; b++)
      {
        kc->head[b]= FM_NULL_SLOT;
        kc->locks[b].f.store(0, std::memory_order_relaxed);
      }
    }
  }
}

static inline void fm_core_free_share(FM_SHARE_CORE *s)
{
  fm_u32 cnt= s->chunk_cnt.load(std::memory_order_relaxed);
  for (fm_u32 i= 0; i < cnt; i++)
  {
    FM_CHUNK *c= &s->chunks[i];
    if (c->slots)
      s->ops.dealloc(c->slots);
    if (c->key_next)
    {
      for (fm_u32 k= 0; k < s->keys; k++)
        if (c->key_next[k])
          s->ops.dealloc(c->key_next[k]);
      s->ops.dealloc(c->key_next);
    }
  }
  if (s->chunks)
    s->ops.dealloc(s->chunks);
  for (fm_u32 k= 0; k < s->keys; k++)
  {
    if (s->keydef[k].head)
      s->ops.dealloc(s->keydef[k].head);
    if (s->keydef[k].locks)
      s->ops.dealloc(s->keydef[k].locks);
  }
  if (s->keydef)
    s->ops.dealloc(s->keydef);
  std::memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* Seqlock-protected record image copy (the lock-free reader path)     */
/* ------------------------------------------------------------------ */

/*
  Copy the record image of slot idx into buf.

  check_gen: when true the slot generation must equal expect_gen
             (positions captured earlier), else FM_ERR_DELETED.

  Return FM_ERR_OK or FM_ERR_DELETED.
*/
static inline int fm_core_read_row(FM_SHARE_CORE *s, fm_i32 idx,
                                   bool check_gen, fm_u32 expect_gen,
                                   fm_u8 *buf)
{
  const FM_SLOT *sl= fm_slot(s, idx);
  for (;;)
  {
    if (sl->state.load(std::memory_order_acquire) != FM_SLOT_ACTIVE)
      return FM_ERR_DELETED;
    if (check_gen &&
        sl->gen.load(std::memory_order_acquire) != expect_gen)
      return FM_ERR_DELETED;
    fm_u32 s1= sl->seq.load(std::memory_order_acquire);
    if (s1 & 1u)
      continue;                                  /* update in progress */
    std::memcpy(buf, (const fm_u8 *)sl + FM_SLOT_DATA_OFFSET,
                s->reclength);
    if (sl->seq.load(std::memory_order_acquire) != s1)
      continue;                                  /* changed, retry */
    return FM_ERR_OK;
  }
}

/* ------------------------------------------------------------------ */
/* Slot allocation (INSERT/DELETE/clear only, under struct_mutex)      */
/* ------------------------------------------------------------------ */

/* Allocate one new chunk.  Caller holds struct_mutex. */
static inline int fm_core_grow_chunk(FM_SHARE_CORE *s)
{
  fm_u32 k= s->chunk_cnt.load(std::memory_order_relaxed);
  if (k >= s->chunk_cap)
    return FM_ERR_FULL;
  FM_CHUNK *c= &s->chunks[k];
  c->slots= (fm_u8 *)s->ops.alloc((std::size_t)s->slot_size * FM_CHUNK_SIZE);
  if (!c->slots)
    return FM_ERR_NOMEM;
  for (fm_u32 i= 0; i < FM_CHUNK_SIZE; i++)
  {
    FM_SLOT *sl= (FM_SLOT *)(c->slots + (std::size_t)i * s->slot_size);
    new (sl) FM_SLOT;                  /* state FREE, seq 0, gen 0 */
  }
  if (s->keys)
  {
    c->key_next= (fm_i32 **)s->ops.alloc(sizeof(fm_i32 *) * s->keys);
    if (!c->key_next)
    {
      s->ops.dealloc(c->slots);
      c->slots= 0;
      return FM_ERR_NOMEM;
    }
    for (fm_u32 kk= 0; kk < s->keys; kk++)
    {
      c->key_next[kk]= (fm_i32 *)s->ops.alloc(sizeof(fm_i32) * FM_CHUNK_SIZE);
      if (!c->key_next[kk])
      {
        for (fm_u32 j= 0; j < kk; j++)
          s->ops.dealloc(c->key_next[j]);
        s->ops.dealloc(c->key_next);
        c->key_next= 0;
        s->ops.dealloc(c->slots);
        c->slots= 0;
        return FM_ERR_NOMEM;
      }
      for (fm_u32 i= 0; i < FM_CHUNK_SIZE; i++)
        c->key_next[kk][i]= FM_NULL_SLOT;
    }
  }
  /* Publish: release store makes all initialisation visible to readers
     that observe the incremented chunk count. */
  s->chunk_cnt.store(k + 1, std::memory_order_release);
  return FM_ERR_OK;
}

/* Pop a free slot (freelist first, else a fresh chunk slot).
   Caller holds struct_mutex.  Returns slot index or FM_NULL_SLOT. */
static inline fm_i32 fm_core_alloc_slot(FM_SHARE_CORE *s)
{
  if (s->free_head != FM_NULL_SLOT)
  {
    fm_i32 idx= s->free_head;
    s->free_head= fm_slot(s, idx)->free_next;
    return idx;
  }
  fm_u32 cnt= s->chunk_cnt.load(std::memory_order_relaxed);
  fm_u32 total= cnt << FM_CHUNK_SHIFT;
  if (total >= s->max_records)
    return FM_NULL_SLOT;                         /* caller reports FULL */
  if ((total & (FM_CHUNK_SIZE - 1)) == 0)
  {
    /* A fresh chunk occupies slots [total, total + CHUNK_SIZE).  Grow it
       and put every slot except the first on the freelist. */
    if (fm_core_grow_chunk(s) != FM_ERR_OK)
      return FM_NULL_SLOT;
    fm_u32 end= total + FM_CHUNK_SIZE;
    if (end > s->max_records)
      end= s->max_records;
    for (fm_u32 i= end; i-- > total + 1; )
    {
      fm_slot(s, (fm_i32)i)->free_next= s->free_head;
      s->free_head= (fm_i32)i;
    }
  }
  return (fm_i32)total;
}

static inline void fm_core_free_slot(FM_SHARE_CORE *s, fm_i32 idx)
{
  fm_slot(s, idx)->free_next= s->free_head;
  s->free_head= idx;
}

/* ------------------------------------------------------------------ */
/* Hash chain operations (caller holds the bucket lock)                */
/* ------------------------------------------------------------------ */

/* Unlink slot idx from the chain of bucket b of key k.
   Returns FM_ERR_OK if found, FM_ERR_NOT_FOUND otherwise (the row may
   have been re-chained by a concurrent key-change update). */
static inline int fm_core_chain_unlink(FM_SHARE_CORE *s, fm_u32 k,
                                       fm_u32 b, fm_i32 idx)
{
  FM_KEY_CORE *kc= &s->keydef[k];
  fm_i32 *link= &kc->head[b];
  while (*link != FM_NULL_SLOT)
  {
    if (*link == idx)
    {
      *link= *fm_key_next(s, k, idx);
      return FM_ERR_OK;
    }
    link= fm_key_next(s, k, *link);
  }
  return FM_ERR_NOT_FOUND;
}

/* Link slot idx at the head of bucket b of key k.  Caller holds the
   bucket lock. */
static inline void fm_core_chain_link(FM_SHARE_CORE *s, fm_u32 k,
                                      fm_u32 b, fm_i32 idx)
{
  FM_KEY_CORE *kc= &s->keydef[k];
  *fm_key_next(s, k, idx)= kc->head[b];
  kc->head[b]= idx;
}

/* ------------------------------------------------------------------ */
/* Reads: scans & point lookups                                        */
/* ------------------------------------------------------------------ */

/*
  Scan next: pos is in/out.  Bound each call by the current slot count
  so rows inserted during the scan may appear ("loose scan").
  Return FM_ERR_OK (buf filled), FM_ERR_EOF.
*/
static inline int fm_core_scan_next(FM_SHARE_CORE *s, fm_u32 *pos,
                                    fm_u8 *buf)
{
  fm_u32 bound= fm_core_slot_count(s);
  while (*pos < bound)
  {
    fm_i32 idx= (fm_i32)(*pos)++;
    if (fm_core_read_row(s, idx, false, 0, buf) == FM_ERR_OK)
      return FM_ERR_OK;
  }
  return FM_ERR_EOF;
}

/* Exact point lookup.  Copies the row into buf, returns the slot in
   *out_idx.  Non-ACTIVE chain nodes (deleted / recycled) are skipped. */
static inline int fm_core_find(FM_SHARE_CORE *s, fm_u32 key,
                               const fm_u8 *packed_key, fm_u8 *buf,
                               fm_i32 *out_idx)
{
  FM_KEY_CORE *kc= &s->keydef[key];
  fm_u32 b= fm_bucket(kc, kc->ops.hash_key(kc->ops.ud, packed_key));
  kc->locks[b].lock();
  fm_i32 h= kc->head[b];
  while (h != FM_NULL_SLOT)
  {
    if (fm_core_read_row(s, h, false, 0, buf) == FM_ERR_OK &&
        kc->ops.cmp_rec_key(kc->ops.ud, buf, packed_key) == 0)
    {
      kc->locks[b].unlock();
      *out_idx= h;
      return FM_ERR_OK;
    }
    h= *fm_key_next(s, key, h);
  }
  kc->locks[b].unlock();
  return FM_ERR_NOT_FOUND;
}

/*
  Continue an exact-key scan: first matching row after cur in chain
  order.  If cur is gone (deleted or re-chained) return the first match
  ("loose" semantics).  Caller must pass the same full packed key.
*/
static inline int fm_core_find_next(FM_SHARE_CORE *s, fm_u32 key,
                                    const fm_u8 *packed_key, fm_i32 cur,
                                    fm_u8 *buf, fm_i32 *out_idx)
{
  FM_KEY_CORE *kc= &s->keydef[key];
  fm_u32 b= fm_bucket(kc, kc->ops.hash_key(kc->ops.ud, packed_key));
  kc->locks[b].lock();
  bool after_cur= false;
  fm_i32 h= kc->head[b];
  while (h != FM_NULL_SLOT)
  {
    if (h == cur)
      after_cur= true;
    else if (after_cur &&
             fm_core_read_row(s, h, false, 0, buf) == FM_ERR_OK &&
             kc->ops.cmp_rec_key(kc->ops.ud, buf, packed_key) == 0)
    {
      kc->locks[b].unlock();
      *out_idx= h;
      return FM_ERR_OK;
    }
    h= *fm_key_next(s, key, h);
  }
  kc->locks[b].unlock();
  return FM_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Writes: insert / update / delete                                    */
/* ------------------------------------------------------------------ */

/*
  Duplicate check for a unique key: walks the chain of bucket b of key k
  comparing every ACTIVE row with rec.  Caller holds the bucket lock and
  (for insert) struct_mutex; rec must not be chained yet, so the walk
  never compares a row with itself.
*/
static inline int fm_core_dup_check(FM_SHARE_CORE *s, fm_u32 k, fm_u32 b,
                                    const fm_u8 *rec, fm_u8 *scratch)
{
  FM_KEY_CORE *kc= &s->keydef[k];
  if (kc->unique &&
      !(kc->ops.has_null_part &&
        kc->ops.has_null_part(kc->ops.ud, rec)))
  {
    fm_i32 h= kc->head[b];
    while (h != FM_NULL_SLOT)
    {
      if (fm_core_read_row(s, h, false, 0, scratch) == FM_ERR_OK &&
          kc->ops.cmp_rec_rec(kc->ops.ud, scratch, rec) == 0)
        return FM_ERR_DUP_KEY;
      h= *fm_key_next(s, k, h);
    }
  }
  return FM_ERR_OK;
}

/*
  Insert a new row.  On FM_ERR_DUP_KEY, *errkey is set to the key index
  and the slot is recycled (the row never becomes visible).
*/
static inline int fm_core_insert_row(FM_SHARE_CORE *s, const fm_u8 *rec,
                                     fm_i32 *out_idx, int *errkey,
                                     fm_u8 *scratch)
{
  s->struct_mutex.lock();
  fm_i32 idx= fm_core_alloc_slot(s);
  if (idx == FM_NULL_SLOT)
  {
    s->struct_mutex.unlock();
    return s->chunk_cnt.load(std::memory_order_relaxed) >= s->chunk_cap
             ? FM_ERR_FULL : FM_ERR_NOMEM;
  }
  FM_SLOT *sl= fm_slot(s, idx);
  std::memcpy(fm_slot_data(s, sl), rec, s->reclength);

  /* Link into every key chain; on a duplicate, roll the previous keys
     back (single bucket at a time, so no lock-ordering issues). */
  for (fm_u32 k= 0; k < s->keys; k++)
  {
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 b= fm_core_key_bucket(kc, rec);
    kc->locks[b].lock();
    int dup= (kc->ops.hash_rec != 0)
               ? fm_core_dup_check(s, k, b, rec, scratch) : FM_ERR_OK;
    if (dup != FM_ERR_OK)
    {
      kc->locks[b].unlock();
      while (k-- > 0)
      {
        FM_KEY_CORE *kc2= &s->keydef[k];
        fm_u32 b2= fm_core_key_bucket(kc2, rec);
        kc2->locks[b2].lock();
        fm_core_chain_unlink(s, k, b2, idx);
        kc2->locks[b2].unlock();
      }
      fm_core_free_slot(s, idx);
      s->struct_mutex.unlock();
      if (errkey) *errkey= (int)k + 1;
      return FM_ERR_DUP_KEY;
    }
    fm_core_chain_link(s, k, b, idx);
    kc->locks[b].unlock();
  }

  /* Publish: the image is complete, then make the row visible. */
  sl->state.store(FM_SLOT_ACTIVE, std::memory_order_release);
  s->records.fetch_add(1, std::memory_order_relaxed);
  s->struct_mutex.unlock();
  *out_idx= idx;
  return FM_ERR_OK;
}

/* Internal: single-slot image write under the seqlock protocol.
   Caller holds the slot wlock. */
static inline void fm_core_write_image(FM_SHARE_CORE *s, FM_SLOT *sl,
                                       const fm_u8 *rec)
{
  sl->seq.fetch_add(1, std::memory_order_relaxed);      /* odd  */
  std::memcpy(fm_slot_data(s, sl), rec, s->reclength);
  sl->seq.fetch_add(1, std::memory_order_release);      /* even */
}

/*
  Update row at idx (generation expect_gen, 0 = don't care).

  When the key columns did not change this is a plain seqlock-protected
  image write (hot path, fully parallel with readers and with writers of
  other rows).

  When key(s) changed, all involved buckets (old and new, per key) are
  locked in one globally ascending order before anything is mutated:
  dup-checks run first (read-only), then the slot is verified and locked
  (wlock) and the image is written, then old chains are unlinked and new
  chains linked.  No rollback is ever needed because nothing is mutated
  until every dup check has passed.
*/
static inline int fm_core_update_row(FM_SHARE_CORE *s, fm_i32 idx,
                                     fm_u32 expect_gen,
                                     const fm_u8 *old_rec,
                                     const fm_u8 *new_rec, int *errkey,
                                     fm_u8 *scratch)
{
  FM_SLOT *sl= fm_slot(s, idx);

  /* --- fast path: no key column changed -------------------------- */
  fm_u16 changed[FM_MAX_CHANGED_KEYS];
  fm_u32 nch= 0;
  for (fm_u32 k= 0; k < s->keys && nch < FM_MAX_CHANGED_KEYS; k++)
  {
    FM_KEY_CORE *kc= &s->keydef[k];
    if (kc->ops.hash_rec &&
        kc->ops.cmp_rec_rec(kc->ops.ud, old_rec, new_rec) != 0)
      changed[nch++]= (fm_u16)k;
  }
  if (nch == 0)
  {
    sl->wlock.lock();
    if (sl->state.load(std::memory_order_acquire) != FM_SLOT_ACTIVE ||
        sl->gen.load(std::memory_order_acquire) != expect_gen)
    {
      sl->wlock.unlock();
      return FM_ERR_DELETED;
    }
    fm_core_write_image(s, sl, new_rec);
    sl->wlock.unlock();
    return FM_ERR_OK;
  }

  /* --- key change path ------------------------------------------- */
  /* Collect every (key, bucket) pair: old and new bucket per changed
     key, deduplicated, sorted ascending, locked in that order. */
  struct FMB { fm_u16 key; fm_u32 b; } lockset[FM_MAX_CHANGED_KEYS * 2];
  fm_u32 nl= 0;
  for (fm_u32 i= 0; i < nch; i++)
  {
    fm_u16 k= changed[i];
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 bnew= fm_core_key_bucket(kc, new_rec);
    fm_u32 bold= fm_core_key_bucket(kc, old_rec);
    bool have_new= false, have_old= false;
    for (fm_u32 j= 0; j < nl; j++)
    {
      if (lockset[j].key == k && lockset[j].b == bnew) have_new= true;
      if (lockset[j].key == k && lockset[j].b == bold) have_old= true;
    }
    if (!have_new) lockset[nl++]= { k, bnew };
    if (!have_old) lockset[nl++]= { k, bold };
  }
  /* simple insertion sort on (key, b) - nl is tiny (<= 128) */
  for (fm_u32 i= 1; i < nl; i++)
  {
    FMB v= lockset[i];
    fm_u32 j= i;
    fm_u64 vi= ((fm_u64)v.key << 32) | v.b;
    while (j > 0)
    {
      FMB p= lockset[j - 1];
      if ((((fm_u64)p.key << 32) | p.b) <= vi) break;
      lockset[j]= p;
      j--;
    }
    lockset[j]= v;
  }
  fm_u32 lock_count= nl;
  for (fm_u32 i= 0; i < nl; i++)
    if (i == 0 || lockset[i].key != lockset[i-1].key ||
        lockset[i].b != lockset[i-1].b)
      s->keydef[lockset[i].key].locks[lockset[i].b].lock();
    else
      lock_count--;       /* duplicate entries: skip (already locked)  */

  /* Dup checks (read-only) for every changed unique key. */
  for (fm_u32 i= 0; i < nch; i++)
  {
    fm_u16 k= changed[i];
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 bnew= fm_core_key_bucket(kc, new_rec);
    int dup= fm_core_dup_check(s, k, bnew, new_rec, scratch);
    if (dup != FM_ERR_OK)
    {
      /* unlock in reverse order */
      for (fm_u32 j= nl; j-- > 0; )
      {
        bool first= (j == 0 || lockset[j].key != lockset[j-1].key ||
                     lockset[j].b != lockset[j-1].b);
        if (first)
          s->keydef[lockset[j].key].locks[lockset[j].b].unlock();
      }
      if (errkey) *errkey= (int)k + 1;
      return FM_ERR_DUP_KEY;
    }
  }

  /* Verify the slot while nothing else can race us. */
  sl->wlock.lock();
  if (sl->state.load(std::memory_order_acquire) != FM_SLOT_ACTIVE ||
      sl->gen.load(std::memory_order_acquire) != expect_gen)
  {
    sl->wlock.unlock();
    for (fm_u32 j= nl; j-- > 0; )
    {
      bool first= (j == 0 || lockset[j].key != lockset[j-1].key ||
                   lockset[j].b != lockset[j-1].b);
      if (first)
        s->keydef[lockset[j].key].locks[lockset[j].b].unlock();
    }
    return FM_ERR_DELETED;
  }

  /* Commit. */
  fm_core_write_image(s, sl, new_rec);
  for (fm_u32 i= 0; i < nch; i++)
  {
    fm_u16 k= changed[i];
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 bnew= fm_core_key_bucket(kc, new_rec);
    fm_u32 bold= fm_core_key_bucket(kc, old_rec);
    fm_core_chain_unlink(s, k, bold, idx);
    fm_core_chain_link(s, k, bnew, idx);
  }
  sl->wlock.unlock();
  for (fm_u32 j= nl; j-- > 0; )
  {
    bool first= (j == 0 || lockset[j].key != lockset[j-1].key ||
                 lockset[j].b != lockset[j-1].b);
    if (first)
      s->keydef[lockset[j].key].locks[lockset[j].b].unlock();
  }
  return FM_ERR_OK;
}

/*
  Update row at idx while the caller already holds sl->wlock (used by the
  handler-level read-modify-write serialization).  Identical semantics to
  fm_core_update_row, but the slot wlock is never taken nor released here:
  it stays owned by the caller for the whole cycle.
*/
static inline int fm_core_update_row_locked(FM_SHARE_CORE *s, fm_i32 idx,
                                            fm_u32 expect_gen,
                                            const fm_u8 *old_rec,
                                            const fm_u8 *new_rec, int *errkey,
                                            fm_u8 *scratch)
{
  FM_SLOT *sl= fm_slot(s, idx);

  /* Caller holds sl->wlock: verify the slot while nobody can race us. */
  if (sl->state.load(std::memory_order_acquire) != FM_SLOT_ACTIVE ||
      sl->gen.load(std::memory_order_acquire) != expect_gen)
    return FM_ERR_DELETED;

  /* --- fast path: no key column changed -------------------------- */
  fm_u16 changed[FM_MAX_CHANGED_KEYS];
  fm_u32 nch= 0;
  for (fm_u32 k= 0; k < s->keys && nch < FM_MAX_CHANGED_KEYS; k++)
  {
    FM_KEY_CORE *kc= &s->keydef[k];
    if (kc->ops.hash_rec &&
        kc->ops.cmp_rec_rec(kc->ops.ud, old_rec, new_rec) != 0)
      changed[nch++]= (fm_u16)k;
  }
  if (nch == 0)
  {
    fm_core_write_image(s, sl, new_rec);
    return FM_ERR_OK;
  }

  /* --- key change path ------------------------------------------- */
  struct FMB { fm_u16 key; fm_u32 b; } lockset[FM_MAX_CHANGED_KEYS * 2];
  fm_u32 nl= 0;
  for (fm_u32 i= 0; i < nch; i++)
  {
    fm_u16 k= changed[i];
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 bnew= fm_core_key_bucket(kc, new_rec);
    fm_u32 bold= fm_core_key_bucket(kc, old_rec);
    bool have_new= false, have_old= false;
    for (fm_u32 j= 0; j < nl; j++)
    {
      if (lockset[j].key == k && lockset[j].b == bnew) have_new= true;
      if (lockset[j].key == k && lockset[j].b == bold) have_old= true;
    }
    if (!have_new) lockset[nl++]= { k, bnew };
    if (!have_old) lockset[nl++]= { k, bold };
  }
  for (fm_u32 i= 1; i < nl; i++)
  {
    FMB v= lockset[i];
    fm_u32 j= i;
    fm_u64 vi= ((fm_u64)v.key << 32) | v.b;
    while (j > 0)
    {
      FMB p= lockset[j - 1];
      if ((((fm_u64)p.key << 32) | p.b) <= vi) break;
      lockset[j]= p;
      j--;
    }
    lockset[j]= v;
  }
  fm_u32 lock_count= nl;
  for (fm_u32 i= 0; i < nl; i++)
    if (i == 0 || lockset[i].key != lockset[i-1].key ||
        lockset[i].b != lockset[i-1].b)
      s->keydef[lockset[i].key].locks[lockset[i].b].lock();
    else
      lock_count--;

  for (fm_u32 i= 0; i < nch; i++)
  {
    fm_u16 k= changed[i];
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 bnew= fm_core_key_bucket(kc, new_rec);
    int dup= fm_core_dup_check(s, k, bnew, new_rec, scratch);
    if (dup != FM_ERR_OK)
    {
      for (fm_u32 j= nl; j-- > 0; )
      {
        bool first= (j == 0 || lockset[j].key != lockset[j-1].key ||
                     lockset[j].b != lockset[j-1].b);
        if (first)
          s->keydef[lockset[j].key].locks[lockset[j].b].unlock();
      }
      if (errkey) *errkey= (int)k + 1;
      return FM_ERR_DUP_KEY;
    }
  }

  fm_core_write_image(s, sl, new_rec);
  for (fm_u32 i= 0; i < nch; i++)
  {
    fm_u16 k= changed[i];
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 bnew= fm_core_key_bucket(kc, new_rec);
    fm_u32 bold= fm_core_key_bucket(kc, old_rec);
    fm_core_chain_unlink(s, k, bold, idx);
    fm_core_chain_link(s, k, bnew, idx);
  }
  for (fm_u32 j= nl; j-- > 0; )
  {
    bool first= (j == 0 || lockset[j].key != lockset[j-1].key ||
                 lockset[j].b != lockset[j-1].b);
    if (first)
      s->keydef[lockset[j].key].locks[lockset[j].b].unlock();
  }
  return FM_ERR_OK;
}

/*
  Delete row at idx.  scratch must be a buffer of at least reclength
  bytes owned by the caller.

  Protocol: take the slot wlock, verify ACTIVE + generation, flip the
  state to DELETING (writers refuse from now on), release the wlock,
  unlink every key chain (using the frozen image to find the buckets),
  then flip to DELETED, bump the generation, and recycle the slot.
*/
static inline int fm_core_delete_row(FM_SHARE_CORE *s, fm_i32 idx,
                                     fm_u32 expect_gen, fm_u8 *scratch)
{
  FM_SLOT *sl= fm_slot(s, idx);
  sl->wlock.lock();
  if (sl->state.load(std::memory_order_acquire) != FM_SLOT_ACTIVE ||
      sl->gen.load(std::memory_order_acquire) != expect_gen)
  {
    sl->wlock.unlock();
    return FM_ERR_DELETED;
  }
  sl->state.store(FM_SLOT_DELETING, std::memory_order_release);
  sl->wlock.unlock();

  /* Image is frozen now (no writer may modify a DELETING slot). */
  std::memcpy(scratch, (const fm_u8 *)sl + FM_SLOT_DATA_OFFSET,
              s->reclength);
  for (fm_u32 k= 0; k < s->keys; k++)
  {
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 b= fm_core_key_bucket(kc, scratch);
    kc->locks[b].lock();
    fm_core_chain_unlink(s, k, b, idx);
    kc->locks[b].unlock();
  }

  sl->state.store(FM_SLOT_DELETED, std::memory_order_release);
  sl->gen.fetch_add(1, std::memory_order_relaxed);
  s->records.fetch_sub(1, std::memory_order_relaxed);

  s->struct_mutex.lock();
  fm_core_free_slot(s, idx);
  s->struct_mutex.unlock();
  return FM_ERR_OK;
}

/*
  Delete row at idx while the caller already holds sl->wlock.  The lock
  stays owned by the caller for the whole cycle (handler-level
  read-modify-write serialization); unlike fm_core_delete_row it is
  never released here.  Everything else is identical.
*/
static inline int fm_core_delete_row_locked(FM_SHARE_CORE *s, fm_i32 idx,
                                            fm_u32 expect_gen, fm_u8 *scratch)
{
  FM_SLOT *sl= fm_slot(s, idx);

  /* Caller holds sl->wlock: verify while nobody can race us. */
  if (sl->state.load(std::memory_order_acquire) != FM_SLOT_ACTIVE ||
      sl->gen.load(std::memory_order_acquire) != expect_gen)
    return FM_ERR_DELETED;
  sl->state.store(FM_SLOT_DELETING, std::memory_order_release);

  /* Image is frozen now (no writer may modify a DELETING slot). */
  std::memcpy(scratch, (const fm_u8 *)sl + FM_SLOT_DATA_OFFSET,
              s->reclength);
  for (fm_u32 k= 0; k < s->keys; k++)
  {
    FM_KEY_CORE *kc= &s->keydef[k];
    fm_u32 b= fm_core_key_bucket(kc, scratch);
    kc->locks[b].lock();
    fm_core_chain_unlink(s, k, b, idx);
    kc->locks[b].unlock();
  }

  sl->state.store(FM_SLOT_DELETED, std::memory_order_release);
  sl->gen.fetch_add(1, std::memory_order_relaxed);
  s->records.fetch_sub(1, std::memory_order_relaxed);

  s->struct_mutex.lock();
  fm_core_free_slot(s, idx);
  s->struct_mutex.unlock();
  return FM_ERR_OK;             /* wlock still held: caller unlocks     */
}

/* ------------------------------------------------------------------ */
/* Clear (DELETE FROM / TRUNCATE semantics)                            */
/* ------------------------------------------------------------------ */

/*
  Free every row: all chains are reset, every slot flipped to FREE with
  an increased generation, the freelist rebuilt, records zeroed.
  Concurrent readers see rows disappear one by one (non-transactional).
  A concurrent INSERT/UPDATE/DELETE is serialized with the clear on the
  struct_mutex (insert/delete) and the bucket/slot locks.
*/
static inline void fm_core_clear(FM_SHARE_CORE *s)
{
  s->struct_mutex.lock();
  for (fm_u32 k= 0; k < s->keys; k++)
  {
    FM_KEY_CORE *kc= &s->keydef[k];
    for (fm_u32 b= 0; b < kc->nbuckets; b++)
    {
      kc->locks[b].lock();
      kc->head[b]= FM_NULL_SLOT;
      kc->locks[b].unlock();
    }
  }
  fm_u32 cnt= s->chunk_cnt.load(std::memory_order_relaxed);
  fm_u32 total= cnt << FM_CHUNK_SHIFT;
  for (fm_u32 i= 0; i < total; i++)
  {
    FM_SLOT *sl= fm_slot(s, (fm_i32)i);
    sl->wlock.lock();
    sl->state.store(FM_SLOT_FREE, std::memory_order_release);
    sl->gen.fetch_add(1, std::memory_order_relaxed);
    sl->wlock.unlock();
  }
  s->free_head= FM_NULL_SLOT;
  for (fm_u32 i= 0; i < total; i++)
  {
    FM_SLOT *sl= fm_slot(s, (fm_i32)i);
    sl->free_next= s->free_head;
    s->free_head= (fm_i32)i;
  }
  s->records.store(0, std::memory_order_relaxed);
  s->struct_mutex.unlock();
}

/* ------------------------------------------------------------------ */
/* Auto increment                                                      */
/* ------------------------------------------------------------------ */

/*
  Reserve an interval of auto_increment values.

  Returns the first value of the interval (>= 1), advances the table
  counter past the whole interval, sets *reserved to the number of
  values safely reserved.  Values respect auto_increment_offset /
  auto_increment_increment.  Concurrent callers receive disjoint
  intervals because reservation is a CAS loop on the share counter.

  Returns 0 on overflow (caller should report an auto-inc error).
*/
static inline fm_u64 fm_core_auto_inc_reserve(FM_SHARE_CORE *s,
                                              fm_u64 offset, fm_u64 inc,
                                              fm_u64 nb, fm_u64 *reserved)
{
  if (offset == 0) offset= 1;
  if (inc == 0) inc= 1;
  if (nb == 0) nb= 1;
  for (;;)
  {
    fm_u64 cur= s->auto_inc.load(std::memory_order_acquire);
    fm_u64 v= cur + 1;
    if (v == 0)
      return 0;                                  /* overflow            */
    if (inc > 1)
    {
      /* smallest v' >= v with v' == offset (mod inc) */
      fm_u64 r= (v > offset) ? (v - offset) : 0;
      fm_u64 steps= (r + inc - 1) / inc;
      fm_u64 v2= offset + steps * inc;
      if (v2 < v)                                /* wrap around */
        return 0;
      v= v2;
      if (v <= cur)
      {
        v= cur + inc;
        if (v <= cur)
          return 0;                              /* overflow            */
        fm_u64 steps2= (v > offset) ? (v - offset) : 0;
        fm_u64 v3= offset + ((steps2 + inc - 1) / inc) * inc;
        if (v3 < v || v3 == 0)
          return 0;
        v= v3;
      }
    }
    fm_u64 last= v + (nb - 1) * inc;
    if (last < v)
      return 0;                                  /* overflow            */
    if (s->auto_inc.compare_exchange_weak(cur, last,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
    {
      *reserved= nb;
      return v;
    }
  }
}

/* Raise the counter above an explicitly inserted value. */
static inline void fm_core_auto_inc_observe(FM_SHARE_CORE *s, fm_u64 value)
{
  fm_u64 cur= s->auto_inc.load(std::memory_order_relaxed);
  while (value > cur &&
         !s->auto_inc.compare_exchange_weak(cur, value,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed))
  {
  }
}

#endif /* FM_CORE_H_INCLUDED */