# FASTMEM 并发核心独立测试

`main.cpp` 直接编译 `fm_core.h`（不依赖 MariaDB 服务器），验证核心的
并发正确性。这是 FASTMEM 的最小可验证单元：所有"行级并发"逻辑都在
这里被压力测试。

## 编译

### MSVC (Windows)

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat"
cd storage\fastmem\standalone-test
cl /nologo /W3 /std:c++17 /O2 /EHsc /MT main.cpp /Fe:fmtest.exe
fmtest.exe > result.txt
type result.txt
```

> stdout 重定向到文件是为了绕过沙箱/控制台管道对子进程 stdout 的
> 限制；直接运行也可以看到输出。

### GCC/Clang (Linux/macOS)

```sh
g++ -O2 -std=c++17 -pthread main.cpp -o fmtest
./fmtest
```

## 测试内容

| 编号 | 场景 | 验证点 |
|------|------|--------|
| A | 4 写线程 + 2 读线程，64 行持续 UPDATE/SELECT | 无撕裂读（seqlock），30M+ 写 17M+ 读 |
| B | 6 线程并发按唯一键 INSERT/UPDATE/DELETE | 键空间与影子 Map 一致（唯一性完整性） |
| C | 删除后复用槽 | 旧 generation 引用被拒绝（无悬垂） |
| D | 多键（唯一+非唯一）1000 行混合操作 | 哈希链/非唯一链记账正确 |

## 通过标准

输出 `ALL TESTS PASSED`；任一处断言失败即退出非零并指出行号。

## 与 MySQL 插件的对应关系

- `fm_*` 核心函数 = `fm_data.cc/fm_hash.cc` 的底层实现；
- 服务器插件在这些原语外包一层 `FM_INFO/FM_SHARE` 生命周期与
  handler 接口（见 `../fm_def.h`）。