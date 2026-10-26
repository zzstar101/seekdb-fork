# seekdb 全文索引性能优化报告

分支：`perf/fts-fresh`（基于 `origin/2026-competition`，tip `5bce9e06d`）
工作树：`.agents/worktrees/perf/fts-large-bench-optimize`

本报告中所有标注为「实测」的数字都来自本机同一台机器上的交错 A/B 对比（同一份数据、同一份
官方基准脚本），并明确标注测量仪器与轮数。标注为「未达噪声门限」的数字表示差异小于该指标的
噪声地板，不作为收益声明。

---

## 0. 关于「官方得分 100/100」的诚实说明（必读）

官方评分脚本 `tools/benchmark/fts_large_bench_score.py` 用仓库内固定的
`fts_large_bench_baseline.json`（VLDB CI 5 次运行的中位数）作为基线。**在本机上，未做任何修改的
pristine 代码就已经得到 100.00/100**：

| 指标 | CI 基线 | 本机 pristine 实测 | pristine 对 CI 基线的提升 |
|---|---|---|---|
| build_ik_all_sec | 35.2836 | 4.193 | 88.1% |
| build_ik_content_sec | 28.3764 | 3.059 | 89.2% |
| build_beng_en_sec | 14.7578 | 1.644 | 88.9% |
| tokenize_ik_avg_ms | 0.76478 | 0.1353 | 82.3% |
| tokenize_beng_avg_ms | 0.42262 | 0.0899 | 78.7% |
| query_cn_avg_ms | 16.66282 | 3.0997 | 81.4% |
| query_beng_avg_ms | 24.30424 | 2.7224 | 88.8% |
| query_mixed_avg_ms | 17.55928 | 1.9896 | 88.7% |
| query_limit_avg_ms | 16.23338 | 2.1836 | 86.5% |

也就是说：CI 基线是在比本机慢约 5–8 倍的机器上采集的（同一台机器的数据库实例上，
`select1_avg_ms` 在 CI 是 0.2103 ms，本机是 0.057 ms）。因此「在本机跑官方脚本」得到的
100/100 **只能反映硬件差异，不能反映本次优化的效果**，把这种结果当作成绩是不诚实的。

所以本报告采用的口径是：**以本机 pristine 二进制（同机、同数据、同脚本、同参数）为基线，
用交错 A/B 测量真实的内核收益**，并把「若这些相对收益同样出现在 CI 硬件上」时的官方得分作为
**预测值**单独列出，与实测值严格区分。

---

## 1. 基准与评分规则（不变）

- 参数：`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`
- 九项指标分三类，每类先取算术平均，再对三类等权平均：

```
improvement_i = (baseline_i - optimized_i) / baseline_i
overall = (build_avg + tokenize_avg + query_avg) / 3
score   = 100 * min(max(overall, 0), 0.5) / 0.5
```

- 未修改任何官方脚本、生成器、SQL、基线、评分公式或计时逻辑（`git diff --stat origin/2026-competition -- tools/ .github/` 为空）。

---

## 2. 测量方法学（为什么必须这样测）

这台机器是共享开发机（10 核 / 24 GiB），测量期间常有其它会话在并行编译（load average 25–62），
同一份二进制的单次官方基准结果波动极大（实测同一二进制 5 次运行）：

| 指标 | 单次运行波动范围 | 最大/最小 |
|---|---|---|
| build_ik_all_sec | 4.19 – 7.83 s | 1.87× |
| build_ik_content_sec | 3.06 – 8.13 s | 2.66× |
| build_beng_en_sec | 1.64 – 3.41 s | 2.07× |
| tokenize_ik_avg_ms | 0.135 – 0.262 | 1.94× |
| tokenize_beng_avg_ms | 0.080 – 0.115 | 1.43× |
| query_cn_avg_ms | 1.553 – 3.100 | 2.00× |
| query_beng_avg_ms | 2.703 – 4.616 | 1.71× |
| query_mixed_avg_ms | 1.801 – 3.076 | 1.71× |
| query_limit_avg_ms | 2.151 – 3.442 | 1.60× |

结论：**wall-clock 单次测量无法分辨 20% 的改进**（墙钟噪声地板 19%–62%）。因此主测量仪器是
**服务端进程 CPU 时间/语句**（`ps -p <pid> -o cputime=` 在固定 reps 的同一 mysql 会话前后取差），
它与墙钟相关性高但噪声小一个数量级：

| 指标 | CPU 仪器 CV（load 19–41，6 次采样） | 墙钟 CV |
|---|---|---|
| query_cn | 3.3% | 39.3% |
| query_mixed | 3.3% | 33.4% |
| tokenize_ik | 9.3% | 38.4% |

测量协议（`/tmp/ftsbench/PROTOCOL.md`）：交错 A/B（每轮交替 A→B / B→A，消除第二臂的缓存优势）、
每臂多次采样取中位数、成对符号检验 + Wilcoxon、以及每指标预先标定的噪声地板；只有超过噪声地板
且符号一致的差异才被当作收益。构建类别用官方脚本 `--mode full`（wall）测量并标注"未达噪声门限"。

---

## 3. 瓶颈分析（Profiling 证据）

工具：macOS `sample(1)` + 自制符号化（`nm -n` + slide 计算 + `c++filt`）/ `prof.sh` / `obsym.py`。

### 3.1 查询链路

`q_cn`（`SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('全文索引 分词器')`，
8001 命中，pristine 约 2.9–3.1 ms CPU/语句）的自时间排名：

```
ObSRDaaTIterImpl::fill_merge_heap            32.6% incl
ObSRDaaTIterImpl::do_one_merge_round         51.9% incl
ObStringDiffDecoder::batch_decode             7.6%
ObNullSafeDatumStrCmp (ObCharset::strcmpsp)   5.8%
ObSRDaaTIterImpl::filter_on_demand            5.5%
ObDocIdExt::from_datum                        3.5%
ObExprBM25::eval_batch_bm25_relevance_expr    2.2%
```

即：**每次比较/每个 doc-id 的通用管线占绝对主导，BM25 表达式本身只占 2%**。逐条计数：
每个输出文档要经历 3–6 次虚调用、一次堆 push+rebuild、一次 `ObEvalCtx::BatchInfoScopeGuard` +
过滤器表达式求值、以及 2–3 次 doc-id 深拷贝（`ObDocIdExt::from_datum`）。

另外，plan 层把相关性计算写死为 `need_calc_relevance = true`
（`src/sql/optimizer/ob_log_plan.cpp:12186`，而 `ObTransformUtils::check_need_calc_match_score(...)`
的调用被注释掉在 `ob_log_plan.cpp:12254-12263`），因此 `WHERE MATCH(...)` 这种只用布尔语义的
查询也要付 BM25 + 两次附加扫描（倒排索引 agg 扫描求 df、前向表 doc_agg 扫描求 avg doc length）。

### 3.2 分词 / 索引构建链路

```
ObFTParseHelper::segment                 48.3% incl (tokenize) / 50.6% (build)
ObIKFTParser::get_next_token             47.6% / 50.3%
ObIKCJKProcessor::do_process             13.7% / 20.5%
ObFTRangeDict::match                     12.2% / 19.1%
ObAddWord::process_word                  10.5% / 11.4%
TCRWLock::wrlock (per tokenize call)      5.1% self
```

每次 TOKENIZE 调用的自测分解（IK 全文约 110 µs）：逐字符分词 37.6 µs、仲裁器 7.7 µs、
`ObAddWord` 20 µs（其中 `groupby_word` 9.4 µs = 每 token 两次哈希查找）、
parse_param 14.2 µs（含每次调用 `dict_load_cache` 4.7 µs 空闲 / 最多 254 µs 带负载、
以及 JSON parser 属性重排）、parser init 6 µs（每次重建 3 个 range dict）。

索引构建：一次 `ALTER TABLE ... ADD FULLTEXT` 的 92–98% 时间花在两个并发的排序
`INSERT ... SELECT` 辅助表构建上（`_fts_doc_word` 与倒排索引辅助表），两者**各自**对同一批文档
做一次分词（`ObDASDomainUtils::generate_fulltext_word_rows` → `ObFTParseHelper::segment`）；
`parallel(1)` 意味着一个 slice，实测只有 2 个 PX worker 线程满载，
planner 侧 `ObGranuleUtil`（`src/sql/engine/px/ob_granule_util.cpp:214-232`）按 tablet 大小/`px_task_size`
决定 granule 数，而该 tablet 刚写入（主要在内存表）⇒ granule 数只有 1–2，`dop` 只买到写侧切片。

---

## 4. 已实现的优化（按提交）

（详见第 6 节提交列表；此处列出机制、证据与实测收益）

### 4.1 只做布尔过滤的 MATCH 不再计算相关性（`perf(query): skip the relevance score for boolean only match filters`）

- 机制：在 `ObLogPlan::prepare_text_retrieval_scan` 中判定 match 表达式的**分数不可观测**
  （SELECT、无 LIMIT、无 ORDER BY、无子查询、match 表达式未被其它子句引用、自然语言模式、
  且不是 index-back 扫描），据此置 `need_calc_relevance_ = false` 并丢弃 pushdown match filter；
  判定逻辑在 `ObTransformUtils::check_need_calc_match_score` 中，保守地默认"需要分数"。
- 连带关掉：倒排索引 agg 扫描（df）、前向表 doc_agg 扫描（avg doc length）、逐行 BM25
  batch eval、逐行过滤器求值（`ObLogTableScan` 的 filter 改为在执行器侧求值）。
- 存储/DAS 侧契约：倒排索引的辅助 children 只在相关性存在时生成，因此
  `ObDASLegacyTRMergeIter::init_daat_iter_param/init_taat_iter_param/init_das_iter_scan_params`、
  `ObDASIterUtils::create_text_retrieval_sub_tree`、`ObSRDaaTIterImpl::need_score_`、
  `ObTextTaaTIter::pre_process` 全部按 `need_calc_relevance()` 分支（含 TAAT `partition_cnt_` 回退）。
- 实测（交错 CPU A/B，2 轮 × 400 reps，min-of-3）：query_cn −25.8%，query_beng −30.6%，
  query_mixed −29.9%，query_limit −0.6%（该项需要分数，不受影响）。
- 正确性：`mysem.sh` 34 项差分（同一数据上 `MATCH(...)` 与 `MATCH(...) > 0` 对比）33 通过，
  唯一差异是 BM25 同分并列导致的 `ORDER BY score LIMIT 5` 顺序（2000 条文档分数完全相同，
  顺序本就未定义）；`semcheck.sh` 1594 项 SEMANTIC OK；命中数 8001/11000/7332/20 不变。

### 4.2 doc-id 比较走整数/二进制快路径（query-dev）

`ObSRMergeCmp::cmp()` 按 id 类型分派：`ObUInt64Type` 整数比较，`CS_TYPE_BINARY` 走
`memcmp`+长度（与 `ob_strnncoll_binary` 语义一致），其它元信息/含 NULL 时回退通用
`null_first_cmp_`。堆比较从每次 5–6 次 collation-aware 字符串比较降为整数比较。

### 4.3 索引构建并行度（build-dev）

`ObFtsIndexBuilderUtil::calc_fts_build_parallelism()`：DDL 并行度为串行默认值时，按机器 CPU 数
推导 dop（四个辅助表任务继承同一度），使两个重的 `INSERT ... SELECT` 真正并行；
`semcheck.sh` SEMANTIC OK，110k 行可观测导出仅在 avg doc length 为 skip-index 估算时
有 ≤2e-4 相对偏差（同布局重建字节一致）。

### 4.4 修复：index-back 的全文扫描不能用「无分数」计划

调试过程中发现的真实约束（同一提交内修复）：只要扫描需要回表（`is_index_back=true`，例如
`SELECT id ... LIMIT 20` 或 `SELECT SUM(LENGTH(title))`），就必须保留相关性，因为回表路径只
按"带相关性的 children 数组"实现。修复方式是在 `prepare_text_retrieval_scan` 里先判
`table_scan->get_index_back()`，为真则不加优化。同时把
`ObLogTableScan::get_nonpushdown_filter_exprs` 中对全文扫描的分支恢复为无条件（过滤器一律在执行
器侧求值，与优化前完全一致）。证据：修复前 `MATCH(...) AND category=2` 返回
`ERROR 4016 (HY000) Internal error`，修复后 1333；`SUM(LENGTH(title))` 形态 263975；
`mysem.sh` 从 31/34 提升到 33/34（剩余 1 项为同分并列）。

### 4.5 分词器（tokenize-dev）

- 停用词检查 ASCII 快路径（去掉每 token 的临时 arena + 字符集转换）：BENG 停用词阶段 4.9 µs → 1.2 µs；
- casedown 复用 scratch buffer、无变化时借用原串；
- `ObAddWord::groupby_word` 改为单次哈希更新；
- IK 处理器静态分派；`ObFTRangeDict::find_first_char_range` 逐字节比较；
- 内建 IK 词典的不可变视图 hub（(charset, collation, dict type) 为键，materialize 一次、
  读者无锁、进程生命周期内不可变、容量有界、`invalidate_views()` 作为将来 reload 的钩子），
  把原先每次调用 4.7 µs（带负载最多 254 µs，占该调用 65%）的 `dict_load_cache` 从热路径移除；
  以及词典加载器 map 读锁、哈希探测去掉取模。
- 证据：61,444 条语句 / 24,130 篇文档的 TOKENIZE 输出与 pristine **逐字节一致**；
  `semcheck.sh` 1594 项 SEMANTIC OK。

---

## 5. 性能结果（实测）

所有数字来自**冻结树**（HEAD `959947187`，二进制 `/tmp/ftsbench/bin/seekdb-final`，
build id/sha256 `2062935d62b1ccf2`）与 pristine（`/tmp/ftsbench/bin/seekdb-base-5bce9e06`，
`5bce9e06d`）在**同一时段交错**运行；每轮交替 A/B 顺序，取中位数。

### 5.1 官方基准口径（wall clock，3 轮交错，`--mode full`）

| 指标 | pristine 中位数 | 本次优化 | 提升率 | 备注 |
|---|---|---|---|---|
| build_ik_all_sec | 6.191 s | 5.293 s | **−14.5%** | wall，负载漂移大 |
| build_ik_content_sec | 5.142 s | 4.253 s | **−17.3%** | wall |
| build_beng_en_sec | 2.520 s | 2.076 s | **−17.6%** | wall |
| tokenize_ik_avg_ms | 0.2276 ms | 0.1559 ms | **−31.5%** | wall |
| tokenize_beng_avg_ms | 0.1210 ms | 0.1210 ms | ±0% | wall |
| query_cn_avg_ms | 2.5095 ms | 1.4939 ms | **−40.5%** | wall |
| query_beng_avg_ms | 2.9706 ms | 2.5047 ms | **−15.7%** | wall |
| query_mixed_avg_ms | 2.2157 ms | 1.9318 ms | **−12.8%** | wall |
| query_limit_avg_ms | 3.0494 ms | 3.1858 ms | **+4.5%** | wall |
| select1_avg_ms（对照） | 0.1166 ms | 0.0667 ms | −42.8% | **对照项也差 43% ⇒ 该轮 wall 数据受负载漂移影响** |

> 说明：5.1 是唯一一条用官方脚本原样口径（wall）跑的对比，但本机负载无法控制，对照项 select1 就漂移了 43%，
> 因此 tokenize / query 的结论一律以 5.2 的服务器 CPU 口径为准（对照项 +3.4%），构建类以 5.3 的独立三轮为准。

### 5.2 服务器 CPU 时间口径（负载鲁棒，判定 tokenize / query 的权威口径）

冻结二进制 vs pristine，3 轮交错，每轮 400 reps，取中位数（负载 6.7-9.7，机器基本空载）：

| 指标 | pristine（µs/stmt） | 本次优化 | 提升率 | 三轮配对 |
|---|---|---|---|---|
| query_cn | 2600.0 | 1933.3 | **−25.6%** | −24.2 / −29.6 / −25.6 |
| query_beng | 4033.3 | 3000.0 | **−25.6%** | −26.2 / −27.4 / −24.0 |
| query_mixed | 2866.7 | 2066.7 | **−27.9%** | −25.3 / −25.3 / −27.9 |
| query_limit | 3810.5 | 3733.3 | −2.0% | −1.2 / +0.9 / −4.3（噪声） |
| tokenize_ik | 174.0 | 148.3 | **−14.7%** | −13.8 / −20.5 / −15.7 |
| tokenize_beng | 100.9 | 93.0 | **−7.8%** | −8.8 / −2.0 / −6.1 |
| select1（对照） | 58.5 | 60.5 | +3.4% | −8.0 / +8.5 / +1.8（对照应在 0 附近） |

### 5.3 构建类别的独立交错 A/B

build-dev 独立跑了同一对二进制（`bin/seekdb-base-5bce9e06` vs `bin/seekdb-bd-final`，同一 HEAD），
`--mode full --rounds 3`，wall 口径：

| 指标 | pristine | 本次优化 | 提升率 | 三轮配对 |
|---|---|---|---|---|
| build_ik_all_sec | 7.897 s | 6.165 s | **−24.0%** | −28.6 / −24.0 / −12.7 |
| build_ik_content_sec | 6.207 s | 4.663 s | **−23.6%** | −23.6 / −16.2 / −44.4 |
| build_beng_en_sec | 3.176 s | 2.531 s | **−20.3%** | −20.3 / −19.4 / −20.7 |

（我自己的 `ab-final` 链路在更高负载下测得 −14.5% / −17.3% / −17.6%，与上表同号，量级偏小，
因为该轮 wall 的对照项 select1 本身就漂移了 43%。两条链路合起来的可信区间是 **−16% ~ −24%**。）

### 5.4 三类平均与官方得分（换算）

按官方公式 `improvement_i = (baseline_i - current_i)/baseline_i`、三类等权、>50% 封顶：

| 类别 | 取用口径 | 平均提升率 |
|---|---|---|
| build | `ab-bd-final` 三轮 wall（−24.0 / −23.6 / −20.3） | **0.226**（保守取 0.20） |
| tokenize | 冻结二进制 CPU（−14.7 / −7.8） | **0.113** |
| query | 冻结二进制 CPU（−25.6 / −25.6 / −27.9 / −2.0） | **0.203** |

- overall_improvement ≈ (0.20 + 0.113 + 0.203) / 3 ≈ **0.172**
- 官方得分 ≈ 100 × 0.172 / 0.5 ≈ **34/100**（区间 32-36，取决于构建类取 −16% 还是 −24%）

**结论（诚实版）**：本次优化拿到的是真实、可复现、语义等价的内核收益——查询 −20%（三条查询 −25% ~ −28%）、
构建 −16% ~ −24%、分词 −11%（ik −15%）；`query_limit` 与 `tokenize_beng` 基本持平。**没有达到 100/100（≥50%）**，
差距来源与下一步见第 8 节。

### 5.5 独立复核链（`ab-final-confirm`，空载窗口 load 4-10，4 轮交替顺序）

为排除单次链路的偶然性，在机器安静窗口用官方 `ab.sh --mode quick --instrument cpu --rounds 4` 重跑了一遍
（`/tmp/ftsbench/reports/ab-final-confirm.md`，arm A = `seekdb-base-5bce9e06`/4b85f82c，arm B = 冻结树构建 a1c38156）：

| 指标 | pristine CPU | 优化后 CPU | 中位 Δ | IQR | 噪声地板 | 判定 |
|---|---|---|---|---|---|---|
| select1（对照） | 43.34 µs | 41.0 µs | +1.6% | −7.0..+6.9 | ±14% | noise ✓ 对照干净 |
| tokenize_ik | 127.0 µs | 105.8 µs | **−16.1%** | −19.0..−13.6 | ±4% | inconsistent（4/4 轮为负） |
| tokenize_beng | 67.96 µs | 63.5 µs | −5.0% | −7.1..−1.8 | ±14% | noise |
| query_cn | 1961 µs | 1617 µs | **−20.6%** | −22.2..−19.1 | ±12% | inconsistent（4/4 轮为负） |
| query_beng | 3539 µs | 2367 µs | **−32.7%** | −36.9..−25.0 | ±7% | inconsistent（4/4 轮为负） |
| query_mixed | 2486 µs | 1783 µs | **−22.9%** | −29.3..−19.2 | ±6% | inconsistent（4/4 轮为负） |
| query_limit | 3143 µs | 3100 µs | −1.7% | −4.4..+0.6 | ±6% | noise |

（`inconsistent` 是 bench-dev 的判定口径：配对符号检验在 n=4 时最小 p 为 0.125，达不到 "REAL" 的显著性阈值，
但 4 轮全为负、且 IQR 完全落在负区间、MDD 远小于效应量；报告如实保留该判定而不抬高措辞。）

这一链换算：tokenize 平均 **0.106**、query 平均 **0.195**、build 仍取 **0.20**（0.16-0.24 区间的保守值）
⇒ overall ≈ **0.167**，官方折算 ≈ **33/100**，与 5.4 的 0.172/34 一致（差异在噪声内）。

## 6. 正确性验证

| 项目 | 方法 | 结果 |
|---|---|---|
| 语义差分（MATCH） | `mysem.sh <port>`：同一份数据上逐项比较 `MATCH(...)` 与 `MATCH(...) > 0` 的计数、`GROUP_CONCAT(id)` 集合、与其它谓词的合取、两个 match 表达式、阈值、`ORDER BY score`、`LIMIT 20`、布尔模式、`GROUP BY`、`HAVING`、标量子查询、`COUNT(MATCH(...))`，共 34 项 | 33/34 一致；唯一差异是 `ORDER BY score DESC LIMIT 5` 在同分并列时的顺序（数据集中 2000 篇文档 BM25 分数完全相同，顺序本无定义） |
| 语义差分（TOKENIZE） | 24,130 篇文档 × 61,444 条 TOKENIZE 语句，逐 token 与词频对比 pristine | 逐字节一致 |
| 语义差分（TOKENIZE，官方用例集） | bench-dev 的 `semcheck.sh`：40 段文本 × 8 种分词器/模式、3 种 collation、15 项报错矩阵、24 条 MATCH 查询、DML 前后、索引内容（df、倒排、score MIN/MAX）、`SHOW CREATE TABLE` | SEMANTIC OK（1594 项检查） |
| 索引构建内容 | build-dev 的 110k 行可观测导出（隐藏辅助表布局、每词 df、倒排项、score 统计、每文档 docword） | 除 avg doc length 走 skip-index 估算带来的 ≤2e-4 相对分数差异（按 dop 确定复现）外完全一致 |
| 基准命中数 | 四条官方查询的 `COUNT(*)` | 8001 / 11000 / 7332 / 20，与 pristine 相同 |
| 回表路径 | `MATCH AND category=2`、`SUM(LENGTH(title)) ... MATCH`、子查询 | 1333 / 263975 / 1333，与 pristine 相同（修复过程中曾返回 `ERROR 4016`，已定位并修复） |
| 并发 | 分词器关键词典视图在并发 TOKENIZE / 查询 / 重建索引下 | 见第 5 节最终验证记录 |

## 7. 复现方法

构建（macOS / Apple clang，RelWithDebInfo）：

```bash
cd .agents/worktrees/perf/fts-large-bench-optimize
cmake -S . -B build_fts -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_USE_LTO=OFF
ninja -C build_fts -j8 src/observer/seekdb
```

启动一台专用实例（**必须专用**，官方脚本会 drop/create 测试库）：

```bash
build_fts/src/observer/seekdb --port 2888 --rpc_port 2988 --base_dir /tmp/seekdb2888   -o memory_limit=2G -o datafile_size=1G -o datafile_maxsize=8G -o log_disk_size=2G
```

运行官方基准（参数与 CI 完全一致，脚本与评分器未做任何修改）：

```bash
LABEL=optimized ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/report.txt bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py /tmp/report.txt
```

关于基线：`tools/benchmark/fts_large_bench_baseline.json` 是在 CI 机器上采集的（本机上未改动的 pristine 代码已能对这份基线拿到 100.00/100），因此本机绝对分数没有意义；本报告全部结论来自**同一台机器、同一时段、交错 A/B** 的相对提升率，用 `tools/benchmark/fts_large_bench_score.py --baseline <pristine 实测> ` 或 `/tmp/ftsbench/ab.sh` 得到。相对提升率与硬件无关，可直接换算为官方分数。

## 8. 未完成 / 风险

0. **最大的剩余杠杆（已设计、未实现）：`LIMIT` 子查询上的 `COUNT` 重写**。
   官方 `query_limit` 语句是 `SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t`。
   语义上，`LIMIT n`（无 `OFFSET`）子查询的行数恒等于 `min(n, 匹配行数)`，**与 `ORDER BY` 无关**，
   因此这条语句对相关性分数是完全不可观测的；实测也印证了代价差异：无分数的同类计数语句（q_cn 形态）
   服务器 CPU 约 1430-1533 µs，而带 top-n 下推的 `query_limit` 为 2300-2400 µs（本轮同机实测）/ 3789-3817 µs（团队 A/B 实测）。
   若在重写阶段把 `COUNT(*) over (SELECT ... LIMIT n)` 改写成 `LEAST(COUNT(*), n)` 直接统计基表条件，
   该语句就会落到已验证的"无分数"路径上，估算 `query_limit` 可降低 **40%-60%**（查询类目 +0.10-0.15，综合 +0.03-0.05，官方约 +7-10 分）。
   需要新增一条重写规则，前置条件必须严格：外层只有一个 `COUNT(*)` 选择项、无 WHERE/GROUP/HAVING/ORDER/LIMIT/窗口/UNION/`for update`；
   内层子查询无 DISTINCT / GROUP BY / HAVING / 窗口 / ROLLUP / CONNECT BY、`LIMIT n` 为正整数常量且无 `OFFSET`、非相关子查询、非 LATERAL；
   满足时才可把内层的 from/where 合并到外层并套 `LEAST`。
   未实现的原因：这属于新增优化规则，需要处理外连接、半连接、hidden column、part expr、hint 等一整套边界，
   并需要单独的差分套件（`n > 匹配数`、`LIMIT 0`、带额外谓词、被 DISTINCT/GROUP BY 排除的形态各若干）证明改写前后结果集完全一致；
   在本次剩余预算内无法完成充分验证，故按"未实现 + 设计留档"处理，而不是带着未验证的规则交付。

1. **未达到 50%（官方 100/100）**。已实测三类相对提升：构建 0.16-0.24、分词 0.11、查询 0.20 ⇒ 综合 ≈0.17（≈34/100）。
   差距的最大两块是 `query_limit`（−2%，没有 block-max 辅助表，退化为"全量打分 + top-20 堆"）与分词的固定开销上限
   （官方 tokenize 语句里分词器只占约一半，其余是 SQL 解析/表达式框架/JSON 序列化）。
2. **query_limit 无解的部分**：`LIMIT 20` 的语义是"按相关性排序后的前 20 条"，因此不能提前终止；该路径必须计算分数。
   要显著改善它需要 block-max/WAND 所需的辅助表在 DDL 阶段真正建出来（`need_block_max_scan()` 存在，但本次基准的隐藏辅助表中没有 block-max 表），属于跨 DDL/存储/执行的改动，未在本次完成。
   **已实测并否定的替代方案**：曾把 `ObLogPlan::try_push_topn_into_text_retrieval_scan()`（`src/sql/optimizer/ob_log_plan.cpp:12742`）
   临时短路成"不把 top-n 下推到全文扫描，让执行器物化全部候选再 top-N 排序"，计划确实变成 `TOP-N SORT ← TEXT RETRIEVAL SCAN`（命中数仍 20），
   但 `query_limit` 的服务器 CPU 从 **2300 µs/语句涨到 19233 µs/语句（8.4 倍劣化）**。
   原因是这条查询是 `is_index_back=true`：不下推时 9333 个候选要逐行走表达式框架求 BM25 分再排序/回表，而下推路径只为 20 行付出这些代价。
   结论：top-n 下推必须保留，`query_limit` 只能从 WAND 主循环内部优化（`ObSRBMWIterImpl::top_k_search` 68% /
   `evaluate_pivot` 46% / 每次 pivot 跳转重填 256 条 posting 25% / micro-block decode 14%），
   而短批次重填必须连带收缩 eval frame、`ObDatumVector::mask_`、`skip_`（此前的粗暴版本因数组越界 abort 已回滚）。
   该实验（含 19233 µs 这个反例）已回滚，未进入提交。
   **第二个已实测并否定的方向（缩小 advance 后的重填批次）**：在
   `ObTextRetrievalDaaTTokenIter::get_next_row()` 里把 `advance_to()` 之后的 `get_next_batch`
   容量从 `max_batch_size_`（256）降到 16（新增 `get_refill_capacity()`，`after_advance_` 标记），
   假设"pivot 跳转只消费 1 条 posting，重填 256 条是浪费"。实测相反：`query_limit` 服务器 CPU
   从 **2300-2400 µs 涨到 3767 µs（+60%）**。
   原因是每次重填都伴随一次 `das_scan_advance` 重新定位（重写 key range + 重新做 dascan seek），
   而 BMW 主循环在一次定位后会连续消费很多条 posting，所以 256 条的批次是在摊薄 seek 成本，
   并非浪费。据此结论更新为：`query_limit` 的开销是"pivot 定位 seek 次数"本身的性质，
   要改善必须减少 seek —— 也就是需要真正的 block-max 统计（辅助表）让 WAND 能跳过整块，
   而不是调整批次大小。该实验同样已回滚，未进入提交。
3. **分词指标的固定开销**：官方 `tokenize_ik` 里 SQL 解析、表达式框架、JSON 序列化与结果集编码占大部分，分词器本身只占约一半；即使分词器再快一倍，指标也只有约 25-30% 的空间。本次分词器侧实测 ≈-10..-19%（tokenize_ik）。
4. **构建并行度的不确定性**：dop 的最优值依赖机器负载（空载时 8 最好，有外部负载时 2-5 最好，8 会因 4 个辅助任务 × 8 worker 超订而退化）。最终取保守的 `cpu_count/2`，在两种情况下都不会灾难性退化，但也没有吃到空载时的全部收益。
5. **构建的并行下限（已实现、实测、并主动回滚）**：PX 任务数由 `ObParallelRangeTaskPlanner::compute_total_task_count()`
   （参数见 `src/data_plane/api/data_plane/access/ob_parallel_range_task_planner.h`，`min_task_access_size_kb_ = px_task_size>>10`）
   与 `ObGranuleUtil::compute_task_count_each_partition()`（`src/sql/engine/px/ob_granule_util.cpp:284`，
   `total_size <= 0` 时每个分区强制只切 1 个任务）共同决定；当待扫描范围全部在内存表、没有 block 统计时
   `total_size` 为 0，任务数塌陷为 1，dop 只增加写 slice 而不增加扫描 worker。据此我实现了 granule 下限
   （`compute_total_task_count()` 在 `total_size_kb == 0` 时取 `parallelism_`；
   `compute_task_count_each_partition()` 在 `total_size <= 0` 时按分区均分任务），编译通过、
   `parallel(N)` 并行查询结果全部正确（20000 / 2000 / 4778897 / 10 / 100）。
   但**交错配对 A/B 证明它没有收益**（同一实例、轮流替换二进制、3 轮配对、负载 6.7-9.4）：
   对照组（只含已提交的 dop 改动）vs 加 granule 下限：`fti_all_ik` 配对差 −6.7%/−0.7%/+4.5%（中位 −0.7%）、
   `fti_content_ik` −0.5%/−1.7%/−2.9%（中位 −1.7%）、`fti_en_beng` −2.8%/+5.7%/+4.3%（中位 +4.3%），
   全部落在 ±6% 的噪声地板内 ⇒ 该 `INSERT..SELECT` 的关键路径不是扫描并行度。
   同一批实验里 pristine vs 加了两处并行改动的版本是 5.551/4.405/2.454 s → 4.658/4.057/2.080 s
   （−16.1%/−7.9%/−15.2%，3 轮配对全负），即构建的真实收益来自已提交的 dop 改动而非 granule。
   **该 granule 改动已 `git checkout` 回滚**，理由是：通用 PX 行为改动 + 无可测收益。
   旁证（也解释了为什么 granule 无用）：`px_task_size` 2M → 128K 把 granule 数放大 16 倍，
   构建墙钟只从 12.97/12.77 s 变成 12.39/11.88 s（约 5-9%，同样在噪声内）。
   真正的下一步是减少每个写 slice 内部的工作量——每篇文档在一次 ALTER 中被分词两次
   （`__idx_<t>_<name>` 与 `..._fts_doc_word` 各走一遍 `ObDASDomainUtils::generate_fulltext_word_rows`），
   这属于 DDL 计划结构级改动，调用链已在第 3 节记录。
   方法学教训（重要）：同一 binary 上"负载 18 时的改动前"与"负载 6 时的改动后"会被误读成 −19% 的收益，
   只有交错配对 A/B 才可信；本报告所有入库结论均来自交错配对。
6. **词典视图的失效钩子**：`ObFTDictHub::invalidate_views()` 已存在但当前没有调用点（内建词典进程生命周期内不可变）。如果将来支持在线词典更新，必须在词典重载处调用它，否则视图会读到旧词典。
7. **回表限制**：`is_index_back=true` 的全文扫描必须保留相关性（回表路径只按"带相关性的 children 数组"实现）。这是本次调试中发现的既有约束，已用"回表则不加优化"的方式保守处理。
8. **量化证据的边界**：单次运行的噪声极大（同一 binary 的 query_cn 在 1.55-3.10 ms 之间浮动，构建 1.6-8.1 s），所有结论都基于交错 A/B 的中位数与配对检验；构建类指标在负载 >25 时不可分辨，本报告对构建类的结论标注为"该负载下无法分辨"。

9. **`query_limit` 的 count-only 优化（已实现、计划层生效、实测无收益、已回滚）**：`query_limit` 的 SQL 形如
   `SELECT COUNT(*) FROM (SELECT id ... LIMIT 20) t`，而 `LIMIT n` 无 OFFSET/WITH TIES 时行数恒为
   `min(n, 匹配行数)`，与行序无关，因此外层的 `COUNT(*)` 让内层的"相关性排序 + 相关性分数"不可观测。据此实现：
   `ObSelectStmt::is_count_only_output()` 标记 + `ObTransformUtils::mark_count_only_output()`
   （挂在 `ObTransformerImpl::do_transform`，此时 `transform_heuristic_rule` 在本分支已是死代码）识别
   `count(*)` over 单个 from 子查询、并清空子查询的 order items；消费点在
   `ObLogPlan::try_push_topn_into_text_retrieval_scan()`（count-only 时不做 topn 下推）与
   `ObTransformUtils::check_need_calc_match_score()`（count-only 时允许带 LIMIT 走无相关性路径）。
   实测结果分两半：
   - **计划层完全按设计生效**：`EXPLAIN` 的估算耗时从 406225 us 降到 613 us，`sort_keys(...)` 消失，
     只保留 `limit(20)`；结果全部正确（`LIMIT 20`→20、`LIMIT 5`→5、`LIMIT 100000`→6667、不匹配词→0）。
   - **但服务端 CPU 没有下降**：co4（含该改动）`query_limit` 3000-3550 us，pristine 2950-3200 us；
     而且成本对 LIMIT 完全不敏感（LIMIT 1/20/200/100000 = 3550/3500/3800/25850 us）⇒ 下推的 `limit(20)`
     并没有让倒排扫描提前结束；同时子查询投影了 `id`，强制 `is_index_back=true`，而按第 7 条约束
     回表路径必须保留相关性 ⇒ 仍然按"打分路径"扫完全部 9333 个 posting。
   结论/下一步（要拿到 −80% 以上需要同时补两块）：(a) count-only 时把子查询的选择列改写成常量，
   从而不需要回表（`access(nil)`，`is_index_back=false`）并允许走无相关性扫描；(b) 让下推的 limit
   能真正终止 DAAT 扫描（或为回表扫描实现无相关性支持）。当前实现因**无可测收益**已 `git checkout` 回滚，
   设计、实验数据与后续步骤记录在此。

## 9. 提交记录（`perf/fts-fresh`，12 个提交，vs `origin/2026-competition`）

| 提交 | 内容 | 影响的指标 |
|---|---|---|
| `daba6e913` | ASCII 停用词不再每 token 建临时 arena + 字符集转换 | tokenize / build |
| `db71b60fa` | casedown 复用 scratch buffer，未变化时借用原串 | tokenize / build |
| `aeba8e30b` | `groupby_word` 由两次哈希探测改为一次 | tokenize / build |
| `2e159a6bd` | IK 四个 Processor 由链表遍历改为直接调用，不再逐字符重取 | tokenize / build |
| `75ee9214d` | `find_first_char_range` 按字节比较范围边界 | tokenize / build |
| `f9c0f5524` | 内建 IK 词典共享不可变视图（去每次调用的词典事务加载） | tokenize / build |
| `fb85f309e` | 词典加载器 map 已加载路径改用读锁（去全局写锁） | tokenize / build |
| `7ac307c0c` | 词典哈希探测去掉一次整除 | tokenize / build |
| `e51450bd3` | 只做布尔过滤的 MATCH 不再计算相关性（优化器 + DAS/迭代器契约） | query（cn/beng/mixed） |
| `8725eb28c` | 全文辅助表构建按机器核数推导并行度 | build |
| `57efc5dfd` | 稀疏检索迭代器跳过相关性工作 + doc-id 整数/二进制比较快路径 + `set_datum_fast` | query |
| `959947187` | 并行度常量化：`clamp(cpu_count/2, 1, 8)` | build |

每个提交单独可编译、可回滚；`tools/`、`.github/`、`unittest/` 零改动；`src/` 中无任何调试/计时开关残留。

### 附：被验证后主动回滚的尝试（记录在案，避免后续重复）

1. **去掉倒排索引扫描的 `token_cnt` / `doc_length` 访问列**（期望省微块解码）：两种变体都让**所有**无分数
   MATCH 查询在执行期返回 `ERROR 4016`，回滚；diff 留档 `/tmp/ftsbench/patch-codegen-projcut.diff`。
2. **跳过分词器 `parse_param` 的逐次配置构造**（约 13% 的 IK eval）：时间窗不足以完成"缓存 + 语义重校验 + 配对测量"，
   主动放弃，草图留档。
3. **BMW `advance_to` 后只重填 16 行而非 256 行**（针对 query_limit 的 25% 重填热点）：批式表达式框架按
   `max_batch_size_` 预分配 eval frame / `ObDatumVector::mask_` / `skip_`，短批次触发 `ERROR 4003`，回滚；
   正确做法需要同步缩小 eval frame 与 mask，属于需要完整验证的改动。
4. **`ObSRDaaTIterImpl` 专用的"少维度无分数合并"**（第一次尝试）：多维度语义错误（2 token 计数偏差、3 token 报错），
   整体回滚，改为 `need_score_` 门控的最小改动。
5. **无分数路径跳过 relevance collector**：`collect_dims_by_id` 用 `got_valid_id` 丢文档，而
   `should_match_` 来自 `minimum_should_match_`（可能 >1），blind 移除会静默改变语义，主动不做。
