-- FASTMEM vs MEMORY 并发读写基准
-- 用法: mariadb --defaults-file=... < bench_compare.sql
-- 或:  mysql -e "source bench_compare.sql"
--
-- 说明：
--   * 用 INFORMATION_SCHEMA 制造伪随机数（避免依赖存储函数）。
--   * 分别对 MEMORY 与 FASTMEM 跑同样的 DML 混合负载，打印耗时。
--   * 多线程并发请用外部驱动（见 README“压测”一节）；下面的脚本
--     演示单连接下两引擎的UPDATE/INSERT/SELECT吞吐基线。

SET @bench_time := NOW(6);
SELECT 'FASTMEM setup' AS phase;

DROP TABLE IF EXISTS fm_bench, mm_bench;

-- 两个结构完全相同的表，仅引擎不同
CREATE TABLE fm_bench (
  id   INT PRIMARY KEY,
  v    BIGINT NOT NULL,
  pad  VARCHAR(64) NOT NULL
) ENGINE=FASTMEM;

CREATE TABLE mm_bench (
  id   INT PRIMARY KEY,
  v    BIGINT NOT NULL,
  pad  VARCHAR(64) NOT NULL
) ENGINE=MEMORY;

-- 填充 N 行
SET @N := 100000;
INSERT INTO fm_bench
  SELECT seq, seq * 2, REPEAT('x', 16)
  FROM seq_1_to_100000
  WHERE seq <= @N;
INSERT INTO mm_bench SELECT * FROM fm_bench;

SELECT ROUND(TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6))/1000000, 3) AS load_seconds;

-- ---------------------------------------------------------------
-- 负载 1: 随机点 UPDATE（每次改一行，模拟“不停更新”）
-- ---------------------------------------------------------------
SET @bench_time := NOW(6);
UPDATE fm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE fm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE fm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE fm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE fm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
SET @fm_update_us := TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6));

SET @bench_time := NOW(6);
UPDATE mm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE mm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE mm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE mm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
UPDATE mm_bench SET v = v + 1 WHERE id = FLOOR(1 + RAND() * @N);
SET @mm_update_us := TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6));

-- ---------------------------------------------------------------
-- 负载 2: 随机点 SELECT（大量并发读的基线）
-- ---------------------------------------------------------------
SET @bench_time := NOW(6);
SELECT SUM(v) INTO @x FROM (
  SELECT v FROM fm_bench WHERE id IN
    (FLOOR(1+RAND()*@N), FLOOR(1+RAND()*@N), FLOOR(1+RAND()*@N),
     FLOOR(1+RAND()*@N), FLOOR(1+RAND()*@N))) t;
SET @fm_read_us := TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6));

SET @bench_time := NOW(6);
SELECT SUM(v) INTO @x FROM (
  SELECT v FROM mm_bench WHERE id IN
    (FLOOR(1+RAND()*@N), FLOOR(1+RAND()*@N), FLOOR(1+RAND()*@N),
     FLOOR(1+RAND()*@N), FLOOR(1+RAND()*@N))) t;
SET @mm_read_us := TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6));

-- ---------------------------------------------------------------
-- 负载 3: 批量 INSERT（写吞吐）
-- ---------------------------------------------------------------
SET @bench_time := NOW(6);
INSERT INTO fm_bench (id, v, pad)
  SELECT seq + @N, seq, 'x' FROM seq_1_to_100000 WHERE seq <= @N;
SET @fm_insert_us := TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6));

SET @bench_time := NOW(6);
INSERT INTO mm_bench (id, v, pad)
  SELECT seq + @N, seq, 'x' FROM seq_1_to_100000 WHERE seq <= @N;
SET @mm_insert_us := TIMESTAMPDIFF(MICROSECOND, @bench_time, NOW(6));

-- ---------------------------------------------------------------
-- 汇总
-- ---------------------------------------------------------------
SELECT 'metric' AS item, 'MEMORY (us)' AS mem, 'FASTMEM (us)' AS fm,
       'gain' AS ratio;
SELECT '5 x point UPDATE' AS item, @mm_update_us, @fm_update_us,
       ROUND(@mm_update_us / GREATEST(@fm_update_us, 1), 2);
SELECT '5 x point SELECT' AS item, @mm_read_us, @fm_read_us,
       ROUND(@mm_read_us / GREATEST(@fm_read_us, 1), 2);
SELECT '100k INSERT' AS item, @mm_insert_us, @fm_insert_us,
       ROUND(@mm_insert_us / GREATEST(@fm_insert_us, 1), 2);

DROP TABLE fm_bench, mm_bench;

-- 提示：单连接下优势不明显（表锁只有并发时才是瓶颈）。
-- 真实场景请用多线程客户端（sysbench/自定义驱动）并发打同一张表，
-- 那时 MEMORY 的整表锁会把写串行化，FASTMEM 的行级并发优势才会显现。
SELECT 'NOTE: single-connection baseline only; use concurrent clients for the real win' AS note;