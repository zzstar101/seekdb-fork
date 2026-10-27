# FTS 优化与实测记录

基于提交 [9f846938b309a3e5320a61f71cb84ef8d375185a](https://github.com/zzstar101/seekdb/commit/9f846938b309a3e5320a61f71cb84ef8d375185a)。
本次在真实 seekdb 上完成构建、CPU 采样、代码优化、回归和完整性能对照。
最终保留 7 个生产源文件的修改。三轮完整对照的三类等权综合耗时下降为 **6.02%**。
这是相对于该提交的本机改善，不是新的官方成绩；用户提供的原始 FTS raw score 为 66.737/100。

## 1. 最终对照结果

每个版本完整运行三次，顺序为 A1、B1、B2、A2、A3、B3；A 为原提交，B 为最终优化版本。
每轮都使用原始脚本重新导入 20,000 篇文档并构建三个全文索引。
参数固定为 BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。
下表是每项指标跨三次完整运行的算术平均值；分词、查询的每次运行内部又各采样三组。
构建指标单位为秒，其他计分指标单位为毫秒。下降率为 1 − 优化后/优化前，负值照常参与类别平均。

| 指标 | 原提交均值 | 优化版均值 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.62600 | 7.49267 | +1.75% |
| build_ik_content_sec | 5.64367 | 5.44533 | +3.51% |
| build_beng_en_sec | 3.13733 | 2.93633 | +6.41% |
| tokenize_ik_avg_ms | 0.11737 | 0.11393 | +2.93% |
| tokenize_beng_avg_ms | 0.07410 | 0.07293 | +1.57% |
| query_cn_avg_ms | 1.66373 | 1.37860 | +17.14% |
| query_beng_avg_ms | 2.39250 | 2.12857 | +11.03% |
| query_mixed_avg_ms | 1.75380 | 1.54947 | +11.65% |
| query_limit_avg_ms | 0.28223 | 0.25997 | +7.89% |

- 构建类别平均下降：3.89%。
- 分词类别平均下降：2.25%。
- 查询类别平均下降：11.93%。
- 三类等权平均下降：6.02%。

原始数据见 [baseline-1](fts-profiled-results/baseline-1.txt)、[baseline-2](fts-profiled-results/baseline-2.txt)、
[baseline-3](fts-profiled-results/baseline-3.txt)、[optimized-1](fts-profiled-results/optimized-1.txt)、
[optimized-2](fts-profiled-results/optimized-2.txt)、[optimized-3](fts-profiled-results/optimized-3.txt)。
[summary.json](fts-profiled-results/summary.json) 同时给出全部单次结果、样本标准差和计算结果。

查询的改善最明确；构建和分词的较小变化与运行波动相近，不将其解释为每次运行都能保证的收益。
本机原提交对 CI 固定基线计算时已达到评分上限，所以没有用本机的 100 分声称官方成绩提升。
正式新分数仍需统一评测环境运行。仅有原始总分，无法准确换算优化后的官方分数。

## 2. 主要修改及证据

### 恢复实际未进入的 COUNT 快速路径

原 need_count_only_text_scan 使用 !has_group_by() 排除分组。
但 ObSelectStmt::has_group_by() 的定义也包含 get_aggr_item_size() > 0，
导致普通 COUNT(*) 同样被排除，无法进入本来为它实现的 count-only 分支。

原版三个官方 COUNT 查询的执行计划已经是 calc_relevance=false、output/access/filter=nil、
is_index_back=false，CPU 采样仍出现 do_one_merge_round、cache_result、project_results。
修正后采样出现 do_one_count_round，缓存和投影分支消失。

修改显式判断 GROUP BY/ROLLUP 键，并共同约束为单基础表、唯一 MATCH 条件、自然语言模式，
无残余扫描过滤、join、分组、HAVING、窗口、锁、采样和其他功能性查找。
已标记的 count-only 子查询也重新检查常量输出等不变量。
需要文档值、评分或额外过滤的查询继续使用原执行路径。
真实 posting 访问、文档去重、事务可见性、limit 和 EOF 处理仍沿用原实现。

### 无评分查询只投影需要的倒排列

在已有 need_calc_relevance_ 标志为 false 时，倒排扫描保留 document ID，
不读取未被消费的 token count 和 document length。
同时调整表达式构建对 document length 的必需性和空值处理，避免只删投影却留下初始化依赖。
评分查询保持原有列集合。索引格式和写入内容没有改变。

### 字符分类和比较的重复开销

- IK 分类对同一字符只 decode 一次，仍按原顺序使用同一组 Unicode 谓词。
  原 release 汇编中可见同一分类链的多个独立 decode 调用，编译器未消除它们。
- ObCharset::strcmpsp 对已经初始化的字符集描述符复用现有表项，保留范围、长度和空指针检查。
  未初始化和异常输入继续走原路径；仍调用同一 strnncollsp，并原样传递尾空格参数。
  没有将 general_ci 改为 binary，也没有增加永久缓存或改变字符集规则。
- BENG 仅对三个受支持的 UTF-8 collation 下已经是小写的 ASCII token 跳过重复转换。
  非 ASCII、含大写字符和 binary collation 都回退原实现。最终持久化 token 的复制仍保留。

### 其他局部改动

- 对新建或本来就是空的文档词表，跳过逐 bucket 的 reuse；非空词表仍清理上一文档。
- 同一文档归并已经结束时，先检查 curr_doc_end，避免再调用一次无副作用的 heap.empty 虚函数。

## 3. 实验取舍

曾尝试用 set_or_update 合并词表的新词插入和重复词更新，但消融中没有足够收益，最终撤回。
完整第一组候选的 BENG 短样本有退化，因此没有直接交付那组修改。
额外的每项 90,000 次 TOKENIZE 诊断同时记录墙钟和服务端 CPU，帮助区分候选开销与短样本波动。
单独撤回 ASCII 分支更慢，撤回词表更新改动略好；最后选择上述较小的修改集合，并重新独立做三轮完整对照。
[token-diagnostics.json](fts-profiled-results/token-diagnostics.json) 记录这些诊断。
诊断使用延长的重复次数，未作为正式参数下的成绩。

未采用去掉局部词表锁、改写共享字典布局、跳过相关性 collector、硬编码并行度等未经充分实测的方向。
没有修改官方数据生成器、SQL、计时逻辑、评分脚本或固定基线。

## 4. 正确性验证

原提交和最终版本均通过两个新增 mysqltest 用例。
最终生产库使用可复现的独立编译入口通过 12 项 native 测试。
具体用例和执行方法见 [回归说明](../unittest/fts/README.md)。

- 533 项 TOKENIZE 的完整 JSON 字节对比，包括顺序、首次拼写、词频、五种 parser、
  三种 UTF-8 collation、binary 回退、空值、Unicode、内嵌 NUL、重复词和交替输入。
- 完整基准查询的全部 document ID 与保留 12 位小数的评分逐字节相同；不是仅核对命中数。
- 独立预期计数和 ID、物化可观测评分对照、冷/热计划、prepared statement 换参、
  join/残余谓词、分组/多聚合、LIMIT/OFFSET、heap ID、INSERT/UPDATE/DELETE 和 rollback。
- 255/256/257/512/513 命中边界、多词重叠及重复 posting、先加载后建索引与先建索引再写入，
  事务内可见性和回滚，以及评分结果与计数的独立对照。
- 全部 1,112,064 个有效 UTF-8 Unicode scalar、全部 UTF-16 code unit 的双端序旧行为，
  非法和截断输入；所有支持 collation 下的精确比较结果、尾空格、空串和非法参数。

六次最终运行的 TOKENIZE、查询及独立回归输出都与原版参考逐字节一致。
[verification.json](fts-profiled-results/verification.json) 保存结果校验和及 CPU 计数器差值；
[native-tests.txt](fts-profiled-results/native-tests.txt) 保存实际测试输出。
另进行了独立代码复查，未发现可证明的 Blocker 或 Major 问题。

## 5. 环境及复现边界

Linux x86_64，AMD EPYC 9V74；可见 cgroup cpu.max=800000 100000、memory.max=8589934592。
seekdb 参数固定为 memory_budget=4G、cpu_count=8、log_disk_size=2G。
编译使用原仓库依赖中的 Clang 17，release/RelWithDebInfo、-O2 -g1 -DNDEBUG、unity batch 10、
关闭 ccache；两个版本使用同一 Rust 1.98.1 和相同的生成锁文件。
运行计时期间没有并行构建或 CPU 采样。

数据库实例及实际运行二进制放在独立临时目录。最终比较统一固定日志盘大小，
没有把早期自动日志盘配置的探索数据混入最终均值。
[environment.json](fts-profiled-results/environment.json) 保存资源、构建参数、锁文件和未修改 benchmark 的哈希。

报告中的 git_head/git_dirty 由脚本读取当前工作树，不能单独标识已保存的二进制。
原版从干净的指定提交先构建并保留，优化版另存；最终比较使用以下 SHA-256 固定身份：

- 原版：afae6ca3905230c68e3b17ea9b7519d78f3a66956fe8d3fee88f257c1b76c9ad
- 优化版：78ab5449987c4a8e1e51e46eeee35d457fff1179dedbc8332f506695de98a011

可见 CPU 上限是 8 个 CPU 的带宽，本次没有复现 0.5 核配额。
不能用总耗时相近反推官方一定为半核；需要 CI 的实际 quota、父层级限制和 throttling 记录。
Linux 的 cpu.max 定义为每 PERIOD 可消耗的 MAX 时间，详见
[官方 cgroup v2 文档](https://docs.kernel.org/admin-guide/cgroup-v2.html)。
本次未提交资源配置或并行度特判。
