# fs vs uring tier 定制对比计划

这份计划要回答的问题是:什么条件下 uring tier 优于官方 fs tier、优势多大、机制上为什么。

出发点是一个方法论判断:不需要、也不可能枚举上游业务场景。prompt 的到达率、长短、会话结构千变万化,但它们穿过 `SecondaryTierManager` 接口之后,全部被投影成一条 block 粒度的 IO 流,自由度只剩几个——读写比、并发深度、工作集大小、复用局部性、持续时长。两个业务场景只要投影出来的 IO 流一样,对 tier 来说就是同一个场景。而 fs 和 uring 的机制差异就三条:fs 走 buffered IO(读写经过内核 page cache),uring 走 O_DIRECT(绕过 page cache 直达设备);fs 的提交路径是同步 syscall,uring 是单线程批量提交;fs 的数据要经过内核缓冲区多一次 memcpy(耗 CPU),uring 的路径 DMA 直达(省 CPU)。两种 tier 只会在这三条差异被激活的维度上分化,所以测试就是逐个激活这些维度,最后用一条真实 trace 标定"真实负载落在这些维度的什么位置"。(注:第 0 步读源码推翻了这三条里的两条半——fs 实为 O_DIRECT + 16读/16写线程池 + 每 block 一文件,page cache 轴和 memcpy 轴都不存在。真正的分化轴见第 0 步答案,E2/E3/E5 已按新轴改写,原文见 git 历史。方法论本身不变:枚举的是 IO 流的自由度,不是业务场景。)

还有一个必须先面对的事实:现有一轮结果(results_e2e_20260711_0058 的 revisit 139ms vs 123ms)**还不能用**。三个混杂变量都没控制——16 会话约 7GB 的工作集大概率整个躺在 page cache 里,fs 的"读盘"其实是在读 RAM;revisit 是串行发的,任一时刻 tier 里只有一个 load job,io_uring 批量提交的优势根本没被激活;churn 只写几 GB、一分钟跑完,压不出 buffered 写的 writeback 风暴。下面的实验设计,每一个都对应拆掉其中一个混杂变量。

## 第 0 步:摸清对手,校准环境

对比实验的预期必须建立在读过对方实现之上,不能靠猜。第一件事是在 GPU 机上读 vLLM 0.24 里 `type: "fs"` 对应的 backend 源码(在 `vllm/v1/kv_offload/` 下面),确认三个问题:它的读写是 buffered 还是 O_DIRECT;提交是在 scheduler 线程里同步做,还是丢给线程池;文件布局是每 block 一个文件还是聚合大文件。这三个答案直接决定后面每个实验的预期怎么写——如果它其实用了线程池或者 O_DIRECT,E2、E3 的预期就要改。如果是每 block 一个文件,那 open/close/unlink 的元数据开销还是一条额外的对比轴。把答案记回本文档这一节。

环境侧要校准四个数。`free -g` 看宿主总内存和空闲内存,它决定 E1 的"冷热"怎么构造;`df -T /root/autodl-tmp` 确认文件系统和剩余空间(上一轮只剩 35G,这是全计划最紧的约束);`uname -r` 加 `cat /proc/mdstat` 确认有没有 md RAID 导致 io_uring punt 的风险(背景见 BENCH_ANALYSIS §5/§12);从 serve 的 `/metrics` 读 `kv_bytes_per_offloaded_block`,把单会话前缀的 KV 字节数算准,替换本文档里"约 0.45GB/会话"的估算。

最后验证内存挤压手段。E1 需要把空闲内存压到小于工作集,但 AutoDL 是容器,`drop_caches` 和 cgroup 大概率没有权限,所以预案是 ballast 进程:写一个 `bench/ram_ballast.py`,mmap 并写满 N GiB 匿名内存抱住不放,跑起来后用 `free -g` 确认 available 真的降到了目标值。如果容器本身内存给得小(比如不到 32G),"冷"条件可能天然成立,反而是"热"条件要靠减少 sessions 缩小工作集来构造——这就是为什么 `free -g` 要放在第 0 步。

**第 0 步的答案(2026-07-12 摸底)。** 内存:`free` 显示宿主 1TB,不可信;容器是 cgroup v2(`/sys/fs/cgroup/memory/` 不存在,配额 120GiB 来自 `memory.max`),page cache 记账在自己 cgroup 头上(谁读的文件算谁的),所以冷热判据全部对 cgroup 算。看 `memory.stat` 曲线时注意两条记账规则:真正的 page cache = `file − shmem`(file 包含 shmem);ballast 的 `mmap(-1)` 是 MAP_SHARED,占用记进 shmem 而非 anon。E1 实测据此闭环:热组 pagecache 全程钉在 15.5G(HF 模型文件)纹丝不动、冷组 ≈0,`file_dirty` 四组全程为零——分别证实 fs 的读写不经过 page cache、以及冷条件压制严格成立。冷条件的构造由此改成自适应:ballast 在 serve 就绪后启动,按 `配额 − 匿名内存 − shmem ≤ CACHE_ROOM_GIB(默认 4G,小于约 7G 工作集)` 填充,不需要预估 serve 常驻——固定压舱量的原方案作废。盘:`/root/autodl-tmp` 是 xfs,删掉上一轮遗留的 20G backing 文件后约 35G 可用,与本计划的假设一致。punt 风险:落实了——内核 5.15.0-97,数据盘是 md0(双 NVMe RAID1),5.15 的 md 不支持 NOWAIT(5.17 才加),所以 uring tier 的所有 O_DIRECT IO 全程被 punt 到 io-wq,prewarm 消不掉它、也不需要消(extent 转换的元数据成本照样省)。解读框架相应调整:iou-wrk 非零是这台宿主的常态基线,不再是 extent punt 警报;本轮测出的 uring 是"退化成内核侧线程池"的下界,微基准复测已证这个下界照样赢 pool 引擎。prewarm 冒烟通过:256MiB 构造 0.07s,filefrag 确认 unwritten 标志全消,文件全零,首笔 store 成功。`kv_bytes_per_offloaded_block` 不用读 /metrics 了:E1 的 kv_fs 落盘统计给出 15574 文件 / 13627MiB ≈ 0.875MiB/block = 16 token × 56KB(Qwen2.5-7B 每 token KV),即 vLLM 默认 block_size;每会话前缀 ≈ 8k token ≈ 0.44GB,工作集 ≈ 7GB,与估算一致。

**/dev/shm 泄漏事故链(2026-07-12 发现,2026-07-14 查清真因,一条修复闭环三件悬案)。** 泄漏的是 vLLM 在 `/dev/shm` 里的 offload 共享内存(`vllm_offload_*.mmap`,每次 serve 约 4.3G——正是 `cpu_bytes_to_use=4G` 的 CPU tier 落在 tmpfs 上的实体:多进程架构里 EngineCore 决策、GPU worker 搬运、secondary tier 读写同一块 CPU KV buffer,只能靠共享内存,而我们的 O_DIRECT 用户 buffer 物理上就是这些 tmpfs 页)。真因不是当初以为的"我们的 `pkill -9` 兜底杀得太狠":vLLM 的 `shutdown_timeout` 默认是 **0**(`config/vllm.py:380`),`mode = "abort" if timeout == 0 else "drain"`,于是 process manager 对 EngineCore 先 `terminate()` 再零等待 SIGKILL(`v1/utils.py:621` 的 join 循环 `deadline = now + 0`,第一轮就 `break`),EngineCore 刚打完 `starting resource teardown` 就在同一秒被打死,`TieringOffloadingManager.shutdown()` → `SharedOffloadRegion.cleanup()` 里的 `os.unlink` 永远跑不到——而那是唯一的清理路径,没有 `atexit`/`__del__` 兜底。对照实验证死了另外两个嫌疑:换信号无效(SIGTERM 单播给主进程,照样 `mode=abort timeout=0s` + `force killing count=1`);与我们的 uring 引擎无关(A 基线组、fs 组同样被 force kill)。根治是 serve 时传 `--shutdown-timeout 30` 走 drain 模式,`pkill -9` 兜底和 `rm -f` 清理都保留——SIGKILL 路径(OOM killer、真挂死)永远可能发生,用 SIGKILL 兜底的脚本必须接管被杀进程的清理义务。尸体逐 serve 累积,直到 E3 的第 5 次 serve 把 tmpfs 填满,`SharedOffloadRegion` 初始化时 `MADV_POPULATE_WRITE` 吃 EFAULT,引擎当场死掉——这才暴露。回头看,它同时解释了另外两件一直没归因的事:第 0 步摸底时空载配额里那神秘的 ~24G shmem(就是前几天被 SIGKILL 的 serve 留下的 mmap 尸体);以及 E1 三轮之间 ballast 起点 cache-room 的漂移(85G→69G→109G,随尸体堆积与人工清理起伏——注意 tmpfs 占的就是 cgroup 配额里的 shmem,直接压缩 page cache 的生存空间,好在自适应 ballast 对起点不敏感,三轮都精确收敛到 4.0G,冷条件未被污染)。修复分两层:`stop_serve` 补一刀 `rm -f /dev/shm/vllm_offload_*.mmap`(止血,2026-07-12),serve 加 `--shutdown-timeout 30` 让 vLLM 自己有时间 unlink(根治,2026-07-14)。教训三条:用 SIGKILL 兜底的自动化脚本要接管被杀进程的清理义务;tmpfs 不是"免费内存",它按 shmem 记在 cgroup 账上,泄漏等价于内存泄漏;兜底动作(那两行 `pkill -9`)必须带守卫和日志,否则"优雅关停成功"和"失败"混成一团,归因会被自己的观测盲区带偏——这次就是靠"尸体逐 serve 累积"反推出优雅关停其实 100% 失败,才回头去读 vLLM 的关停代码。

**fs backend 三问的答案(v0.24.0 源码, `vllm/v1/kv_offload/tiering/fs/`)——本计划的核心前提被推翻。** 一,读写都是 **O_DIRECT**(io.py:写 `O_CREAT|O_EXCL|O_WRONLY|O_TRUNC|O_DIRECT` 走临时文件+rename 原子替换,读 `O_RDONLY|O_DIRECT` 用 readv),不是 buffered——"fs 走 page cache"的假设错了,E1 实测热组 fs 照样从设备读满 4.3GB 与 uring 完全相同。二,提交是线程池:DualQueueThreadPool,默认 **16 读线程 + 16 写线程**,submit 不阻塞调用方,读写双队列互为 fallback。三,**每 block 一个文件**,按 hash 三层子目录布局。由此三条对比轴重写:page cache 轴消失(双方都 O_DIRECT);"多一次 memcpy"轴消失(同上);真正剩下的分化是 (a) 文件布局——每 block 一次 open/O_DIRECT read/close + store 侧 tmp+rename 的元数据事务,对阵单一 slab 文件纯 offset 寻址,(b) 并发模型——32 条用户态阻塞线程对阵单提交线程+io-wq(本宿主 md punt 下实测 ~8 个内核 worker)。E2 的 writeback 风暴预期随之作废(O_DIRECT 写不积脏页,已砍,见 E2 节),E3 的"fs 同步 syscall 一次一发"预期已按新轴重写(fs 有 16 深度的读并发,见 E3 节)。

## 第 1 步:bench 工具改造

工具改造在 Mac 上就能写完,上 GPU 机只做验证。`bench/long_context_ttft.py` 加五个能力,全部走新参数、不动现有默认行为。第一,`--revisit-concurrency N`,revisit 阶段用线程池并发发请求,这是 E3 的核心开关(prime 和 churn 不需要并发)。第二,`--phases`,允许指定跑哪些 phase、允许 churn→revisit 循环多轮——E3 扫并发档时靠它在同一个 serve 里复用:prime 一次,之后每档并发前重新 churn 把前缀挤回盘,省掉每档 70 秒的 serve 重启。第三,每个样本记录绝对时间戳(现在 JSON 里只有 ttft 时长),E2 的时间序列图要和 iostat/pidstat 按时间对齐,全靠它。第四,`--mixed`,churn 流量持续不断、期间按固定间隔插入 revisit 请求,两类样本分开统计,这是 E4 的负载形状。第五,`--churn-rate R`,churn 阶段改为开环发压:每隔 1/R 秒发出一条、不等前一条返回,这是 E2 的速率控制(理由见 E2 节);不给该参数时保持现有闭环行为。(2026-07-12 更新:E2 已砍,`--churn-rate` 取消;前三项已实装——`--revisit-concurrency`、`--phases` 带 `revisit@c` 语法和多轮循环、每样本绝对时间戳 `t_wall`,revisit 每轮自动换新问题。2026-07-14:第四项 `--mixed` 连同 E5 的常驻 decode ITL 探针一起实装完毕——`--phases` 新增 `mixed@c` token,配 `--mixed-interval`/`--mixed-waves`/`--probe-tokens`;探针逐 token 记绝对到达时刻,ITL = 相邻时刻之差。工具链全部到位,Mac 侧无 GPU 的活干完了。)

引擎侧另有一项(已做):uring 引擎和 pool 对照引擎的构造加 `prewarm` 开关,启动时把 backing 文件真写一遍零,把 fallocate 的 unwritten extent 全部翻成 written——这是 BENCH_ANALYSIS §3 那次 dd 预写诊断实验的代码内化。不做的话,serve 起来后 prime/churn 对 slab 的首写全部撞 extent 转换被 punt,E2 的时间序列前段会混进一层与 writeback 无关的噪声,E3 的 iou-wrk 哨兵也会误报。manager 侧默认开(约 9 秒启动开销),pybind/引擎默认关,微基准的既有预写协议不变。上 GPU 机后需要重新编译 .so 才生效。

`bench/run_e2e_overnight.sh` 相应改三处:把写死的 A/B/C1/C2 四组参数化成实验矩阵(tier 类型 × ballast 大小 × bench 参数),每个实验一行 spec,复用现有的 run_group 骨架和监控;监控加一路 `/proc/meminfo` 采样(Cached、Dirty、Available,5 秒一次),E1 看 page cache 增长、E2 看 Dirty 水位都靠它;C1 跑完删 `kv_fs` 之前,先统计它占的磁盘字节数和文件数——fs tier 没有容量上限配置,35G 的盘必须盯着。

## 第 2 步:实验矩阵

### E1 工作集 vs page cache —— 最优先,它决定现有数据能不能要

fs 是 buffered 读,只要数据还在 page cache 里,"读盘"就是读 RAM;uring 是 O_DIRECT,永远真读设备。所以第一个要控制的变量就是"盘上的数据在不在 page cache 里"。做法是 2×2 共四次 serve:fs/uring 两种 tier,各跑"热"(不压内存,空闲内存远大于工作集)和"冷"(ballast 把空闲内存压到小于工作集)两种条件,负载沿用现版的 prime→churn→revisit 串行。

判据有三层。fs 热 vs fs 冷:预期热的 revisit 接近 RAM 速度,而且 iostat 里 revisit 期间设备 r/s 约等于零——这就是"上一轮 139ms 根本没读盘"的实锤;冷的会掉到真实盘速并伴随 cache 抖动。uring 热 vs 冷:预期两者一致,O_DIRECT 不吃 page cache,这份稳定性本身就是结论。第三层看 meminfo 的 Cached 曲线:fs 组一路上涨——同一份 KV 在 CPU tier 和 page cache 里存了两份,这个内存双份开销是独立论点;uring 组应该是平的。

生产语境的解读要提前想好:推理宿主机的空闲内存本来就被模型权重、CPU tier 的 pinned memory 吃掉,"冷"才是真实区间。"热"区间 fs 赢是 page cache 的功劳,但代价是那份内存本可以显式加给 CPU tier——显式加 CPU tier 永远优于让 page cache 隐式缓存,因为前者命中时连盘路径的序列化开销都省了。

### E2 持续写压力下的尾延迟 —— 已砍(2026-07-12,前提被第 0 步推翻)

这一节原本赌的是 buffered 写的脏页积累触发内核 writeback 风暴,与 O_DIRECT 的平稳节奏形成"周期性 spike vs 全程平"的分化。fs 实测是 O_DIRECT 写(还带 tmp+rename 原子替换),不积脏页——E1 四组监控里 `file_dirty` 恒为零——风暴无从发生,开环发压、Dirty 曲线对齐这些设计全部失去观测对象。持续写压力下残存的分化可能(元数据事务的节奏、线程池的调度抖动)已并入 E3/E4 的观测面,不值得单独烧一晚。原设计全文见 git 历史;配套的 `--churn-rate` 开环工具项一并取消,"闭环控制在飞数属于 E3"的分工结论保留。

### E3 并发 load 深度 —— 主战场(2026-07-12 按第 0 步答案重写预期)

真实场景里"一批用户同时回来"是常态,对应 N 个并发 load。原预期"fs 同步 syscall 一次只能在飞一个 IO"已被源码推翻:fs 有 16 条读线程,而且单个 load job 的 blocks 会摊给整个线程池,c ≤ 16 的档位它同样能把设备队列打满。所以这不再是"异步吊打同步"的表演赛,而是两种并发模型在同一设备上限下的真实对决:fs 一边是 32 条用户态阻塞线程(16读+16写)+ 每 block 一次 open/O_DIRECT readv/close 的元数据路径 + Python 层任务派发(syscall 期间放 GIL,派发本身持锁);uring 一边是单提交线程批量提交,在本宿主的 md punt 下退化为 ~8 个 io-wq 内核 worker(E1 实测均值 7)。吞吐大概率被设备封顶打平,分化预期在三处:TTFT p99(fs 的每文件 open/close 在高并发下可能形成元数据串行点,uring 是内核态排队),CPU/GB(E5 的数据源),aqu-sz 的稳定性。E1 已经给了 c=1 的锚点:uring 快 ~11%、分布零重叠——E3 要回答的是这个差距随并发放大还是收敛。

做法不变:同一个 serve 内 prime 一次,对 c ∈ {1, 4, 8, 16}(上限受 sessions=16 限制)依次执行"churn 把前缀挤回盘 → revisit @ 并发 c",每档至少 3 轮取合并样本(bench 的 `--phases` 支持循环,revisit 每轮换新问题防止整条 prompt 命中 prefix cache)。两种 tier 各一次 serve,不压舱(并发轴不和内存压力轴混跑)。作废条件照旧:每档 re-churn 后要确认前缀真的回了盘——现在每个样本带绝对时间戳,事后把各 revisit 窗口对到 iostat 上看读流量即可判定,读量对不上工作集的档位作废。旁证三路:iou_wrk 数量、pidstat -t 的线程级 CPU、iostat aqu-sz。

**初判(2026-07-12 两轮独立 serve,每档合并 n=48,待 t_wall×iostat 作废条件核验后冻结)。** 答案是"放大":uring 的均值优势从 c=1 的 8~13% 单调放大到 c=16 的 26~28%(fs 753/749ms vs uring 559/539ms,p99 1151/1117 vs 912/912)。fs 在 c=16 的 revisit/prime ratio 两轮都是 1.11——从盘拉回比串行重算还慢(脚注:分母 prime 是串行基线,16 路并发重算同样会排队,但同档横向对比干净),uring 同档保住 0.84/0.81。两轮复现度极高(uring@4 两轮均值同为 209.2ms),证实同 serve 交错 + 多轮合并的设计确实锁住了 E1 暴露的跨 serve 环境漂移。两个待跟进旁证:fs 的 churn 均值系统性比 uring 高 ~10ms(26 批全部同向,疑似 store 路径 CPU 干扰 prefill,E5 用 pidstat 证实);fs 落盘无上限,13 轮 churn 累积 40462 文件/34.6GiB,距盘满余 0.7G——fs tier 连容量配置项都没有,这既是运维层面的对比点,也是 E4 设计 churn 总量的硬约束。

**核验与冻结(2026-07-12 晚,t_wall×iostat 对齐,取 1842 轮)。** 作废条件通过:四档所有轮的 revisit 窗口盘读量几乎全部精确等于 5.32GB(个别轮记到 3~4GB 是突发跨采样边界的记账损失,不是缓存命中——原始时间线上整波突发都在),前缀确实每轮都从盘上回来,无作废档位。但对齐同时揭开一个自己埋的解读陷阱:高并发档反复出现的 1089MB/s 差点被当成"设备封顶"——其实是 5.32GB 突发(1.5~3s)摊在 5s iostat 采样里的均值(5.32GB÷5s≈1.06GB/s);md RAID1 的读走双镜像,真实瞬时聚合带宽能到 3.5GB/s。所以吞吐结论要反着写:**高并发下两边都没打到设备上限,分化是带宽提取能力**——有效带宽(5.32GB÷窗口时长)uring 随 c 一路爬到 ~3.5GB/s(c=16),fs 在 ~2.5GB/s 出平台。三路旁证自洽:(a) aqu-sz uring 钉在 ~8,恰好等于 io-wq worker 数——md punt 把 uring 的设备并发钉死在这里,这仍是被 punt 压着的下界,5.17+ 内核上不封顶;fs 6~7 且 c=16 掉到 4~6,线程时间被 open/close 元数据吃掉,喂不满设备。(b) 设备请求尺寸 uring 342~500KB vs fs 135~250KB——slab 预热后 extent 连续,IO 在 md 层合并得大;40462 个小文件 extent 天然碎,同字节数 fs 要发 2~3 倍请求。(c) pidstat 上 fs 的 EngineCore 系线程更热(c=8 时 119% vs 99%),c=1 的干净窗口(10s,不受采样量化影响)CPU/GB fs 高 ~10%(2.18 vs 1.99 CPU·s/GB)——E5 素材成立,churn 期 fs 总 CPU 也高(116% vs 110%),与"store 路径 CPU 干扰"猜想同向。一条负结果记录在案:md0 的 r_await 在所有繁忙采样点恒为 445~446ms,与 aqu-sz 完全对不上账(2507 r/s × 0.445s 应得 aqu≈1100,实测 8),是 md 层统计残次品,从判据里剔除。**E3 冻结:差距随并发放大(均值 +15%→+39%,p99 +17%→+22%),归因是 fs 的每文件元数据路径既偷走设备队列深度、又碎掉 IO 尺寸;uring 即便被 punt 钉在 qd≈8 也全档赢。** CPU 轴的最终口径(2026-07-12 晚二次核对后**降级**):process 级 CPU-秒 fs 确实多 +10%(c=1)到 +35%(c=16),io-wq worker 实测占 uring 账面 0.10~0.43 CPU·s/轮——但这些是记账差异不是干扰证据:绝对量是 128 核上多 ~0.1 个核,线程级形态两组一致(单条引擎主循环 92~97%,其余全部 <8%,fs ~53 条线程沾 CPU、uring ~30 条,没有任何线程饿着),与 project4 "GIL 税≈0"一脉相承——**这个负载下 CPU 不是差异来源,"fs 线程池抢 CPU"不成立,不进结论**。随之 churn TTFT 系统性 +10ms 变成无主悬案(且 uring churn 期设备写量近 3 倍反而更快,设备层干扰解释也不通),挂起待查。

**已知不对称:容量与写放大(2026-07-12 晚,xcheck 全程写量核对)。** 全程 md0 写入 fs 35.4/35.8GB(恰等于 kv_fs 唯一块落盘量,无上限=按文件名天然去重,每块只写一次) vs uring 101.4/101.5GB(两轮精确复现)——2.9× 写放大,根源是 `disk_bytes_to_use=20G` < 本 workload 盘上工作集 ~35G,前缀块被 revisit 拉回后反复逐出重写。后果:revisit 窗口内 uring 还压着 200~250MB/s 的自身逐出写(fs 同窗口写≈0),它是在读写混合状态下赢下 TTFT 全档的——**E3 结论方向成立且保守**,但这是不干净的对照,记为已知不对称。E1 不受影响(该 workload 唯一块 13.6G < 20G,两组读写量对称)。干净对照的选项:HF 缓存挪系统盘腾 16G 后 backing 给 36G,或把后续实验的唯一块总量设计到 20G 以内使上限失效。连带发现一条待查悬案:churn 窗口读流量第 1 轮两组均为 0、随轮次爬升至 ~180-200MB/s 饱和(判别器排除"churn 文本复用回读"——churn 每轮全新),形态指向"前缀被 revisit 拉回→再逐出"循环中的某条回读路径,两组对称不构成混淆,机制待 1 秒粒度 iostat(监控已改)或引擎侧 per-job 账目定位。

### E4 读写混战 —— 最接近真实 serving 的稳态

真实 serving 里 store 和 load 同时发生:新请求持续把块沉下去,老会话同时要把块拉上来。现版的 phase 是干净分离的,最"脏"的稳态反而没测到。用 `--mixed` 跑:churn 流不停,期间每隔几秒插一个 revisit,并发档取 E3 里分化最明显的那一档。(2026-07-12 定档 **c=8**,保守留一档余量:c=16 分化最大但 revisit 波内自排队已到 750ms 量级,mixed 想测的"写流对读路径的干扰"会被自排队淹没;c=8 的分化(+22%)已在噪声外,且留出了干扰导致劣化的观测空间。)判据是 revisit TTFT 相比"安静盘"基线劣化了多少:预期 fs 的读要和自己的写争线程池与设备队列,且 store 侧每 block 的 tmp 文件 + rename 是文件系统日志事务,会在读路径上制造串行点;uring 读写同一个 ring、在设备层公平排队,劣化应显著更小。(writeback 相关的原措辞随 E2 一起作废。)

**干净对照:唯一块总量必须落在 uring 的容量上限以内(2026-07-14 定案)。** E3 暴露的 2.9× 写放大(uring 101G vs fs 35G)必须在 E4 之前解决,否则"写流干扰读路径"这个测量对象会被"uring 自己的逐出重写"污染——两边根本不是同一场比赛。根源已定位在容量:`submit_store` 本来就过滤盘上已驻留/在途的 key(manager.py:222),所以重写只可能来自 `disk_bytes_to_use` < 盘上工作集逼出的 LRU 逐出。做法是让负载的盘上唯一块总量小于容量上限,使上限失效(全程零逐出),两边都是"写一次、留着",写量对称。代价是 mixed 阶段的时长被硬顶死,不能靠加波数拿样本(加样本走加 serve)。

**预算怎么算:兜最坏,不兜预期(第一次跑 e4 失败换来的教训)。** 第一版预算是按"每条 churn 往盘上留多少唯一块"标定的——从 E1 的 276MiB/条 和 E3 的 98.6MiB/条 反解出稳态 82MiB/条,算得全程 17.2G,容量给 20G。实跑直接撞墙:唯一块 **28357MiB**(32405 个块,三次 fs serve 一模一样,负载是确定性的),三个 uring 组全被容量哨兵判 `WARN(capacity)`。错因是把"落盘率"当成了负载常数:一条 6000 词 churn 产出约 488 个块的 KV,能落到盘上多少取决于 store 路径吸收得过来多少,而这个吸收率**不是常数**——E3 只吸收了 23%,E4 吸收了 75%,两者的请求节奏还几乎一样(1.16 vs 1.24 s/条)。机制没查清,记为悬案(见下)。

修法是绕开这个未知数:容量按**最坏情况上界**给,即假设 churn 产出的 KV 100% 落盘。上界只由 prompt 大小和请求条数决定,与吸收率无关,所以算得准:

    上界(块) = prime 8000 + churn 条数 × 488

新配置 churn 16(独立轮)+ mixed 流约 34 条 = 50 条 ⇒ 上界 32400 块 ≈ **27.7G**,容量给 **30G**(留 8% 余量),预期落盘按 75% 吸收率约 22.5G。这个上界同时兜住了一个哨兵测不到的风险:**uring 的 store 路径更快,可能比 fs 吸收更多唯一块**,而收尾的 `kv_fs 落盘` 哨兵只量得到 fs 那一侧——真正的保险是上界,哨兵只是先行指标。盘占用峰值 = max(kv_fs 27.7G, uring backing 30G) = 30G < 35G 可用(组间即删,不共存)。

为把上界压进 30G,churn 从 24 砍到 16、mixed 间隔从 25s 砍到 20s,代价是每轮的挤出压力从 E3 的 24 条降到约 16 条。够不够挤是**自验**的:每波 revisit 的盘读量(t_wall × iostat)若两 tier 相当且量级可观(E3 同档是 5.32GB),前缀就是真从盘上回来的;若明显偏小,说明挤出不足、波打在了 GPU/CPU tier 上,该轮作废。

顺带这一步把写放大的归因变成一个**可证伪的实验**:零逐出之后,uring 的全程 md0 写量应当降到 ≈ 它自己的唯一块字节数(扣掉 prewarm 那 30G)。若它仍然是 2~3 倍,说明"容量逼出 LRU 逐出"的归因是错的,盘上有真 bug(key 不稳定、或有别的路径在重写),得回头查 manager。

**悬案:store 路径的吸收率为什么能差三倍。** E3 的 churn 每条只有 23% 的 KV 落盘(85MB/s),E4 是 75%(约 257MB/s),请求节奏几乎相同。两个候选解释:一是 fs 的 store 吞吐随目录里文件数增长而衰减(E3 累积到 4 万个文件,E4 只有 3.2 万),那样的话 E3 的落盘率是被自己拖垮的、后期在静默丢块——这对 fs 是个独立的负面论点,值得查;二是吸收不下的块被上层直接丢弃(KV 可重算,丢块合法),丢弃阈值与队列深度/在飞 job 数有关。查法:E4 复跑的 iostat 里看 fs 的写带宽随时间是否单调下滑,以及 serve 日志里有没有 store job 失败计数。不阻塞 E4 结论,但会影响 fs 的机制叙事,记在这里别忘。

**第一次 e4 的结果(2026-07-14,results_e2e_20260714_0036,uring 侧被容量污染,方向仍成立)。** 六组全部跑完,数据不能当终稿,但因为污染方向是**对 uring 不利**的(它背着 28.4G 唯一块挤进 20G 容量,全程逐出重写,还可能因前缀被逐出而 revisit MISS 掉进重算),所以它给出的是 uring 的**下界**,清干净后只会更好;fs 那边无上限、零逐出,跑的是它的最佳状态。结论方向因此保守成立:

| | 安静盘基线 revisit@8(同 serve) | mixed_revisit@8 | 劣化 |
|---|---|---|---|
| fs (a/b/c) | 341 / 346 / 357 ms | 635 / 724 / 728 ms | **+86% / +109% / +104%** |
| uring (a/b/c) | 303 / 286 / 303 ms | 375 / 350 / 361 ms | **+24% / +22% / +19%** |

三次复现零翻转。两个更硬的观察:(a) fs 的劣化**逐波恶化**(wave1→wave2:539→730、572→876、634→823),uring 逐波持平(371→378、344→356、374→348)——写流对 fs 读路径的伤害是累积的,对 uring 不是;(b) fs 的 `mixed_revisit@8/prime` 比值三次分别是 0.92/1.04/1.05,**即从盘上拉回前缀已经不比全量重算更快了**——在读写混战下 fs 的磁盘 tier 收益归零,而 uring 同期是 0.54/0.51/0.52。干扰源本身是对称的:两边的 `mixed_churn` TTFT 几乎相同(fs 735/740/740 vs uring 724/722/720),说明 GPU 侧负载一致,分化确实出在 tier 的读路径而不是算力争抢。

**安静盘基线放在同一个 serve 里。** 劣化 = `mixed_revisit@8` − 本 serve 自己的 `revisit@8`,不去减 E3 那次 serve 的数(fs 369.3ms / uring 303.5ms)。理由是 E1 立下的纪律:跨 serve 差异小于 ~15-20ms 的邻居噪声底不许下结论,而劣化幅度事先并不知道有没有超过这个底。所以 phase 是 `prime,churn,revisit@8,mixed@8`,基线和 mixed 相隔几分钟、同一个 serve、同一块盘。已知残留缺口:mixed 期盘上文件数比基线期多(churn 流一直在写),fs 的元数据面随文件数增长,所以 fs 的劣化里掺了一点"文件更多"而非纯"写流干扰"——E3 每档 3 轮(轮间文件数递增)读数稳定,说明这一项经验上很小,如实记为缺口,不假装没有。样本量靠 **a/b/c 三次交替 serve** 补回:每 serve 基线 16 + mixed 2 波 × 16 = 32,合并 48 基线 / 96 mixed 每 tier(≥ E3 每档的 48)。

工具侧已实装并离线冒烟通过(假 openai 客户端,不烧 GPU):波按固定节拍开环(实测间隔精确 2.00s)、churn 流覆盖整个相位(第一波前就在跑、末波后仍未停)、ITL 探针拿到逐 token 时间戳、峰值并发 = 波 c + churn 1 + 探针 1。

### E5 CPU 干扰面 —— 不单独跑,从 E3~E4 的监控里出(2026-07-12 改写归因)

fs 的 CPU 税不在内核拷贝——它是 O_DIRECT,拷贝轴已死——而在两处:32 条线程的唤醒与上下文切换加 Python 层任务派发(每 block 一个 `functools.partial` 过队列),以及每 block 一文件的元数据操作(open/close,store 侧 tmp 文件 + rename 的日志事务)。uring 的 CPU 面是单 C++ 提交线程加内核侧 io-wq worker(md punt 税,5.17+ 内核上自动消失)。两个证据源:一是 pidstat 加 `-t` 拆到线程级,对比同等 GB 吞吐下 EngineCore 全家的 CPU 占用——io-wq worker 是挂在进程下的 PF_IO_WORKER 线程,pidstat -t 能直接看到,两边的账都算得全;二是在 E4 的 mixed 阶段加一个常驻的长 decode 请求,量它的 ITL(inter-token latency,逐 token 间隔)在 IO 高峰期间被压高多少——这是"IO 路径省下的 CPU 还给了 decode"的端到端证据。(2026-07-12 晚初判:证据源一已出结果且为**负**——E3 的 pidstat -t 线程级形态两组一致,CPU 在本宿主 128 核上不是差异来源,详见 E3 冻结段的降级口径。E5 的裁决只剩证据源二 ITL 探针;若 E4 的 ITL 也无分化,E5 as 负结果写进报告——128 核宿主淹掉了 CPU 轴,更小的宿主上该轴可能重新显形,但那超出本轮实测范围,只许写成推测。)

**E5 裁决:负结果(2026-07-14,E4 的 ITL 探针出数)。** 证据源二也是负的,而且负得毫无悬念:六组的常驻 decode 探针各 1785 个 ITL 样本,均值 fs 29.65/29.90/30.07ms vs uring 30.00/30.63/29.91ms,**p50 六组全部是 16.35ms**,一模一样。写流高峰期把 ITL 压高的幅度两组无差别(p99 也都在 227~230ms)。所以"IO 路径省下的 CPU 还给 decode"这条端到端证据在本宿主上不存在——两条证据源全负,E5 按负结果写进报告:**这个负载下 CPU 不是分化来源**。这不是 uring 的失败,而是一条诚实的边界——128 核宿主把 CPU 轴彻底淹掉了,分化全部来自 IO 路径本身(E3 已证:元数据面偷设备队列深度 + 碎掉 IO 尺寸)。更小的宿主(比如 8~16 核的推理机,fs 的 32 条池线程占比会陡增)上这条轴可能重新显形,但那超出本轮实测范围,报告里只许写成推测,不许当结论。

### E6 真实 trace 锚点 —— 收尾

回放一条真实 trace,两种 tier 各跑一遍。它的作用不是探索空间——E1~E4 已经覆盖了——而是标定:真实多轮负载落在 E1 的冷/热哪个区间、E3 的哪个并发档。这样报告的推理链是完整的:轴扫描曲线说明什么条件下谁赢、为什么,真实 trace 说明真实世界落在哪个条件里,最后这一跑同时充当预测的验证。

**选 Mooncake(2026-07-14 定案)。** 它是生产 trace,直接就是 KV-cache/block 粒度的多轮会话记录,带真实到达时间——和我们的观测对象同层,映射到 prompt 生成器最直接。ShareGPT 只有文本没有到达时间戳,得自己合成到达过程(泊松或借 BurstGPT 的到达率),那样标定出来的"真实世界落在哪个并发档"有一半是我自己编的到达率,标定意义打折,所以不用。要标定的三个量:revisit 窗口的盘读量落在冷区还是热区;并发 load 深度的分布(对到 E3 的哪个档);读写比(对到 E4 的干扰强度)。

### E7 复用局部性 —— 设计完备但不跑,省略理由如下

五个自由度里唯一没有对应实验的是复用局部性,这里把"为什么不测"写明,免得矩阵看起来有漏洞。如果要测,做法是固定工作集总量,扫前缀池配比 P×F:维护 P 条互不相同的长前缀,每条被 F 个会话共享(prompt = 共享前缀 + 每会话独有后缀),保持 P×F×前缀长度不变。P=1、F=16 是"全员共用一条 system prompt"的极端局部性——所有读砸在同一小撮 block 上;P=16、F=1 就是现版每会话独有前缀,读均匀散开。vLLM 的 prefix caching 按 block 内容哈希,前缀 token 相同即映射到同一批 KV block,构造上是可行的。预期是 fs 在高局部性端靠 page cache 对热 block 的反复命中占便宜,uring 无缓存、两端表现一致。

不跑的理由是这个轴在磁盘层是自我衰减的:高局部性意味着同一批 block 被反复访问,而被反复访问的 block 恰恰会被 GPU/CPU tier 留住,根本沉不到磁盘;真正落到磁盘 tier 的读,天然是被上层筛剩下的低局部性尾部。所以磁盘层 IO 流的局部性区间很窄,E1 冷条件(每会话独有前缀、逐出后回访)已经落在这个真实区间里,单独扫这条轴测到的高局部性端在生产中不会出现。把这段论证写在报告里,比多跑一组实验更有信息量;若结论被挑战,上面的 P×F 设计随时可以启用。

## 第 3 步:产出

每个轴一张图:E1 交叉条形(已有数据)、E3 并发曲线、E4 劣化对比、E5 CPU/ITL 对照,汇成 BENCH_ANALYSIS.md 的新章节或独立的 COMPARE_RESULTS.md。主指标为什么是 TTFT 而不是 TBT/吞吐/goodput、判据的三层结构(主指标/归因/哨兵),完整推导见 METRICS.md。最终给一张结论矩阵:行是轴,列是 fs 赢、打平、uring 赢的条件区间。原本预期的"fs 赢的区间(热 page cache 加低并发)"被第 0 步证伪——fs 根本不用 page cache——但诚实原则不变:E1 三轮的最终判决是热区均值打平(单轮的 11% 领先没能跨轮复现,被邻居噪声推翻)、uring 赢在稳定性与冷区,"fs 的 O_DIRECT + 线程池设计本身相当能打,分差来自文件布局"这个对对手有利的归因也是报告的一部分;E3 若打平或翻盘照写。

## 风险与执行顺序

盘只剩 35G 是最紧的约束:uring backing 20G、fs 目录(无上限)、模型缓存三者共存不下,沿用"跑完即删"的串行策略(overnight 脚本已把 fs/uring 的落盘统计+清理做成每组收尾动作)。样本量方面,n=16 撑不起 p99,关键实验(E3 各并发档)的 revisit 至少循环 3 轮取合并样本——serve 内循环比多起 serve 便宜;E1 热区结论靠分布零重叠成立,不受此限。公平性方面,两 tier 的磁盘容量、CPU tier 大小保持一致,fs 组沿用 PYTHONHASHSEED=0,关键结论换 seed 重跑一遍确认不翻。

执行顺序(2026-07-12 更新,E1 共跑三轮):E1 已完成并冻结。单轮内"热区 uring 赢 ~11%、分布零重叠"没能跨轮复现——fs_hot 三轮在 129~148ms 间漂(±9ms,大于均值差),uring_hot 三轮 131±2ms 纹丝不动——所以热区判平,且立下纪律:跨 serve 差异小于 ~15-20ms(共享 md0 的邻居噪声底)不许下结论。冷区三连胜方向零翻转(fs 181/197/187 vs uring 142/147/161,p99 差更大)。E1 定论两条:热区均值打平但 uring 对环境扰动显著更稳(15574 个小文件的元数据面暴露在邻居 IO/dentry/分配器状态下,单 slab 纯数据通路没有作用面);内存压力下 fs 因元数据路径系统性劣化,uring 近乎免疫。机制归因与三层判据闭环记录在第 0 步答案;E2 已砍。E3 已完成并冻结(2026-07-12,两轮独立 serve 复现 + t_wall×iostat 核验,定论见 E3 节):差距随并发放大,uring 全档赢,E4 定档 c=8。

2026-07-14:E4 第一次跑完(六组 a/b/c 交替,19 分钟),容量哨兵按设计动作,判 uring 三组 `WARN(capacity)`——唯一块实测 28.4G 撞穿 20G 上限,对照不干净。错因已定位(预算按"预期落盘率"算,而落盘率不是负载常数)并改成按最坏情况上界给容量:容量 20G→30G、churn 24→16、mixed 间隔 25s→20s,上界 27.7G < 30G,盘占用峰值 30G < 35G。这一轮的数据方向上仍成立且对 uring 保守(fs 劣化 +86~109%,uring +19~24%,三次零翻转,详见 E4 节)。E5 已凭这一轮的 ITL 探针出裁决(负结果,两条证据源全负)。

**下一步:重跑 `bash bench/run_e2e_overnight.sh e4`**,期待六组全 OK。拿到数后核三件事:(1) 每波 revisit 的盘读量两 tier 相当且量级可观——churn 砍到 16 之后挤出压力变弱,这条作废条件必须验;(2) uring 全程写量降到 ≈ 自身唯一块字节(写放大归因的证伪实验);(3) fs 的写带宽是否随文件数单调下滑(吸收率悬案)。之后 E6 用 Mooncake trace 收尾。全程沿用 overnight 脚本的无人值守骨架。