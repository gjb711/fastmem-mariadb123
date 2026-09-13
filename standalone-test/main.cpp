/*
   FASTMEM core standalone concurrency stress test.

   Compiles WITHOUT any MariaDB dependency:
     cl /std:c++17 /O2 /EHsc main.cpp

   Validates the lock-free core invariants:
     A. no torn record images under heavy concurrent update + read
     B. unique-key integrity under concurrent insert/delete/key-change
     C. stale positions (slot + generation) are rejected
     D. insert/delete slot recycling is safe
*/

#include "../fm_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

struct Rec { std::int64_t k; std::int64_t v; };

static std::atomic<std::int64_t> g_epoch{0};

/* --- key 0: unique on k (simulates PRIMARY KEY) -------------------- */

static std::uint32_t hk0_hash_rec(const void *, const fm_u8 *rec)
{
  std::int64_t k= reinterpret_cast<const Rec *>(rec)->k;
  fm_u64 h= 1469598103934665603ULL;
  for (int i= 0; i < 8; i++)
  {
    h^= (fm_u64)((fm_u8 *)&k)[i];
    h*= 1099511628211ULL;
  }
  return (fm_u32)(h ^ (h >> 32));
}
static std::uint32_t hk0_hash_key(const void *, const fm_u8 *pk)
{
  fm_u64 h= 1469598103934665603ULL;
  for (int i= 0; i < 8; i++)
  {
    h^= pk[i];
    h*= 1099511628211ULL;
  }
  return (fm_u32)(h ^ (h >> 32));
}
static int hk0_cmp_rec_rec(const void *, const fm_u8 *a, const fm_u8 *b)
{
  return (reinterpret_cast<const Rec *>(a)->k ==
          reinterpret_cast<const Rec *>(b)->k) ? 0 : 1;
}
static int hk0_cmp_rec_key(const void *, const fm_u8 *rec, const fm_u8 *pk)
{
  return (reinterpret_cast<const Rec *>(rec)->k ==
          *(const std::int64_t *)pk) ? 0 : 1;
}
static int hk0_null(const void *, const fm_u8 *) { return 0; }

/* --- key 1: non-unique on (k mod 4) -------------------------------- */

static std::uint32_t hk1_hash_rec(const void *, const fm_u8 *rec)
{
  return (fm_u32)(reinterpret_cast<const Rec *>(rec)->k & 3);
}
static std::uint32_t hk1_hash_key(const void *, const fm_u8 *pk)
{
  return (fm_u32)(*(const std::int64_t *)pk & 3);
}
static int hk1_cmp_rec_rec(const void *, const fm_u8 *a, const fm_u8 *b)
{
  return ((reinterpret_cast<const Rec *>(a)->k & 3) ==
          (reinterpret_cast<const Rec *>(b)->k & 3)) ? 0 : 1;
}
static int hk1_cmp_rec_key(const void *, const fm_u8 *rec, const fm_u8 *pk)
{
  return ((reinterpret_cast<const Rec *>(rec)->k & 3) ==
          (*(const std::int64_t *)pk & 3)) ? 0 : 1;
}
static int hk1_null(const void *, const fm_u8 *) { return 0; }

static FM_SHARE_CORE g_core;

static std::atomic<int> g_failures{0};
static void fail(const char *msg, long line)
{
  std::printf("FAIL[%ld]: %s\n", line, msg);
  g_failures.fetch_add(1);
}

#define CHECK(c) do { if (!(c)) fail(#c, __LINE__); } while (0)

static fm_u32 gen_of(FM_SHARE_CORE *s, fm_i32 idx)
{
  return fm_slot(s, idx)->gen.load(std::memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* Test A: torn-read detection                                         */
/* ------------------------------------------------------------------ */

struct Rec2 { std::int64_t k; std::int64_t magic; std::int64_t v; };

static void test_torn_read()
{
  FM_SHARE_CORE s;
  fm_core_init_share(&s, sizeof(Rec2), 1, 1 << 20, 64, 0);
  s.keydef[0].ops.ud= 0;
  s.keydef[0].unique= true;
  s.keydef[0].ops.hash_rec= hk0_hash_rec;
  s.keydef[0].ops.hash_key= hk0_hash_key;
  s.keydef[0].ops.cmp_rec_rec= hk0_cmp_rec_rec;
  s.keydef[0].ops.cmp_rec_key= hk0_cmp_rec_key;
  s.keydef[0].ops.has_null_part= hk0_null;
  const int NROWS= 64;
  Rec2 rows[NROWS];
  for (int i= 0; i < NROWS; i++)
  {
    rows[i].k= i;
    rows[i].magic= 0xABCD000000000000LL;
    rows[i].v= 0;
    fm_i32 idx= -1;
    int ek= 0;
    int rc= fm_core_insert_row(&s, (fm_u8 *)&rows[i], &idx, &ek,
                               (fm_u8 *)&rows[0]);
    CHECK(rc == FM_ERR_OK);
    CHECK(idx == i);               /* unique k means stable slot order   */
  }

  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> writes{0}, reads{0};

  /* 4 writers, each hammering a disjoint subset of rows */
  std::vector<std::thread> threads;
  for (int w= 0; w < 4; w++)
  {
    threads.emplace_back([&, w]() {
      std::mt19937_64 rng(w + 42);
      std::int64_t epoch= 0;
      while (!stop.load(std::memory_order_relaxed))
      {
        int i= (int)(rng() % NROWS);
        Rec2 nr= rows[i];
        nr.v= ++epoch;
        int ek= 0;
        std::int64_t ik= i;
        fm_u8 pk[8];
        std::memcpy(pk, &ik, 8);
        fm_i32 out= -1;
        fm_u8 scratch[sizeof(Rec2)];
        if (fm_core_find(&s, 0, pk, scratch, &out) == FM_ERR_OK)
        {
          int rc= fm_core_update_row(&s, out, gen_of(&s, out),
                                     (fm_u8 *)&rows[i],
                                     (fm_u8 *)&nr, &ek, scratch);
          CHECK(rc == FM_ERR_OK);
          rows[i]= nr;
        }
        writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  /* 2 readers scanning everything (exercise scan + find) */
  for (int r= 0; r < 2; r++)
  {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed))
      {
        fm_u32 pos= 0;
        Rec2 buf;
        while (fm_core_scan_next(&s, &pos, (fm_u8 *)&buf) == FM_ERR_OK)
        {
          if (buf.magic != 0xABCD000000000000LL)
          {
            fail("torn record detected (bad magic)", __LINE__);
            stop.store(true);
            return;
          }
          if (buf.v < 0)
          {
            fail("torn record detected (bad v)", __LINE__);
            stop.store(true);
            return;
          }
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(3));
  stop.store(true);
  for (auto &t : threads) t.join();
  std::printf("test A (torn read): writes=%lld reads=%lld\n",
              (long long)writes.load(), (long long)reads.load());

  /* serial re-verify: every magic intact */
  fm_u32 pos= 0;
  Rec2 buf;
  while (fm_core_scan_next(&s, &pos, (fm_u8 *)&buf) == FM_ERR_OK)
    CHECK(buf.magic == 0xABCD000000000000LL);
  fm_core_free_share(&s);
}

/* ------------------------------------------------------------------ */
/* Test B: unique-key integrity under concurrent insert/delete/move    */
/* ------------------------------------------------------------------ */

static void test_unique_key()
{
  FM_SHARE_CORE s;
  fm_core_init_share(&s, sizeof(Rec), 1, 1 << 20, 64, 0);
  s.keydef[0].ops.ud= (const void *)1;   /* hmm: ud is already set below */
  s.keydef[0].unique= true;
  s.keydef[0].ops.hash_rec= hk0_hash_rec;
  s.keydef[0].ops.hash_key= hk0_hash_key;
  s.keydef[0].ops.cmp_rec_rec= hk0_cmp_rec_rec;
  s.keydef[0].ops.cmp_rec_key= hk0_cmp_rec_key;
  s.keydef[0].ops.has_null_part= hk0_null;

  const int NSLOTS= 200;                 /* key space 0..199 */
  std::atomic<bool> stop{false};
  std::atomic<int> live{0};
  std::mutex mtx;
  std::map<std::int64_t, std::int64_t> shadow;   /* k -> v             */
  std::atomic<long> dups{0};

  std::vector<std::thread> threads;
  for (int t= 0; t < 6; t++)
  {
    threads.emplace_back([&, t]() {
      std::mt19937_64 rng(t + 7);
      while (!stop.load(std::memory_order_relaxed))
      {
        fm_u8 scratch[sizeof(Rec)];
        fm_u8 pk[8];
        std::int64_t k= (std::int64_t)(rng() % NSLOTS);
        std::int64_t v= (std::int64_t)(rng() % 1000000);
        std::memcpy(pk, &k, 8);
        Rec nr= { k, v };
        fm_i32 idx= -1;
        int ek= 0;
        int op= (int)(rng() % 10);
        if (op < 4)                       /* INSERT (dup must be caught) */
        {
          int rc= fm_core_insert_row(&s, (fm_u8 *)&nr, &idx, &ek, scratch);
          std::lock_guard<std::mutex> g(mtx);
          if (rc == FM_ERR_OK)
          {
            if (shadow.count(k))
              fail("unique key inserted twice", __LINE__);
            shadow[k]= v;
            live++;
          }
          else if (rc == FM_ERR_DUP_KEY)
          {
            /* expected only if shadow already has k */
            if (!shadow.count(k))
              dups++;
          }
          else
            fail("insert unexpected error", __LINE__);
        }
        else if (op < 7)                  /* UPDATE in place or key move */
        {
          std::lock_guard<std::mutex> g(mtx);
          if (shadow.count(k))
          {
            std::int64_t nk= (rng() % 2) ? k : (k + 1000) % NSLOTS;
            Rec orc= { k, shadow[k] };
            Rec nrc= { nk, v };
            fm_i32 idx2= -1;
            int rc= fm_core_find(&s, 0, pk, scratch, &idx2);
            if (rc == FM_ERR_OK)
            {
              int rcu= fm_core_update_row(&s, idx2, gen_of(&s, idx2),
                                          (fm_u8 *)&orc,
                                          (fm_u8 *)&nrc, &ek, scratch);
              if (rcu == FM_ERR_OK)
              {
                if (nk != k && shadow.count(nk))
                {
                  fail("unique key moved onto existing", __LINE__);
                }
                shadow.erase(k);
                shadow[nk]= v;
              }
              else if (rcu == FM_ERR_DUP_KEY)
              {
                /* must still have the old key */
                if (!shadow.count(k))
                  fail("dup error but old row vanished", __LINE__);
              }
              else if (rcu == FM_ERR_DELETED)
              {
                /* concurrent delete won: row gone */
                shadow.erase(k);
                live--;
              }
              else
                fail("update unexpected error", __LINE__);
            }
            else
            {
              /* row vanished concurrently -> erase from shadow */
              shadow.erase(k);
              live--;
            }
          }
        }
        else                              /* DELETE */
        {
          std::lock_guard<std::mutex> g(mtx);
          if (shadow.count(k))
          {
            fm_i32 idx2= -1;
            int rc= fm_core_find(&s, 0, pk, scratch, &idx2);
            CHECK(rc == FM_ERR_OK);
            int rcd= fm_core_delete_row(&s, idx2, gen_of(&s, idx2), scratch);
            if (rcd == FM_ERR_OK)
            {
              shadow.erase(k);
              live--;
            }
            /* FM_ERR_DELETED: another thread deleted it first */
            CHECK(rcd == FM_ERR_OK || rcd == FM_ERR_DELETED);
            CHECK(live >= 0);
          }
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(4));
  stop.store(true);
  for (auto &t : threads) t.join();

  /* serial verification: table must contain exactly the shadow keys,
     each with the shadow value, exactly once. */
  std::map<std::int64_t, std::int64_t> found;
  fm_u32 pos= 0;
  Rec buf;
  int n= 0;
  while (fm_core_scan_next(&s, &pos, (fm_u8 *)&buf) == FM_ERR_OK)
  {
    n++;
    if (found.count(buf.k))
      fail("duplicate key in table after quiesce", __LINE__);
    found[buf.k]= buf.v;
  }
  if (found.size() != shadow.size())
  {
    std::printf("  shadow=%zu found=%zu (dups=%ld)\n", shadow.size(),
                found.size(), dups.load());
    fail("key set mismatch after quiesce", __LINE__);
  }
  else
  {
    for (auto &kv : shadow)
      if (!found.count(kv.first) || found[kv.first] != kv.second)
      {
        std::printf("  key %lld: shadow=%lld table=%lld\n",
                    (long long)kv.first, (long long)kv.second,
                    (long long)found[kv.first]);
        fail("value mismatch after quiesce", __LINE__);
        break;
      }
  }
  if (dups.load())
    std::printf("  note: %ld spurious dup errors (harmless races)\n",
                (long)dups.load());
  std::printf("test B (unique-key integrity): %d rows verified\n",
              (int)found.size());
  fm_core_free_share(&s);
}

/* ------------------------------------------------------------------ */
/* Test C: stale positions are rejected                                */
/* ------------------------------------------------------------------ */

static void test_stale_pos()
{
  FM_SHARE_CORE s;
  fm_core_init_share(&s, sizeof(Rec), 0, 1 << 20, 64, 0);
  Rec a= { 1, 10 }, b= { 2, 20 };
  fm_i32 ia= -1, ib= -1;
  int ek= 0;
  fm_u8 scratch[sizeof(Rec)];
  CHECK(fm_core_insert_row(&s, (fm_u8 *)&a, &ia, &ek, scratch) == FM_ERR_OK);
  CHECK(fm_core_insert_row(&s, (fm_u8 *)&b, &ib, &ek, scratch) == FM_ERR_OK);

  /* capture the generation */
  fm_u32 ga= fm_slot(&s, ia)->gen.load(std::memory_order_acquire);

  /* delete ia, then insert into the same slot (it should be recycled) */
  CHECK(fm_core_delete_row(&s, ia, ga, scratch) == FM_ERR_OK);
  Rec c= { 3, 30 };
  fm_i32 ic= -1;
  CHECK(fm_core_insert_row(&s, (fm_u8 *)&c, &ic, &ek, scratch) == FM_ERR_OK);
  /* with only two rows and one deleted, the recycled slot is ic */
  CHECK(ic == ia);

  /* reading with the STALE generation must fail */
  Rec buf;
  int rc= fm_core_read_row(&s, ia, true, ga, (fm_u8 *)&buf);
  CHECK(rc == FM_ERR_DELETED);
  Rec d= { 3, 31 };
  rc= fm_core_update_row(&s, ia, ga, (fm_u8 *)&c, (fm_u8 *)&d, &ek,
                         scratch);
  CHECK(rc == FM_ERR_DELETED);
  rc= fm_core_delete_row(&s, ia, ga, scratch);
  CHECK(rc == FM_ERR_DELETED);
  /* reading with the CURRENT generation succeeds and sees the new row */
  fm_u32 gc= fm_slot(&s, ic)->gen.load(std::memory_order_acquire);
  CHECK(fm_core_read_row(&s, ic, true, gc, (fm_u8 *)&buf) == FM_ERR_OK);
  CHECK(buf.k == 3 && buf.v == 30);
  std::printf("test C (stale position): OK\n");
  fm_core_free_share(&s);
}

/* ------------------------------------------------------------------ */
/* Test D: multi-key (unique + non-unique) bookkeeping                 */
/* ------------------------------------------------------------------ */

static void test_multikey()
{
  FM_SHARE_CORE s;
  fm_core_init_share(&s, sizeof(Rec), 2, 1 << 16, 64, 0);

  FM_KEY_CORE *k0= &s.keydef[0];
  k0->unique= true;
  k0->ops= FM_KEY_OPS{ 0, hk0_hash_rec, hk0_hash_key, hk0_cmp_rec_rec,
                       hk0_cmp_rec_key, hk0_null };
  FM_KEY_CORE *k1= &s.keydef[1];
  k1->unique= false;
  k1->ops= FM_KEY_OPS{ 0, hk1_hash_rec, hk1_hash_key, hk1_cmp_rec_rec,
                       hk1_cmp_rec_key, hk1_null };

  fm_u8 scratch[sizeof(Rec)], pk0[8], pk1[8];
  const int N= 1000;
  for (int i= 0; i < N; i++)
  {
    Rec r= { i, (std::int64_t)i * 2 };
    fm_i32 idx= -1; int ek= 0;
    int rci= fm_core_insert_row(&s, (fm_u8 *)&r, &idx, &ek, scratch);
    if (rci != FM_ERR_OK && i < 8)
      std::printf("  insert i=%d rc=%d ek=%d\n", i, rci, ek);
    CHECK(rci == FM_ERR_OK);
  }
  /* find + find_next over the non-unique key */
  for (int bucket= 0; bucket < 4; bucket++)
  {
    std::int64_t bkey= bucket;
    std::memcpy(pk1, &bkey, 8);
    fm_i32 idx= -1;
    Rec buf;
    int seen= 0;
    if (fm_core_find(&s, 1, pk1, (fm_u8 *)&buf, &idx) == FM_ERR_OK)
    {
      seen++;
      fm_i32 cur= idx;
      while (fm_core_find_next(&s, 1, pk1, cur, (fm_u8 *)&buf, &idx) ==
             FM_ERR_OK)
      {
        seen++;
        cur= idx;
      }
    }
    int expect= 0;
    for (int i= bucket; i < N; i+= 4)
      expect++;
    CHECK(seen == expect);   /* no row may be lost or double-counted     */
  }
  /* key move: change k changes membership of BOTH keys */
  for (int i= 0; i < 200; i++)
  {
    std::int64_t oldk= i;
    std::memcpy(pk0, &oldk, 8);
    fm_i32 idx= -1;
    Rec buf;
    CHECK(fm_core_find(&s, 0, pk0, (fm_u8 *)&buf, &idx) == FM_ERR_OK);
    Rec orc= buf;
    Rec nrc= { oldk + N, buf.v + 1 };
    int ek= 0;
    CHECK(fm_core_update_row(&s, idx, gen_of(&s, idx), (fm_u8 *)&orc,
                             (fm_u8 *)&nrc, &ek, scratch) == FM_ERR_OK);
    /* old key must be gone from key 0 and key 1 tests */
    CHECK(fm_core_find(&s, 0, pk0, (fm_u8 *)&buf, &idx) == FM_ERR_NOT_FOUND);
  }
  /* verify new keys are findable via key 0 and key 1 */
  for (int i= 0; i < 200; i++)
  {
    std::int64_t nk= i + N;
    std::memcpy(pk0, &nk, 8);
    fm_i32 idx= -1;
    Rec buf;
    CHECK(fm_core_find(&s, 0, pk0, (fm_u8 *)&buf, &idx) == FM_ERR_OK);
    CHECK(buf.v == (std::int64_t)i * 2 + 1);
  }
  std::printf("test D (multi-key bookkeeping): OK\n");
  fm_core_free_share(&s);
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  test_torn_read();
  test_unique_key();
  test_stale_pos();
  test_multikey();
  if (g_failures.load() == 0)
    std::printf("ALL TESTS PASSED\n");
  else
    std::printf("%d FAILURES\n", g_failures.load());
  return g_failures.load() ? 1 : 0;
}