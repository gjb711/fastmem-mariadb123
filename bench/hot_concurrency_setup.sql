-- 热行碰撞并发负载：8 writer 随机更新 100 个热行 + 4 reader 并发读
-- 表：fm_kline (FASTMEM), inn_kline (InnoDB)，各 7000 行，结构一致
-- 用法：mysql ... --database=fmtest < hot_concurrency_setup.sql
USE fmtest;

DROP TABLE IF EXISTS hot_sids;
CREATE TABLE hot_sids (sid varchar(50) NOT NULL PRIMARY KEY) ENGINE=MEMORY;
INSERT INTO hot_sids SELECT sid FROM fm_kline ORDER BY RAND() LIMIT 100;
SELECT COUNT(*) AS hot_rows FROM hot_sids;

DROP PROCEDURE IF EXISTS fmtest.fm_hot_work;
DELIMITER //
CREATE PROCEDURE fmtest.fm_hot_work(IN tbl varchar(64), IN steps INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  DECLARE sidv varchar(50);
  WHILE i < steps DO
    SELECT sid INTO sidv FROM hot_sids ORDER BY RAND() LIMIT 1;
    SET @s := CONCAT('UPDATE ', tbl,
                     ' SET close = COALESCE(close,0) + 1 WHERE sid = ''', sidv, '''');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    SET i = i + 1;
  END WHILE;
END//

DROP PROCEDURE IF EXISTS fmtest.fm_reader;
CREATE PROCEDURE fmtest.fm_reader(IN tbl varchar(64), IN steps INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < steps DO
    SET @r := CONCAT('SELECT COALESCE(SUM(close),0), COUNT(*) FROM ', tbl);
    PREPARE st FROM @r; EXECUTE st; DEALLOCATE PREPARE st;
    SET i = i + 1;
  END WHILE;
END//
DELIMITER ;
SELECT 'load infra ready' AS status;