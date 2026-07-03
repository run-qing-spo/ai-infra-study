#include "cached_backend.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace p3 {

CachedBackend::CachedBackend(std::unique_ptr<IoBackend> inner,
                             size_t max_in_flight,
                             size_t slot_count,
                             uint32_t block_size)
    : block_size_(block_size),
      slot_count_(slot_count),
      inner_(std::move(inner)) {
    if (!inner_) {
        throw std::invalid_argument("CachedBackend: inner must not be null");
    }
    if (slot_count < max_in_flight) {
        // 见 hpp:slot_count < max_in_flight 会有 miss 请求借不到 slot 的问题。
        // 学习项目直接抛;工程实现可以选择排队等待。
        throw std::invalid_argument("slot_count must be >= max_in_flight");
    }
    if (block_size_ == 0 || (block_size_ % 512) != 0) {
        // O_DIRECT 至少要求 512B 对齐。这里放宽,只要 512 的倍数就允许,
        // 具体对齐在 posix_memalign 时按 4096 对齐(覆盖大多数 NVMe)。
        throw std::invalid_argument("block_size must be a positive multiple of 512");
    }

    slots_.resize(slot_count_);
    free_slots_.reserve(slot_count_);
    // 反着 push,让 pop_back 出来的 idx 从 0 开始,只是为了调试打印时好看
    for (int32_t i = static_cast<int32_t>(slot_count_) - 1; i >= 0; --i) {
        if (::posix_memalign(&slots_[i].buf, 4096, block_size_) != 0) {
            // 已分配的会在析构时释放,这里直接抛
            throw std::runtime_error("posix_memalign failed in slot pool");
        }
        free_slots_.push_back(i);
    }

    offset_index_.reserve(slot_count_ * 2);
    misses_.reserve(max_in_flight * 2);
}

CachedBackend::~CachedBackend() {
    // 保险:如果上层没 drain,inner 可能还在往 slot 里 DMA。
    // 简单做:忽略剩余 completion,只把内存释放。生产上应该 drain。
    for (auto& s : slots_) {
        if (s.buf) ::free(s.buf);
    }
}

// ---- LRU 双链表操作 ----------------------------------------------------------

void CachedBackend::lru_push_front(int32_t idx) {
    auto& s = slots_[idx];
    s.lru_prev = kNil;
    s.lru_next = lru_head_;
    if (lru_head_ != kNil) slots_[lru_head_].lru_prev = idx;
    lru_head_ = idx;
    if (lru_tail_ == kNil) lru_tail_ = idx;
}

void CachedBackend::lru_remove(int32_t idx) {
    auto& s = slots_[idx];
    if (s.lru_prev != kNil) slots_[s.lru_prev].lru_next = s.lru_next;
    else                    lru_head_ = s.lru_next;
    if (s.lru_next != kNil) slots_[s.lru_next].lru_prev = s.lru_prev;
    else                    lru_tail_ = s.lru_prev;
    s.lru_prev = s.lru_next = kNil;
}

void CachedBackend::lru_touch(int32_t idx) {
    // 已经在头?那就啥都不做,省一次链表改动
    if (lru_head_ == idx) return;
    lru_remove(idx);
    lru_push_front(idx);
}

// ---- slot 申请 --------------------------------------------------------------

int32_t CachedBackend::acquire_slot_for(uint64_t offset) {
    int32_t idx;
    if (!free_slots_.empty()) {
        idx = free_slots_.back();
        free_slots_.pop_back();
    } else {
        // 必须从 LRU 尾驱逐一个 valid 且非 pinned 的 slot。
        // 由于 slot_count >= max_in_flight,in-flight miss 最多这么多个 →
        // LRU 里至少还剩 slot_count - max_in_flight 个可驱逐,只要
        // slot_count >= max_in_flight + 1 就一定有。等号成立时理论上
        // 可能全 pinned;这种极端 corner case 学习项目里忽略。
        if (lru_tail_ == kNil) {
            // 不应该发生:slot_count >= max_in_flight 且 free 空 → LRU 至少 1 个
            throw std::runtime_error("cache pool exhausted: all slots pinned");
        }
        idx = lru_tail_;
        lru_remove(idx);
        // 把老 offset 从索引里踢掉
        offset_index_.erase(slots_[idx].offset);
        slots_[idx].valid = false;
    }
    slots_[idx].offset = offset;
    slots_[idx].pinned = true;   // 借给 in-flight miss 用,期间不能被驱逐
    slots_[idx].valid = false;   // 数据还没读回来,标 invalid
    offset_index_[offset] = idx; // 提前占位,避免同 offset 并发 miss 时重复下盘
    return idx;
}

// ---- submit -----------------------------------------------------------------

size_t CachedBackend::submit(const IoRequest* reqs, size_t n) {
    size_t accepted = 0;
    // 一批里 miss 请求先攒起来,统一交给 inner 一次 submit,
    // 保留 io_uring "N 请求 1 syscall"的批量收益(sync backend 里也无所谓)。
    std::vector<IoRequest> to_inner;
    to_inner.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const IoRequest& r = reqs[i];

        // 大小 / 对齐不匹配 → 不进 cache,直接透传 inner
        const bool cacheable =
            (r.size == block_size_) && ((r.offset % block_size_) == 0);
        if (!cacheable) {
            to_inner.push_back(r);
            ++stats_.passthrough;
            ++accepted;
            continue;
        }

        if (r.op == IoRequest::READ) {
            auto it = offset_index_.find(r.offset);
            if (it != offset_index_.end() && slots_[it->second].valid) {
                // ---- Cache hit ----
                int32_t idx = it->second;
                std::memcpy(r.buf, slots_[idx].buf, block_size_);
                lru_touch(idx);
                IoCompletion cq{};
                cq.user_data = r.user_data;
                cq.res       = static_cast<int32_t>(block_size_);
                pending_completions_.push_back(cq);
                ++stats_.hits;
                ++accepted;
                continue;
            }
            // ---- Cache miss:借 slot,以内部 id 提交给 inner ----
            // 特例:同 offset 已经有在飞 miss(slot 存在但 valid=false + pinned)
            // 简单做:直接透传 inner,不共享那个 pending slot(会重复下盘一次,
            // 生产可优化成"挂 waiter 队列",学习项目不做)。
            if (it != offset_index_.end()) {
                to_inner.push_back(r);
                ++stats_.passthrough;
                ++accepted;
                continue;
            }
            int32_t slot_idx = acquire_slot_for(r.offset);
            uint64_t iid = next_internal_id_++;
            misses_[iid] = InflightMiss{
                /*slot_idx=*/ static_cast<size_t>(slot_idx),
                /*user_data=*/ r.user_data,
                /*user_buf=*/ r.buf,
                /*user_size=*/ r.size,
                /*is_read=*/ true,
            };
            IoRequest inner = r;
            inner.buf       = slots_[slot_idx].buf; // DMA 目标改成 slot
            inner.user_data = iid;                  // 用内部 id 跟踪
            to_inner.push_back(inner);
            ++stats_.misses;
            ++accepted;
        } else {
            // ---- Write:透传 + invalidate ----
            auto it = offset_index_.find(r.offset);
            if (it != offset_index_.end()) {
                int32_t idx = it->second;
                if (!slots_[idx].pinned) {
                    lru_remove(idx);
                    // 归还 free 池;下次 miss 可复用
                    slots_[idx].valid = false;
                    free_slots_.push_back(idx);
                }
                // pinned 说明该 offset 正在 miss 读回来:invalidate 语义变复杂
                // (读回来的数据"过期"了)。简单做:留在原地,等 miss 完成时
                // 把 slot 的 valid 设为 true——这里就带上了"已被 write 污染"的
                // 老数据。学习项目暂不修;生产要么改 write-through 要么加脏位。
                offset_index_.erase(it);
                ++stats_.writes_invalidated;
            }
            to_inner.push_back(r);
            ++accepted;
        }
    }

    // 一次性 submit 给 inner。inner 可能吃不下全部(SQ 满),此时后半段丢弃、
    // 上层 bench 靠 in_flight() 和 reap 循环收敛。学习项目里 qd 管控得住,
    // 通常不会遇到。
    if (!to_inner.empty()) {
        size_t taken = inner_->submit(to_inner.data(), to_inner.size());
        inner_pending_ += taken;
        // 如果没全被接受,后面的请求丢了 → 我们记的 miss 表会漏 completion。
        // 学习实现:检测到就抛出,让上层能立刻看到问题,而不是静默 hang。
        if (taken < to_inner.size()) {
            throw std::runtime_error(
                "inner backend did not accept full batch (SQ full?); "
                "raise queue_depth or drain more often");
        }
    }
    return accepted;
}

// ---- reap -------------------------------------------------------------------

size_t CachedBackend::reap(IoCompletion* out, size_t max_n, size_t min_complete) {
    size_t produced = 0;

    // 1) 先吃 pending queue(cache hit 立即完成的部分)
    while (produced < max_n && !pending_completions_.empty()) {
        out[produced++] = pending_completions_.front();
        pending_completions_.pop_front();
    }

    // 2) 如果 pending 已经够 min_complete,不用去 kernel 那边等
    if (produced >= min_complete) {
        // 还能不能顺手从 inner 拿些已经 ready 的?能:min=0 非阻塞 peek
        if (produced < max_n && inner_pending_ > 0) {
            std::vector<IoCompletion> tmp(max_n - produced);
            size_t k = inner_->reap(tmp.data(), tmp.size(), 0);
            inner_pending_ -= k;
            for (size_t i = 0; i < k && produced < max_n; ++i) {
                IoCompletion cq = tmp[i];
                auto it = misses_.find(cq.user_data);
                if (it == misses_.end()) {
                    // write 透传的 completion(user_data 是原始的,不在 misses_ 表)
                    out[produced++] = cq;
                    continue;
                }
                // miss 读完:memcpy slot → user_buf,slot 入 LRU
                InflightMiss m = it->second;
                misses_.erase(it);
                int32_t idx = static_cast<int32_t>(m.slot_idx);
                if (cq.res >= 0) {
                    std::memcpy(m.user_buf, slots_[idx].buf, m.user_size);
                    slots_[idx].valid = true;
                    slots_[idx].pinned = false;
                    lru_push_front(idx);
                } else {
                    // IO 失败:slot 丢回 free,offset 索引撤销
                    slots_[idx].valid = false;
                    slots_[idx].pinned = false;
                    offset_index_.erase(slots_[idx].offset);
                    free_slots_.push_back(idx);
                }
                IoCompletion uc{};
                uc.user_data = m.user_data;
                uc.res       = cq.res;
                out[produced++] = uc;
            }
        }
        return produced;
    }

    // 3) pending 不够 min_complete,必须去 inner 等
    // inner 要等的数量 = min_complete - produced,但不能超过 inner_pending_
    size_t need_from_inner = min_complete - produced;
    if (need_from_inner > inner_pending_) {
        // 需要的比在飞的还多 → 上层调用不合理,尽力而为(退化成非阻塞)
        need_from_inner = inner_pending_;
    }

    std::vector<IoCompletion> tmp(max_n - produced);
    size_t k = inner_->reap(tmp.data(), tmp.size(), need_from_inner);
    inner_pending_ -= k;
    for (size_t i = 0; i < k && produced < max_n; ++i) {
        IoCompletion cq = tmp[i];
        auto it = misses_.find(cq.user_data);
        if (it == misses_.end()) {
            out[produced++] = cq;
            continue;
        }
        InflightMiss m = it->second;
        misses_.erase(it);
        int32_t idx = static_cast<int32_t>(m.slot_idx);
        if (cq.res >= 0) {
            std::memcpy(m.user_buf, slots_[idx].buf, m.user_size);
            slots_[idx].valid = true;
            slots_[idx].pinned = false;
            lru_push_front(idx);
        } else {
            slots_[idx].valid = false;
            slots_[idx].pinned = false;
            offset_index_.erase(slots_[idx].offset);
            free_slots_.push_back(idx);
        }
        IoCompletion uc{};
        uc.user_data = m.user_data;
        uc.res       = cq.res;
        out[produced++] = uc;
    }
    return produced;
}

size_t CachedBackend::in_flight() const {
    return pending_completions_.size() + inner_pending_;
}

} // namespace p3
