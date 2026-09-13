USE fmtest;
DROP TABLE IF EXISTS fm_kline, inn_kline;
CREATE TABLE fm_kline (
  sid varchar(50) NOT NULL, symbol varchar(10) NOT NULL, period varchar(10) NOT NULL DEFAULT '60m',
  open decimal(19,4), high decimal(19,4), low decimal(19,4), close decimal(19,4),
  vol decimal(19,4) NOT NULL DEFAULT 0, amount decimal(19,4) NOT NULL DEFAULT 0,
  date varchar(20) NOT NULL DEFAULT '', time varchar(10) NOT NULL DEFAULT '',
  datetime datetime NOT NULL, update_time int unsigned DEFAULT NULL,
  PRIMARY KEY (sid), KEY idx_symbol_datetime (symbol, datetime), KEY idx_datetime (datetime)
) ENGINE=FASTMEM;
CREATE TABLE inn_kline LIKE fm_kline;
ALTER TABLE inn_kline ENGINE=InnoDB;
INSERT INTO fm_kline SELECT * FROM kline_stage LIMIT 7000;
INSERT INTO inn_kline SELECT * FROM kline_stage LIMIT 7000;
SELECT 'reset done', (SELECT COUNT(*) FROM fm_kline), (SELECT COUNT(*) FROM inn_kline);