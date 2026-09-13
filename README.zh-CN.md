# FASTMEM

**面向 MariaDB 12.3 的行锁内存表存储引擎 —— 无表锁 · 无锁读**

[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![MariaDB](https://img.shields.io/badge/MariaDB-12.3-003545.svg)](https://mariadb.org)
[![Platform](https://img.shields.io/badge/platform-Linux%2FWindows-4A4A55.svg)]()

[English](README.md) | 简体中文

FASTMEM 是面向 MariaDB 的内存存储引擎（本仓库为 **MariaDB 12.3** 线，
已按 **12.3.3** 在 **Linux 与 Windows/MSVC** 上构建、并在 Windows 真机验证），
目标是在高并发读写场景下超越内置 **MEMORY（heap）** 引擎：

- **完全没有表级锁** —— `store_lock()` 返回**零个** `THR_LOCK_DATA`；
- **无锁读者** —— 通过 seqlock（奇偶序号 + 重试）复制行镜像，读者永不阻塞写者、写者也永不阻塞读者；
- **行级写者** —— 每行槽一把自旋锁 + 每哈希桶链一把自旋锁。

## 相关版本线

FASTMEM 以同一份无锁核心（`fm_core.h`）服务于多代服务器：

- **MariaDB 12.3** —— 本仓库 · `v1.0-mariadb123`（Linux `.so` + Windows `.dll`）
- **MariaDB 11.8 LTS** —— [gjb711/fastmem-mariadb118](https://github.com/gjb711/fastmem-mariadb118) · `v1.0-mariadb118`
- **MySQL 9.7.0 LTS** —— [gjb711/fastmem-mysql97](https://github.com/gjb711/fastmem-mysql97) · `v1.0-mysql97`
- **MySQL 8.4 LTS** —— [gjb711/fastmem-mysql84](https://github.com/gjb711/fastmem-mysql84) · `v1.0-mysql84`
- **MySQL 5.7** —— [gjb711/fastmem-mysql57](https://github.com/gjb711/fastmem-mysql57) · `v1.1-mysql57`

> 本仓库专指 **MariaDB 12.3** 线。

## 为什么 FASTMEM？

| MEMORY(heap) 的问题 | FASTMEM |
|---|---|
| 一张大表锁把**每条语句**串行化 | 无表锁、无 THR_LOCK 条目 |
| 读者被写者阻塞（反之亦然） | 读者完全无锁（seqlock） |
| 并发 `UPDATE` 全表排队 | 行级读-改-写串行化，热行不挡冷行 |
| 朴素自增依赖表锁 | CAS 区间预留 —— 并发安全 |

完整设计论证、并发模型与正确性证明见 **[DESIGN.md](DESIGN.md)**。

## 特性

- 无表锁（`store_lock()` 返回空；无等待队列、无锁竞争点）
- **Seqlock 读者**：撕裂检查的行镜像复制，真实负载下几乎零重试
- **行槽写者自旋锁** + 哈希桶链自旋锁；正确的并发链插入/摘除
- **语句级读-改-写串行化**：并发 `UPDATE`/`DELETE` 永不丢更新（真机验证：8 写者热行争用下 SUM 增量精确等于语句数）
- **并发安全 `AUTO_INCREMENT`**：CAS 区间预留 → 语句间区间互斥，并行插入无重复 id
- 固定槽位：热路径零分配，整行单次 `memcpy` 替换
- 8 字节位置引用 = 槽号 + 代号（generation）→ 过期引用永不悬垂
- **纯哈希索引**（显式 `BTREE` 在 `CREATE` 即拒绝，不假装支持）
- `TRUNCATE`/clear 回收内存；支持自增计数重置
- 非事务（`HA_NO_TRANSACTIONS`），语句级语义，后写者胜

## 快速开始

```sql
INSTALL SONAME 'ha_fastmem';           -- 或用 --plugin-load-add=ha_fastmem 启动

CREATE TABLE t (
  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  v  BIGINT NOT NULL
) ENGINE=FASTMEM;

INSERT INTO t (v) VALUES (1),(2),(3);
SELECT * FROM t WHERE id = 2;          -- 无锁读
UPDATE t SET v = v + 1 WHERE id = 2;   -- 行级串行化读-改-写
```

> **成熟度提示**：v1.1 起 FASTMEM 声明 `GAMMA` 成熟度，MariaDB ≥ 11.x/12.x
> 默认 `plugin_maturity=AUTO` 即可加载。注意：**不要**对已被
> `plugin-load-add` 加载的插件再执行 `INSTALL PLUGIN`（重复注册可能崩服务器）。

与 MEMORY 引擎相同的公共契约：

- 行数上限由 `max_heap_table_size` 约束（`CREATE` 时估算）
- 数据**不持久化**：服务器重启表清空（表结构保留）
- 无事务、无外键

## 构建

### 0) 一条命令从源码安装（推荐）

本仓库是源码分发：clone 后跑一个脚本即得插件。无预编译二进制、无
GLIBC/ABI 兼容争议 —— 脚本抓取所选版本的 MariaDB 服务端源码，把
`storage/fastmem/` 放进去，用服务端自带的 cmake 构建。

```bash
git clone https://github.com/gjb711/fastmem-mariadb123.git
cd fastmem-mariadb123
./install.sh --detect              # 自动按本机 server 版本构建（推荐）
./install.sh --mariadb-version 12.3.3     # 或指定任意 12.x 版本
```

`--detect` 自动读取本机 `mariadbd --version`（含常见不在 PATH 的位置，
如 `/usr/sbin`、宝塔 `/www/server/mysql/bin/`），保证插件与服务器**精确同版**
（MariaDB 对插件有 ABI 闸门，不同小版本会用 "API version ... not supported"
或缺符号拒绝）。GitHub 不可达时自动回退 `archive.mariadb.org` 官方源码包
（自带子模块，无需 git）。

| 参数 | 含义 |
|---|---|
| `--detect` | 按本机 server 精确版本构建（推荐） |
| `--mariadb-version <v>` | 目标版本（默认 `12.3.3`）；依次试 tag `maria-<v>`/`mariadb-<v>`、分支 `<v>` |
| `--branch <name>` | 直接指定分支/tag |
| `--source-dir <dir>` | 复用已有 MariaDB 源码树（不 clone） |
| `--build-dir <dir>` | cmake 构建目录（默认 `<src>/build_fastmem`） |
| `--jobs <n>` | 并行编译任务数 |
| `--plugin-dir <dir>` | 插件安装目录（默认自动探测） |
| `--dry-run` | 只打印计划，不改动 |

**已按 MariaDB 12.3.3 双平台端到端验证**：

- **Windows（MSVC，VS2022/x64）**：哈希层适配 12.3 的 `my_hasher_st`
  API，从完整 12.3.3 源码树一次编译通过 → `ha_fastmem.dll` → 装入本机
  **MariaDB 12.3.3** 并在真机冒烟（引擎列表、建表/插入/更新/删除、
  主键 + 多行哈希二级索引扫描、8 客户端 × 200 热行更新精确 SUM 零丢失校验）。
- **Linux（GCC，Ubuntu 22.04 / 官方 `mariadb:12.3.3` Docker 镜像）**：
  同一份源码编译出 `ha_fastmem.so` → 装入官方容器 → **8/8 冒烟全过**
  （引擎列表、建表/插入/回读、更新、多行哈希二级索引扫描、删除、
  8 客户端 × 200 热行零丢失、删表），`ha_fastmem.so` 同样作为
  Release 资产发布。

### 1) 最快自检：独立并发核心测试

无需服务器、无需 cmake，只要任意 C++17 编译器：

```bash
cd standalone-test && g++ -std=c++17 -O2 -pthread -o fmtest main.cpp && ./fmtest
```

含 4 套并发测试：撕裂检测、并行写者唯一键完整性、过期引用安全、多键统计。

### 2) 作为 MariaDB 插件

把 `storage/fastmem/` 放进 MariaDB 12.3.x 源码树后配置：

```bash
cmake -S <src> -B <src>/build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DWITH_SSL=system -DWITH_WSREP=OFF -DPLUGIN_ROCKSDB=NO -DPLUGIN_MROONGA=NO ...
cmake --build <src>/build --config RelWithDebInfo --target fastmem -- -j$(nproc)
```

`CMakeLists.txt` 使用 `MYSQL_ADD_PLUGIN(fastmem ... STORAGE_ENGINE)`，由顶层
`CONFIGURE_PLUGINS()` 自动纳入。

## 验证 —— Windows MariaDB 12.3.3 真机冒烟

以下运行于本机 Windows **MariaDB 12.3.3**，构建的 `ha_fastmem.dll` 置于
`lib\plugin`（全新 datadir，端口 3399）：

| 检查项 | 结果 |
|---|---|
| `SHOW ENGINES` 列出 FASTMEM | ✅ `Lock-free in-memory tables (seqlock row images, per-slot writers)` |
| `CREATE TABLE ... ENGINE=FASTMEM` + AUTO_INCREMENT | ✅ |
| 插入 5 行 → `COUNT(*)`=5，ORDER BY 回读 | ✅ 10/20/30/40/50 |
| `UPDATE ... SET v=v+1 WHERE id=3` | ✅ 30 → 31 |
| **多行哈希二级索引扫描** `WHERE name='apple'` | ✅ 返回 2 行（index-next 路径） |
| `DELETE` + `COUNT(*)` | ✅ 4 → 3 |
| `DROP TABLE` | ✅ 干净 |
| **零丢失**：8 客户端 × 200 次 `UPDATE`（100 热行） | ✅ SUM 增量 = 1600 精确等于 8×200，无丢失更新 |

## 预编译插件

[v1.0-mariadb123 发布页](https://github.com/gjb711/fastmem-mariadb123/releases)
提供两个已实测的预编译资产（均针对 **MariaDB 12.3.3**）：

| 资产 | 平台 / 工具链 | 验证 |
|---|---|---|
| `ha_fastmem-mariadb1233.dll` | Windows x64（MSVC, VS2022） | 本机 MariaDB 12.3.3 真机冒烟 + 零丢失 |
| `ha_fastmem-mariadb1233.so`  | Linux x64（GCC, Ubuntu 22.04） | 官方 mariadb:12.3.3 容器 8/8 冒烟 + 零丢失 |

其它服务器版本请在目标机器上用 `./install.sh --detect` 按需构建。