# fs vs uring tier 定制对比计划

这份计划要回答的问题是:什么条件下 uring tier 优于官方 fs tier、优势多大、机制上为什么。

出发点是一个方法论判断:不需要、也不可能枚举上游业务场景。prompt 的到达率、长短、会话结构千变万化,但它们穿过 `SecondaryTierManager` 接口之后,全部被投影成一条 block 粒度的 IO 流,自由度只剩几个——读写比、并发深度、工作集大小、复用局部性、持续时长。两个业务场景只要投影出来的 IO 流一样,对 tier 来说就是同一个场景。而 fs 和 uring 的机制差异就三条:fs 走 buffered IO(读写经过内核 page cache),uring 走 O_DIRECT(绕过 page cache 直达设备);fs 的提交路径是同步 syscall,uring 是单线程批量提交;fs 的数据要经过内核缓冲区多一次 memcpy(耗 CPU),uring 的路径 DMA 直达(省 CPU)。两种 tier 只会在这三条差异被激活的维度上分化,所以测试就是逐个激活这些维度,最后用一条真实 trace 标定"真实负载落在这些维度的什么位置"。

还有一个必须先面对的事实:现有一轮结果(results_e2e_20260711_0058 的 revisit 139ms vs 123ms)**还不能用**。三个混杂变量都没控制——16 会话约 7GB 的工作集大概率整个躺在 page cache 里,fs 的"读盘"其实是在读 RAM;revisit 是串行发的,任一时刻 tier 里只有一个 load job,io_uring 批量提交的优势根本没被激活;churn 只写几 GB、一分钟跑完,压不出 buffered 写的 writeback 风暴。下面的实验设计,每一个都对应拆掉其中一个混杂变量。

## 第 0 步:摸清对手,校准环境

对比实验的预期必须建立在读过对方实现之上,不能靠猜。第一件事是在 GPU 机上读 vLLM 0.24 里 `type: "fs"` 对应的 backend 源码(在 `vllm/v1/kv_offload/` 下面),确认三个问题:它的读写是 buffered 还是 O_DIRECT;提交是在 scheduler 线程里同步做,还是丢给线程池;文件布局是每 block 一个文件还是聚合大文件。这三个答案直接决定后面每个实验的预期怎么写——如果它其实用了线程池或者 O_DIRECT,E2、E3 的预期就要改。如果是每 block 一个文件,那 open/close/unlink 的元数据开销还是一条额外的对比轴。把答案记回本文档这一节。

环境侧要校准四个数。`free -g` 看宿主总内存和空闲内存,它决定 E1 的"冷热"怎么构造;`df -T /root/autodl-tmp` 确认文件系统和剩余空间(上一轮只剩 35G,这是全计划最紧的约束);`uname -r` 加 `cat /proc/mdstat` 确认有没有 md RAID 导致 io_uring punt 的风险(背景见 BENCH_ANALYSIS §5/§12);从 serve 的 `/metrics` 读 `kv_bytes_per_offloaded_block`,把单会话前缀的 KV 字节数算准,替换本文档里"约 0.45GB/会话"的估算。

最后验证内存挤压手段。E1 需要把空闲内存压到小于工作集,但 AutoDL 是容器,`drop_caches` 和 cgroup 大概率没有权限,所以预案是 ballast 进程:写一个 `bench/ram_ballast.py`,mmap 并写满 N GiB 匿名内存抱住不放,跑起来后用 `free -g` 确认 available 真的降到了目标值。如果容器本身内存给得小(比如不到 32G),"冷"条件可能天然成立,反而是"热"条件要靠减少 sessions 缩小工作集来构造——这就是为什么 `free -g` 要放在第 0 步。

**第 0 步的答案(2026-07-12 摸底)。** 内存:`free` 显示宿主 1TB,不可信;容器是 cgroup v2(`/sys/fs/cgroup/memory/` 不存在,配额 120GiB 来自 `memory.max`),page cache 记账在自己 cgroup 头上(谁读的文件算谁的),所以冷热判据全部对 cgroup 算。看 `memory.stat` 曲线时注意两条记账规则:真正的 page cache = `file − shmem`(file 包含 shmem);ballast 的 `mmap(-1)` 是 MAP_SHARED,占用记进 shmem 而非 anon。E1 实测据此闭环:热组 pagecache 全程钉在 15.5G(HF 模型文件)纹丝不动、冷组 ≈0,`file_dirty` 四组全程为零——分别证实 fs 的读写不经过 page cache、以及冷条件压制严格成立。冷条件的构造由此改成自适应:ballast 在 serve 就绪后启动,按 `配额 − 匿名内存 − shmem ≤ CACHE_ROOM_GIB(默认 4G,小于约 7G 工作集)` 填充,不需要预估 serve 常驻——固定压舱量的原方案作废。盘:`/root/autodl-tmp` 是 xfs,删掉上一轮遗留的 20G backing 文件后约 35G 可用,与本计划的假设一致。punt 风险:落实了——内核 5.15.0-97,数据盘是 md0(双 NVMe RAID1),5.15 的 md 不支持 NOWAIT(5.17 才加),所以 uring tier 的所有 O_DIRECT IO 全程被 punt 到 io-wq,prewarm 消不掉它、也不需要消(extent 转换的元数据成本照样省)。解读框架相应调整:iou-wrk 非零是这台宿主的常态基线,不再是 extent punt 警报;本轮测出的 uring 是"退化成内核侧线程池"的下界,微基准复测已证这个下界照样赢 pool 引擎。prewarm 冒烟通过:256MiB 构造 0.07s,filefrag 确认 unwritten 标志全消,文件全零,首笔 store 成功。`kv_bytes_per_offloaded_block` 不用读 /metrics 了:E1 的 kv_fs 落盘统计给出 15574 文件 / 13627MiB ≈ 0.875MiB/block = 16 token × 56KB(Qwen2.5-7B 每 token KV),即 vLLM 默认 block_size;每会话前缀 ≈ 8k token ≈ 0.44GB,工作集 ≈ 7GB,与估算一致。

**fs backend 三问的答案(v0.24.0 源码, `vllm/v1/kv_offload/tiering/fs/`)——本计划的核心前提被推翻。** 一,读写都是 **O_DIRECT**(io.py:写 `O_CREAT|O_EXCL|O_WRONLY|O_TRUNC|O_DIRECT` 走临时文件+rename 原子替换,读 `O_RDONLY|O_DIRECT` 用 readv),不是 buffered——"fs 走 page cache"的假设错了,E1 实测热组 fs 照样从设备读满 4.3GB 与 uring 完全相同。二,提交是线程池:DualQueueThreadPool,默认 **16 读线程 + 16 写线程**,submit 不阻塞调用方,读写双队列互为 fallback。三,**每 block 一个文件**,按 hash 三层子目录布局。由此三条对比轴重写:page cache 轴消失(双方都 O_DIRECT);"多一次 memcpy"轴消失(同上);真正剩下的分化是 (a) 文件布局——每 block 一次 open/O_DIRECT read/close + store 侧 tmp+rename 的元数据事务,对阵单一 slab 文件纯 offset 寻址,(b) 并发模型——32 条用户态阻塞线程对阵单提交线程+io-wq(本宿主 md punt 下实测 ~8 个内核 worker)。E2 的 writeback 风暴预期随之作废(O_DIRECT 写不积脏页),E3 的"fs 同步 syscall 一次一发"预期也要改(fs 有 16 深度的读并发),两节需要重写预期后再跑。

## 第 1 步:bench 工具改造

工具改造在 Mac 上就能写完,上 GPU 机只做验证。`bench/long_context_ttft.py` 加五个能力,全部走新参数、不动现有默认行为。第一,`--revisit-concurrency N`,revisit 阶段用线程池并发发请求,这是 E3 的核心开关(prime 和 churn 不需要并发)。第二,`--phases`,允许指定跑哪些 phase、允许 churn→revisit 循环多轮——E3 扫并发档时靠它在同一个 serve 里复用:prime 一次,之后每档并发前重新 churn 把前缀挤回盘,省掉每档 70 秒的 serve 重启。第三,每个样本记录绝对时间戳(现在 JSON 里只有 ttft 时长),E2 的时间序列图要和 iostat/pidstat 按时间对齐,全靠它。第四,`--mixed`,churn 流量持续不断、期间按固定间隔插入 revisit 请求,两类样本分开统计,这是 E4 的负载形状。第五,`--churn-rate R`,churn 阶段改为开环发压:每隔 1/R 秒发出一条、不等前一条返回,这是 E2 的速率控制(理由见 E2 节);不给该参数时保持现有闭环行为。

引擎侧另有一项(已做):uring 引擎和 pool 对照引擎的构造加 `prewarm` 开关,启动时把 backing 文件真写一遍零,把 fallocate 的 unwritten extent 全部翻成 written——这是 BENCH_ANALYSIS §3 那次 dd 预写诊断实验的代码内化。不做的话,serve 起来后 prime/churn 对 slab 的首写全部撞 extent 转换被 punt,E2 的时间序列前段会混进一层与 writeback 无关的噪声,E3 的 iou-wrk 哨兵也会误报。manager 侧默认开(约 9 秒启动开销),pybind/引擎默认关,微基准的既有预写协议不变。上 GPU 机后需要重新编译 .so 才生效。

`bench/run_e2e_overnight.sh` 相应改三处:把写死的 A/B/C1/C2 四组参数化成实验矩阵(tier 类型 × ballast 大小 × bench 参数),每个实验一行 spec,复用现有的 run_group 骨架和监控;监控加一路 `/proc/meminfo` 采样(Cached、Dirty、Available,5 秒一次),E1 看 page cache 增长、E2 看 Dirty 水位都靠它;C1 跑完删 `kv_fs` 之前,先统计它占的磁盘字节数和文件数——fs tier 没有容量上限配置,35G 的盘必须盯着。

## 第 2 步:实验矩阵

### E1 工作集 vs page cache —— 最优先,它决定现有数据能不能要

fs 是 buffered 读,只要数据还在 page cache 里,"读盘"就是读 RAM;uring 是 O_DIRECT,永远真读设备。所以第一个要控制的变量就是"盘上的数据在不在 page cache 里"。做法是 2×2 共四次 serve:fs/uring 两种 tier,各跑"热"(不压内存,空闲内存远大于工作集)和"冷"(ballast 把空闲内存压到小于工作集)两种条件,负载沿用现版的 prime→churn→revisit 串行。

判据有三层。fs 热 vs fs 冷:预期热的 revisit 接近 RAM 速度,而且 iostat 里 revisit 期间设备 r/s 约等于零——这就是"上一轮 139ms 根本没读盘"的实锤;冷的会掉到真实盘速并伴随 cache 抖动。uring 热 vs 冷:预期两者一致,O_DIRECT 不吃 page cache,这份稳定性本身就是结论。第三层看 meminfo 的 Cached 曲线:fs 组一路上涨——同一份 KV 在 CPU tier 和 page cache 里存了两份,这个内存双份开销是独立论点;uring 组应该是平的。

生产语境的解读要提前想好:推理宿主机的空闲内存本来就被模型权重、CPU tier 的 pinned memory 吃掉,"冷"才是真实区间。"热"区间 fs 赢是 page cache 的功劳,但代价是那份内存本可以显式加给 CPU tier——显式加 CPU tier 永远优于让 page cache 隐式缓存,因为前者命中时连盘路径的序列化开销都省了。

### E2 持续写压力下的尾延迟

buffered 写看起来是瞬间完成的——数据丢进脏页就返回——直到脏页水位触发内核 writeback,回写风暴和整机争 CPU、争内存带宽、争设备队列。O_DIRECT 写每笔都付真实代价,但节奏平稳。现版 churn 只有 24 个请求一分钟跑完,根本压不出这个现象,所以把 churn 加量到写约 15GB(受 35G 盘限制)、持续 5~10 分钟,两种 tier 各跑一次。

发压方式必须是开环固定速率(每隔固定间隔发一条,不等前一条返回),速率取设备写带宽的六七成,理由有两层。一是瞬时速率必须钉在带宽天花板之下:脏页存量是(写入速率−刷盘速率)对时间的积分,短时打满同样能触发风暴,但那时 fs 和 uring 一起顶在带宽墙上,writeback 独有的"周期性 spike vs 全程平稳"分化会被共同瓶颈遮掉——总量要从时长里来,不从速率里来。二是闭环的节奏跟着被测系统走:fs 的 buffered 写返回快(只是 memcpy 进脏页),会诱导客户端发得更快,uring 每笔付设备成本返回慢,客户端跟着慢,结果两组收到的 offered load 不一样,对比失效。开环把负载从系统手里夺回来钉死。闭环不是废弃而是归属不同:它控制的是在飞请求数,那是 E3 并发轴的工具。

判据不看聚合数,看时间序列:churn TTFT 随时间的散点,fs 预期前段平稳、中后段周期性 spike,uring 预期全程平;iostat 的 wMB/s,fs 是突发锯齿、uring 是稳定水位;再把 meminfo 的 Dirty 曲线和 spike 时刻对齐,"spike 是 writeback 造成的"这个归因就闭环了。

### E3 并发 load 深度 —— uring 设计价值的主战场

真实场景里"一批用户同时回来"是常态,对应 N 个并发 load,而这正是提交路径差异被激活的地方:同步 syscall 一次只能在飞一个 IO,io_uring 靠队列深度吃满设备。做法是同一个 serve 内循环:prime 一次,然后对并发档 c ∈ {1, 4, 8, 16}(上限受 sessions=16 限制)依次执行"churn 把前缀挤回盘 → revisit @ 并发 c"。两种 tier 各一次 serve。

判据是 revisit TTFT p50/p99 对并发数的曲线:预期 fs 随并发线性劣化(排队),uring 劣化平缓。顺带盯两个旁证:iou_wrk 日志排除 punt 干扰,iostat 的 aqu-sz 验证 uring 真把设备侧队列深度打上去了。有一个作废条件必须防住:每档并发前的 re-churn 要从 tier 日志或 metrics 确认前缀真的被挤回了盘,否则那一档量到的是 CPU tier 命中,数据作废。

### E4 读写混战 —— 最接近真实 serving 的稳态

真实 serving 里 store 和 load 同时发生:新请求持续把块沉下去,老会话同时要把块拉上来。现版的 phase 是干净分离的,最"脏"的稳态反而没测到。用 `--mixed` 跑:churn 流不停,期间每隔几秒插一个 revisit,并发档取 E3 里分化最明显的那一档。判据是 revisit TTFT 相比 E3 同档"安静盘"基线劣化了多少:预期 fs 的读被 writeback 卡出长尾,uring 读写同 ring、在设备层公平排队,劣化显著更小。

### E5 CPU 干扰面 —— 不单独跑,从 E2~E4 的监控里出

fs 每个 block 要过一次内核缓冲区拷贝,烧的是和 EngineCore 同一台机器的 CPU;uring 的 O_DIRECT 是 DMA,CPU 只管提交和收割。两个证据源:一是 pidstat 里 tier 相关线程(fs 的工作线程 vs uring 的单 C++ 线程加 iou 内核侧)的 CPU 占用,按实验分组对比;二是在 E4 的 mixed 阶段加一个常驻的长 decode 请求,量它的 ITL(inter-token latency,逐 token 间隔)在 IO 高峰期间被压高多少——这是"IO 路径省下的 CPU 还给了 decode"的端到端证据。

### E6 真实 trace 锚点 —— 收尾

从之前整理的真实 trace 清单(Mooncake / ShareGPT 多轮对话)挑一条回放,两种 tier 各跑一遍。它的作用不是探索空间——E1~E4 已经覆盖了——而是标定:真实多轮负载落在 E1 的冷/热哪个区间、E3 的哪个并发档。这样报告的推理链是完整的:轴扫描曲线说明什么条件下谁赢、为什么,真实 trace 说明真实世界落在哪个条件里,最后这一跑同时充当预测的验证。

### E7 复用局部性 —— 设计完备但不跑,省略理由如下

五个自由度里唯一没有对应实验的是复用局部性,这里把"为什么不测"写明,免得矩阵看起来有漏洞。如果要测,做法是固定工作集总量,扫前缀池配比 P×F:维护 P 条互不相同的长前缀,每条被 F 个会话共享(prompt = 共享前缀 + 每会话独有后缀),保持 P×F×前缀长度不变。P=1、F=16 是"全员共用一条 system prompt"的极端局部性——所有读砸在同一小撮 block 上;P=16、F=1 就是现版每会话独有前缀,读均匀散开。vLLM 的 prefix caching 按 block 内容哈希,前缀 token 相同即映射到同一批 KV block,构造上是可行的。预期是 fs 在高局部性端靠 page cache 对热 block 的反复命中占便宜,uring 无缓存、两端表现一致。

不跑的理由是这个轴在磁盘层是自我衰减的:高局部性意味着同一批 block 被反复访问,而被反复访问的 block 恰恰会被 GPU/CPU tier 留住,根本沉不到磁盘;真正落到磁盘 tier 的读,天然是被上层筛剩下的低局部性尾部。所以磁盘层 IO 流的局部性区间很窄,E1 冷条件(每会话独有前缀、逐出后回访)已经落在这个真实区间里,单独扫这条轴测到的高局部性端在生产中不会出现。把这段论证写在报告里,比多跑一组实验更有信息量;若结论被挑战,上面的 P×F 设计随时可以启用。

## 第 3 步:产出

每个轴一张图:E1 交叉条形、E2 时间序列、E3 并发曲线、E4 劣化对比、E5 CPU/ITL 对照,汇成 BENCH_ANALYSIS.md 的新章节或独立的 COMPARE_RESULTS.md。主指标为什么是 TTFT 而不是 TBT/吞吐/goodput、判据的三层结构(主指标/归因/哨兵),完整推导见 METRICS.md。最终给一张结论矩阵:行是轴,列是 fs 赢、打平、uring 赢的条件区间。fs 赢的区间(热 page cache 加低并发)要诚实写进去——一份有对手赢面的报告比全赢的报告可信,而且"那份内存不如显式给 CPU tier"的反驳本身就是报告的一部分。

## 风险与执行顺序

盘只剩 35G 是最紧的约束:uring backing 20G、fs 目录(无上限)、模型缓存三者共存不下,沿用"C1 跑完删 kv_fs"的串行策略,E2 加量前先 `df` 预检。样本量方面,n=16 撑不起 p99,关键实验(E1 冷、E3 高并发档)的 revisit 至少循环 3 轮取合并样本——serve 内循环比多起 serve 便宜。公平性方面,两 tier 的磁盘容量、CPU tier 大小保持一致,C1 沿用 PYTHONHASHSEED=0,关键结论换 seed 重跑一遍确认不翻。

执行顺序:先在 Mac 上完成第 1 步的工具改造;然后上 GPU 机做第 0 步摸底,按 `free -g` 的结果定 E1 的冷热构造方式;第一晚跑 E1 + E3(四次 serve 加两次 serve,时间够),第二晚跑 E2 + E4,E5 从监控数据里分析得出,E6 最后收尾。全程沿用 overnight 脚本的无人值守骨架。