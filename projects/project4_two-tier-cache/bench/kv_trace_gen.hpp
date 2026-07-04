// KV cache workload trace generator(骨架版)。
//
// 为什么需要这个:
//   通用 block cache 的 bench 用独立 Zipf 采样 blockid 就够 —— key 之间无关,
//   只有热度差。LLM inference 的 KV cache 不是这样,它有两根骨架:
//     1) 前缀共享:一堆 request 共用 system prompt / few-shot 前缀,
//        真实工作集是一棵 prefix tree,根部命中率接近 100%。
//     2) 顺序 append:prefill 一口气写完 prompt 的所有 block(burst put),
//        decode 一步 append 一个新 block,中间要读该 request 迄今为止的历史。
//   独立 Zipf 刻画不了这两件事,所以拿它测 cache 出来的结论(hit rate、锁竞争)
//   跟真实 KV serving 的形状对不上。
//
// 这里做的最小仿真:
//   - 一个 prefix pool(num_prefixes 个),权重 Zipf 采样(theta 可比通用 workload
//     更陡,因为 system prompt 通常一家独大)。每个 prefix 占若干个连续 block_id,
//     这些 id 在不同 request 之间共享 —— 命中它们就是命中 prefix cache。
//   - 每个 request:采样 (prefix_id, prompt_blocks, output_blocks)。长度用
//     log-normal(长尾:少数长请求主导)。
//   - prefill 段展开成 prompt_blocks 个 kGet:前 prefix_blocks 个用 prefix
//     公共 id,后段用该 request 独占的 tail id。走 look-aside 语义 —— caller
//     miss 时应立即 put,这样后续 request 命中同一 prefix 时才能 hit。
//   - decode 段每步:采样一个历史 block 做 kGet(简化的 attention 访问:只 sample
//     一次而不是读全 sequence,否则 op count 会 O(len^2) 爆),再 kPut 一个新
//     append block。
//
// 换真 trace 的路子:后续把 generate() 替换为 replay Mooncake / Azure LLM trace
// 的 JSONL,输入相同的 KVOp 序列,上层 bench 不用改。
//
// 面试点:被问"你 bench 为什么这么造"时,答案不是"Zipf 更真实",是"KV cache
// 的正确 workload 骨架是 prefix tree + burst append,Zipf 独立采样刻画不了
// 结构关系"。

#pragma once

#include "cache.hpp"   // for BlockId

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace p4 {

enum class OpKind : uint8_t {
    kGet,   // look-aside read:hit 直接返,miss 时 caller 负责 fill + put
    kPut,   // 无条件写:decode append 新 block 时用
};

struct KVOp {
    OpKind   kind;
    BlockId  id;
    uint32_t request_id;   // 备用:后面做粒度感知淘汰或 trace 归因分析时用
};

struct KVTraceConfig {
    // —— prefix pool ————————————————————————————————
    // 多少个 "上下文骨架"(system prompt / few-shot 变种数)。真实生产里这个
    // 数字通常很小(1~几十),因为 system prompt 是被反复用的那几段。
    size_t num_prefixes    = 32;
    // prefix 池热度分布 theta。真实 KV workload 里 system prompt 一家独大,
    // 这个值建议 > 1.0,比通用 workload 更陡。
    double prefix_theta    = 1.2;
    // 每个 prefix 占多少 block(先固定,后续如果要更真实,可以按 log-normal 抽)。
    size_t prefix_blocks   = 8;

    // —— request 流 ————————————————————————————————
    size_t num_requests    = 500;

    // prompt / output 长度分布(单位:block,不是 token)。log-normal 造长尾。
    // mean_blocks 就是 lognormal 的 median(接近 mean),sigma 越大尾越长。
    double prompt_mean_blocks   = 16.0;
    double prompt_sigma         = 0.6;
    double output_mean_blocks   = 8.0;
    double output_sigma         = 0.5;

    uint64_t seed          = 42;
};

class KVTraceGen {
public:
    explicit KVTraceGen(KVTraceConfig cfg)
        : cfg_(cfg),
          next_unique_id_(static_cast<BlockId>(cfg.num_prefixes * cfg.prefix_blocks)),
          rng_(cfg.seed) {
        // prefix 的 Zipf CDF。第 i 个 prefix 权重 1 / (i+1)^theta。
        prefix_cdf_.resize(cfg.num_prefixes);
        double sum = 0.0;
        for (size_t i = 0; i < cfg.num_prefixes; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i + 1), cfg.prefix_theta);
            prefix_cdf_[i] = sum;
        }
        for (auto& c : prefix_cdf_) c /= sum;
    }

    // 产出完整 op 序列。stream 里同 request 的 op 是连续的 —— 一个 request
    // 的 prefill+decode 打包 flush 出来,再进入下一个 request。这一版不做
    // 到达时间交错(泊松到达):对 cache 层单机 bench,交错主要影响并发时序,
    // 先把结构对了更重要。后续要做交错时,加一个 arrival_time 字段 + merge。
    //
    // 副产物:request_starts_[k] 是第 k 个 request 起始 op 在 ops 里的下标,
    // 末尾追加一个 ops.size() 当哨兵。多线程 bench 按 request 边界切片时用它,
    // 不然把同 request 的 prefill 和 decode 拆到不同线程 → 破坏语义。
    std::vector<KVOp> generate() {
        std::vector<KVOp> ops;
        request_starts_.clear();
        request_starts_.reserve(cfg_.num_requests + 1);
        // 粗估:平均 prompt + 2*output (decode 每步 2 个 op)
        ops.reserve(cfg_.num_requests *
                    static_cast<size_t>(cfg_.prompt_mean_blocks
                                        + 2.0 * cfg_.output_mean_blocks));

        for (size_t req = 0; req < cfg_.num_requests; ++req) {
            request_starts_.push_back(ops.size());
            const uint32_t rid = static_cast<uint32_t>(req);

            // 1) 选 prefix。相同 pfx 的 request 会在 prefill 头段命中同一批 block。
            const size_t pfx = sample_prefix_id();
            const BlockId prefix_base =
                static_cast<BlockId>(pfx * cfg_.prefix_blocks);

            // 2) 采样长度。prompt 至少要盖满 prefix,否则 "共享前缀" 名不副实。
            const size_t prompt_blocks = std::max<size_t>(
                cfg_.prefix_blocks,
                sample_lognormal_blocks(cfg_.prompt_mean_blocks, cfg_.prompt_sigma));
            const size_t output_blocks = std::max<size_t>(
                1,
                sample_lognormal_blocks(cfg_.output_mean_blocks, cfg_.output_sigma));

            // 3) prefill 段。seq 累计该 request 迄今为止的所有 block_id,
            //    decode 段要从里头 sample 历史访问。
            std::vector<BlockId> seq;
            seq.reserve(prompt_blocks + output_blocks);

            // 前 prefix_blocks 个 = 公共 prefix id(hit prefix cache 的入口)
            for (size_t i = 0; i < cfg_.prefix_blocks; ++i) {
                const BlockId id = prefix_base + static_cast<BlockId>(i);
                seq.push_back(id);
                ops.push_back({OpKind::kGet, id, rid});
            }
            // 后段 = 该 request 独占的 tail(必 miss 一次,put 完就没人再命中了)
            for (size_t i = cfg_.prefix_blocks; i < prompt_blocks; ++i) {
                const BlockId id = next_unique_id_++;
                seq.push_back(id);
                ops.push_back({OpKind::kGet, id, rid});
            }

            // 4) decode 段。每步 sample 一个历史 block 读一下(简化的 attention
            //    历史访问),再 append 一个新 block。
            //    简化点:真实 attention 每步要读所有历史 KV,那样 op count 是
            //    O(prompt * output),对 bench 没有增益反而拖慢。sample 1 已经
            //    保留了 "历史 block 会被反复访问 → 该请求内部的时间局部性"
            //    这个关键性质。
            std::uniform_int_distribution<size_t> hist_pick(0, 0);
            for (size_t i = 0; i < output_blocks; ++i) {
                hist_pick.param(
                    std::uniform_int_distribution<size_t>::param_type(0, seq.size() - 1));
                const size_t h = hist_pick(rng_);
                ops.push_back({OpKind::kGet, seq[h], rid});

                const BlockId new_id = next_unique_id_++;
                seq.push_back(new_id);
                ops.push_back({OpKind::kPut, new_id, rid});
            }
        }
        request_starts_.push_back(ops.size());   // 尾哨兵
        return ops;
    }

    // 每个 request 起点在最近一次 generate() 输出里的偏移。size = num_requests+1,
    // 末尾是 ops.size()。多线程分片按这个数组切,别按 op 数均分。
    const std::vector<size_t>& request_starts() const { return request_starts_; }

    // 给 bench 用来选 cap 的参考量:整个 prefix 池占多少 block。cap 设成这个
    // 值附近可以观察 "刚好放得下 prefix、tail 全 miss" 的边界效应。
    size_t prefix_pool_block_count() const {
        return cfg_.num_prefixes * cfg_.prefix_blocks;
    }

private:
    size_t sample_prefix_id() {
        const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        auto it = std::lower_bound(prefix_cdf_.begin(), prefix_cdf_.end(), u);
        return static_cast<size_t>(it - prefix_cdf_.begin());
    }

    size_t sample_lognormal_blocks(double mean_blocks, double sigma) {
        // 用 lognormal(log(mean), sigma),中位数就是 mean_blocks。
        std::lognormal_distribution<double> d(std::log(mean_blocks), sigma);
        const double v = d(rng_);
        if (v < 1.0)     return 1;
        if (v > 8192.0)  return 8192;   // clamp,别让离群值把 bench 拖住
        return static_cast<size_t>(v + 0.5);
    }

    KVTraceConfig       cfg_;
    std::vector<double> prefix_cdf_;
    std::vector<size_t> request_starts_;
    BlockId             next_unique_id_;   // request tail / decode append 用
    std::mt19937_64     rng_;
};

} // namespace p4
