# FASTMEM

**Row-lock in-memory storage engine for MariaDB 12.3 — no table locks, lock-free readers**

[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![MariaDB](https://img.shields.io/badge/MariaDB-12.3-003545.svg)](https://mariadb.org)
[![Platform](https://img.shields.io/badge/platform-Linux%2FWindows-4A4A55.svg)]()

English | [简体中文](README.zh-CN.md)

FASTMEM is an in-memory storage engine for MariaDB (this repository is the
**MariaDB 12.3** line, validated against **12.3.3** on both Linux and
**Windows/MSVC**) designed to outperform the built-in **MEMORY (heap)**
engine under high-concurrency read/write workloads:

- **No table-level locks at all** — `store_lock()` returns zero `THR_LOCK_DATA` entries.
- **Lock-free readers** — row images are copied through a seqlock (odd/even sequence + retry); readers never block writers and vice versa.
- **Row-granular writers** — per-slot spinlocks plus per-bucket hash-chain spinlocks.

## Related release lines（相关版本线）

FASTMEM ships for several server generations sharing the same lock-free core (`fm_core.h`):

- **MariaDB 12.3** — this repository · `v1.0-mariadb123` (Linux `.so` + Windows `.dll`)
- **MariaDB 11.8 LTS** — [gjb711/fastmem-mariadb118](https://github.com/gjb711/fastmem-mariadb118) · `v1.0-mariadb118`
- **MySQL 9.7.0 LTS** — [gjb711/fastmem-mysql97](https://github.com/gjb711/fastmem-mysql97) · `v1.0-mysql97`
- **MySQL 8.4 LTS** — [gjb711/fastmem-mysql84](https://github.com/gjb711/fastmem-mysql84) · `v1.0-mysql84`
- **MySQL 5.7** — [gjb711/fastmem-mysql57](https://github.com/gjb711/fastmem-mysql57) · `v1.1-mysql57`

> This repository is the **MariaDB 12.3** line.

## Why FASTMEM?

| Problem with MEMORY (heap) | FASTMEM |
|---|---|
| One whole-table lock serializes **every** statement | No table lock, no THR_LOCK entries |
| Readers block behind writers (and vice versa) | Readers are completely lock-free (seqlock) |
| Concurrent `UPDATE` is table-serialized | Row-level read-modify-write serialization, hot rows don't block cold rows |
| Trivial auto-increment relies on the table lock | CAS interval reservation — safe under concurrency |

Full design rationale, the concurrency model and correctness proofs are in **[DESIGN.md](DESIGN.md)**.

## Features

- No table locks (`store_lock()` returns empty; no waiter lists, no lock contention point)
- **Seqlock readers**: tear-free row copy, practically zero retries under real load
- **Per-slot writer spinlock** + per-bucket hash spinlock; correct concurrent chain insert/unlink
- **Statement-level read-modify-write serialization**: concurrent `UPDATE`/`DELETE` can never lose an update (verified on a real server: SUM delta exactly equals the statement count under 8-writer hot-row contention)
- **Concurrent-safe `AUTO_INCREMENT`**: CAS interval reservation → disjoint ranges per statement, no duplicate ids on parallel inserts
- Fixed-size slots: zero allocation on hot paths, rows replaced by a single `memcpy`
- 8-byte position refs = slot id + generation → stale references can never dangle
- **Hash-only indexes** (explicit `BTREE` is rejected at `CREATE`, no fake support)
- `TRUNCATE` / clear recycles memory; auto-increment counter reset supported
- Non-transactional (`HA_NO_TRANSACTIONS`), statement-level semantics, last-writer-wins

## Quick start

```sql
INSTALL SONAME 'ha_fastmem';           -- or start mariadbd with --plugin-load-add=ha_fastmem

CREATE TABLE t (
  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  v  BIGINT NOT NULL
) ENGINE=FASTMEM;

INSERT INTO t (v) VALUES (1),(2),(3);
SELECT * FROM t WHERE id = 2;          -- lock-free read
UPDATE t SET v = v + 1 WHERE id = 2;   -- row-level serialized read-modify-write
```

> **Note (maturity):** since v1.1 FASTMEM declares `GAMMA` maturity, so it
> loads on MariaDB ≥ 11.x/12.x out of the box (the default
> `plugin_maturity=AUTO` accepts it). Either way: do **not** `INSTALL PLUGIN`
> a plugin that was already loaded by `plugin-load-add` — that double
> registration can crash the server.

Same public contract as the MEMORY engine:

- row limit governed by `max_heap_table_size` (estimated at `CREATE`)
- data is **not persisted**: server restart empties the table (definition survives)
- no transactions, no foreign keys

## Building

### 0) One-command install from source (recommended)

The repository is a source distribution: clone it, run one script, get a
built plugin. No prebuilt binaries, no GLIBC/ABI compatibility concerns —
the script fetches the MariaDB server source of your chosen version, drops
`storage/fastmem/` into it and builds with the server's own cmake.

```bash
git clone https://github.com/gjb711/fastmem-mariadb123.git
cd fastmem-mariadb123
./install.sh --detect              # auto-build for YOUR local server version
./install.sh --mariadb-version 12.3.3     # or any 12.x version
```

`--detect` is the zero-guessing path: it reads your installed
`mariadbd --version` (including common non-PATH locations such as
`/usr/sbin` or BaoTa's `/www/server/mysql/bin/`) and builds exactly that
version — so the plugin always matches the server ABI (MariaDB refuses
plugins built for another patch release with "API version ... not
supported" / missing symbols). If GitHub is unreachable, the script
falls back to the official release tarball from `archive.mariadb.org`
(submodules bundled, no git needed).

| Flag | Meaning |
|---|---|
| `--detect` | build for your local server's exact version (recommended) |
| `--mariadb-version <v>` | target version (default `12.3.3`); tries tag `maria-<v>`/`mariadb-<v>`, then branch `<v>` |
| `--branch <name>` | fetch a specific branch/tag instead |
| `--source-dir <dir>` | reuse an existing MariaDB source tree (no clone) |
| `--build-dir <dir>` | cmake build directory (default `<src>/build_fastmem`) |
| `--jobs <n>` | parallel build jobs |
| `--plugin-dir <dir>` | install plugin here (default: auto-detect) |
| `--dry-run` | print the plan, change nothing |

**Validated end-to-end against MariaDB 12.3.3, both platforms**:

- **Windows (MSVC, VS2022/x64)**: adapted to the 12.3 `my_hasher_st` hash
  API, compiled out-of-the-box from the full 12.3.3 source tree →
  `ha_fastmem.dll` → loaded into a local **MariaDB 12.3.3** server and
  smoke-tested on the real server (engine list, create/insert/update/
  delete, PK + multi-row hash index scan, 8-client × 200 hot-row updates
  with exact SUM zero-loss check).
- **Linux (GCC, Ubuntu 22.04 / official `mariadb:12.3.3` Docker image)**:
  `ha_fastmem.so` compiled from the same sources → loaded into the official
  container → **8/8 smoke checks PASS** (engine list, create/insert/
  readback, update, multi-row secondary hash index scan, delete, 8-client ×
  200 hot-row zero-loss, drop) with `ha_fastmem.so` shipped as a release
  asset too.

Because each MariaDB patch release may change the plugin ABI, always build
the plugin for the same version as the running server — `--detect` handles
this automatically.

### 1) Fastest check: standalone concurrency-core test

No server, no cmake. Needs any C++17 compiler:

```bash
cd standalone-test && g++ -std=c++17 -O2 -pthread -o fmtest main.cpp && ./fmtest
```

It runs 4 concurrency suites: tear detection, unique-key integrity under parallel writers, stale-reference safety, multi-key accounting.

### 2) As a MariaDB plugin

Place `storage/fastmem/` inside a MariaDB 12.3.x source tree, then configure:

```bash
cmake -S <src> -B <src>/build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DWITH_SSL=system -DWITH_WSREP=OFF -DPLUGIN_ROCKSDB=NO -DPLUGIN_MROONGA=NO ...
cmake --build <src>/build --config RelWithDebInfo --target fastmem -- -j$(nproc)
```

`CMakeLists.txt` uses `MYSQL_ADD_PLUGIN(fastmem ... STORAGE_ENGINE)` and is
picked up automatically by the top-level `CONFIGURE_PLUGINS()`.

## Validation — Windows MariaDB 12.3.3 (real server, smoke log)

Runs below were executed on the local Windows **MariaDB 12.3.3** with the
built `ha_fastmem.dll` in `lib\plugin` (fresh datadir, port 3399):

| Check | Result |
|---|---|
| `SHOW ENGINES` lists FASTMEM | ✅ `Lock-free in-memory tables (seqlock row images, per-slot writers)` |
| `CREATE TABLE ... ENGINE=FASTMEM` + AUTO_INCREMENT | ✅ |
| INSERT 5 rows → `COUNT(*)` = 5, ORDER BY readback | ✅ 10/20/30/40/50 |
| `UPDATE ... SET v=v+1 WHERE id=3` | ✅ 30 → 31 |
| **Multi-row secondary hash index scan** `WHERE name='apple'` | ✅ 2 rows returned (index-next path) |
| `DELETE` + `COUNT(*)` | ✅ 4 → 3 |
| `DROP TABLE` | ✅ clean |
| **Zero-loss**: 8 clients × 200 `UPDATE` on 100 hot rows | ✅ `SUM` delta = 1600 exactly (= 8×200, no lost updates) |

## Prebuilt binaries（预编译插件）

Windows `ha_fastmem.dll` (MSVC, MariaDB 12.3.3) is attached to the
[v1.0-mariadb123 release](https://github.com/gjb711/fastmem-mariadb123/releases).
For any other server version, build on the target machine with
`./install.sh --detect`.