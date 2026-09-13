# FASTMEM 部署与冒烟测试脚本（需管理员权限运行）
# 用法：右键"以管理员身份运行 PowerShell"，然后执行本脚本
#     powershell -ExecutionPolicy Bypass -File deploy_admin.ps1

$ErrorActionPreference = 'Stop'
$dll       = 'D:\server-mariadb-12.1.1\build\storage\fastmem\RelWithDebInfo\ha_fastmem.dll'
$pluginDir = 'D:\Program Files\MariaDB 12.1\lib\plugin'
$mysql     = 'D:\Program Files\MariaDB 12.1\bin\mysql.exe'

if (-not (Test-Path $dll))  { Write-Error "找不到 DLL: $dll" }
if (-not (Test-Path $mysql)){ Write-Error "找不到 mysql 客户端: $mysql" }

Write-Host '[1/5] 拷贝插件 DLL ...'
Copy-Item $dll "$pluginDir\ha_fastmem.dll" -Force
Write-Host "      已安装: $pluginDir\ha_fastmem.dll"

Write-Host '[2/5] 启动 MariaDB 服务 ...'
if ((Get-Service -Name MariaDB).Status -ne 'Running') {
  Start-Service MariaDB
  Start-Sleep -Seconds 2
}
Write-Host "      服务状态: $((Get-Service -Name MariaDB).Status)"

function Exec-Sql([string]$sql) {
  & $mysql -uroot --batch --skip-column-names -e $sql
  if ($LASTEXITCODE -ne 0) { throw "SQL 失败: $sql" }
}

Write-Host '[3/5] 安装引擎插件 ...'
Exec-Sql "INSTALL SONAME 'ha_fastmem'"
Exec-Sql "SHOW ENGINES" | Select-String -Pattern 'FASTMEM' | Write-Host

Write-Host '[4/5] 建表 + 冒烟测试 ...'
& $mysql -uroot -e "
DROP TABLE IF EXISTS test.fm_smoke;
CREATE TABLE test.fm_smoke (
  id INT NOT NULL,
  v  BIGINT NOT NULL,
  note VARCHAR(64) DEFAULT NULL,
  PRIMARY KEY (id),
  KEY k_v (v)
) ENGINE=FASTMEM;
INSERT INTO test.fm_smoke VALUES (1,100,'a'),(2,200,'b'),(3,300,'c');
INSERT INTO test.fm_smoke SELECT id+100, v*10, note FROM test.fm_smoke;
UPDATE test.fm_smoke SET v = v + 5 WHERE id IN (1,3,101);
DELETE FROM test.fm_smoke WHERE id = 2;
SELECT COUNT(*) AS rows_ok FROM test.fm_smoke;
SELECT MAX(v) AS max_v FROM test.fm_smoke;
SELECT COUNT(*) AS pk_lookup FROM test.fm_smoke WHERE id = 101;
SELECT COUNT(*) AS idx_lookup FROM test.fm_smoke WHERE v = 1005;
"
if ($LASTEXITCODE -ne 0) { throw '冒烟测试失败' }

Write-Host '[5/5] 并发验证（可选，约 10 秒）...'
& $mysql -uroot -e "
DROP PROCEDURE IF EXISTS test.fm_conc;
DELIMITER //
CREATE PROCEDURE test.fm_conc()
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < 8000 DO
    INSERT INTO test.fm_smoke (id, v) VALUES (10000 + i, i) ON DUPLICATE KEY UPDATE v = v + 1;
    SET i = i + 1;
  END WHILE;
END//
DELIMITER ;
CALL test.fm_conc();
SELECT COUNT(*) AS after_conc FROM test.fm_smoke;
"

Write-Host ''
Write-Host '=== 部署完成。可运行基准测试：==='
Write-Host "mysql -uroot < D:\maria-src\storage\fastmem\bench\bench_compare.sql"
Write-Host "（对比 FASTMEM 与 MEMORY 的读写吞吐）"