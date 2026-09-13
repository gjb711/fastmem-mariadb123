-- 8 路并发写入设置（FASTMEM + MEMORY 双表，同一存储过程模板）
-- 用法：
--   mysql -h 127.0.0.1 -P <port> -uroot -p --database=fmtest < concurrency_8way_setup.sql
--   # 然后并发执行（每个连接一个 worker）：
--   mysql ... -e "CALL fmtest.fm_work(<start_id>, 1000)"     -- target: fm_conc  (FASTMEM)
--   mysql ... -e "CALL fmtest.fm_work_mm(<start_id>, 1000)"  -- target: mm_conc  (MEMORY)
--   start_id = 1, 11, 21, ... 71 （8 个 worker 各管 10 行）
--   验证：两张表 80 行应全部 v=100（无丢失更新、无死锁），后 20 行保持 0。
USE fmtest;

-- FASTMEM 目标表
DROP TABLE IF EXISTS fmtest.fm_conc;
CREATE TABLE fmtest.fm_conc (id INT NOT NULL PRIMARY KEY, v BIGINT NOT NULL) ENGINE=FASTMEM;
INSERT INTO fmtest.fm_conc SELECT seq, 0 FROM seq_1_to_100;

-- MEMORY 目标表（同结构，对照）
DROP TABLE IF EXISTS fmtest.mm_conc;
CREATE TABLE fmtest.mm_conc (id INT NOT NULL PRIMARY KEY, v BIGINT NOT NULL) ENGINE=MEMORY;
INSERT INTO fmtest.mm_conc SELECT seq, 0 FROM seq_1_to_100;

DROP PROCEDURE IF EXISTS fmtest.fm_work;
DELIMITER //
CREATE PROCEDURE fmtest.fm_work(IN start_id INT, IN steps INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < steps DO
    UPDATE fmtest.fm_conc SET v = v + 1 WHERE id = start_id + (i % 10);
    SET i = i + 1;
  END WHILE;
END//

CREATE PROCEDURE fmtest.fm_work_mm(IN start_id INT, IN steps INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < steps DO
    UPDATE mm_conc SET v = v + 1 WHERE id = start_id + (i % 10);
    SET i = i + 1;
  END WHILE;
END//
DELIMITER ;
SELECT 'both engines ready' AS status;