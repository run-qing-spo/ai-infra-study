# 为什么主指标是 TTFT —— 指标选择的推理链

这份文档回答一个 COMPARE_PLAN.md 里只给了结论没给推导的问题:fs vs uring 的对比实验为什么用 TTFT 当主指标,而不是 TBT/ITL、吞吐、或者 goodput。这条推理链在设计评审和面试里都会被追问,值得单独写下来。

## 出发点:磁盘 tier 在关键路径上的位置

指标选择不是口味问题,它由被测对象在系统关键路径上的位置唯一决定。磁盘 tier 干的事只有一件:在 prefill 开始之前把前缀的 KV block 从盘上捞回 CPU/GPU。这次搬运的耗时完整落在"请求到达 → 第一个 token"这段路上,也就是 TTFT 里。

而 decode 的每一步,每一层 attention 都要扫全部历史 KV,这条路径靠 HBM 的 TB/s 级带宽撑着——哪怕一个 block 落在盘上,一步 token 就要停几毫秒,整个 batch 陪着等。所以 vLLM 的调度器维持一条不变量:**在 running batch 里跑 decode 的请求,KV 必然全部在 GPU 显存里**。显存不够时的处理不是"decode 去下层读",而是把整个请求抢占(preemption)出 batch——KV 或者丢掉以后重算,或者整体换出;重新调度时先把 KV 捞回来、然后才回到 batch。下层 tier 参与的时机永远是请求进出 batch 的边界,代价表现为排队延迟和更小的并发批次,不是变慢的 token。

推论:fs 和 uring 的差异,机制上只可能在 TTFT 里现形。这就是主指标。

(一个诚实的细节:被抢占的请求恢复时,用户视角会看到两个 token 之间卡一下,统计上记成一次 ITL 尖峰。但机制上那是边界上的一次重新装填——相当于二次 TTFT——不是 decode 在读盘。bench 里的 revisit 是"上一轮结束、会话回来续聊",装填成本干干净净落在 TTFT 里,没有这个歧义。)

## 为什么不是 TBT / ITL

先对齐术语:TBT(time between tokens)、ITL(inter-token latency)是同一个东西——decode 阶段相邻两个 token 的间隔;TPOT(time per output token)是它在整段生成上的平均。

由上面的不变量,ITL 对磁盘 tier 的路径差异是天然盲的:decode 读的 KV 全在显存,fs 还是 uring 根本不在这条路上。ITL 在实验矩阵(E5)里出现,但身份不同——它不是在测 tier 的性能,而是在测 tier 的**副作用**:fs 的内核 memcpy 和 writeback 烧的是 EngineCore 同一台机器的 CPU,这个干扰会把 decode 的步进拖长。一条常驻的长 decode 请求就是一台连续采样的仪器,IO 高峰砸下来时它的 ITL 曲线画出干扰的形状。所以 ITL 是 E5 的干扰探针,不是主指标。

## 为什么不是吞吐

分两半。一半是实验设计使然:E2/E4 用开环固定速率发压(理由见 COMPARE_PLAN E2 节),offered load 被实验钉死之后,系统吞吐恒等于到达率,不再是有信息量的观测量——所有分化被挤进延迟分布里,这正是开环的意图。E3 的闭环里,"c 路并发的总完成时间"确实是一种批吞吐(每秒完成几个 revisit,注意不是 token 速率——revisit 请求刻意只生成极少 token,成本几乎全是 KV load + 短 prefill),但它只是延迟分布的一个投影(约等于最慢那条的 TTFT)。分布本身信息量更大:16 条请求每条慢 20%,和 15 条正常、1 条排队 16 倍,总时间可能一样,机制含义完全不同——前者是路径均匀变慢,后者是串行化排队。fs 预期的指纹恰恰是后者(p99 随并发线性涨、p50 平),聚合数会把它抹掉。

另一半是机制使然:整机吞吐的瓶颈在 GPU 计算,磁盘 tier 影响吞吐只有两条间接路径——省下的 prefill 重算把 GPU 算力还给别的请求(这是"有无 offload"的差异,A/B/C 阶梯第一轮已经证完,fs 和 uring 在这条路径上没有分化),以及 IO 路径省下的 CPU 别拖累 decode(这就是 E5,用 ITL 量)。

## 三层指标体系

矩阵里每个 E 的"判据"其实是三层结构,每层职责不同:

第一层,**端到端主指标**,回答"谁赢、赢多少":revisit TTFT 的 p50/p99(E2 要看随时间的序列而非聚合数),加 E5 的 ITL。

第二层,**机制归因指标**,回答"为什么赢":iostat 的 r/s、wMB/s、aqu-sz,meminfo 的 Cached/Dirty 曲线,pidstat 的线程 CPU。归因靠时间轴对齐——比如 fs 的 TTFT spike 和 Dirty 水位的回落对上,writeback 的归因才闭环。

第三层,**有效性哨兵**,不进报告正文但决定数据能不能要:tier 日志确认 re-churn 后前缀真的被逐出到盘(否则 E3 测的是 CPU tier 命中);iou-wrk 线程数排除 punt 和 unwritten extent 干扰(后者已由引擎 prewarm 内化,见 COMPARE_PLAN 第 1 步);revisit 期间设备 r/s 不为零(否则是 page cache 在替 fs 答题)。第一轮 e2e 的教训就是这一层缺位:139ms vs 123ms 没有哨兵护着,事后说不清它测到了什么。

## 备注:goodput

生产评测的标准收法是 goodput——单位时间内同时满足 TTFT < x 且 TBT < y 的请求数,把延迟和吞吐合成一个 SLO 视角的数。E6 的真实 trace 回放如果想升级,可以按这个口径报一版,让报告收尾更像生产评测。但判别性实验(E1~E4)刻意不用合成指标:合成会把机制信息抹掉,而这些实验要的正是"这条轴上谁在哪劣化、为什么"。

面试语境的一句话总结:指标跟着关键路径走——被测组件在哪条路径上,主指标就在哪;不在的路径上,它的指标只能当副作用探针;被实验设计钉死的量(开环下的吞吐)不是观测量;聚合数是分布的投影,归因要靠分布形状和时间序列。
