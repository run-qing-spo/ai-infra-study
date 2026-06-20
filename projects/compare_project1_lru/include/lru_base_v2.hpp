#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

template <typename K, typename V>
class lrucache_v2 {
private:
    struct Node {
        // ← 关 键 变 化 ： 原 来 的  K key 替 换 为 指 向  map 内 部  pair 的  iterator
        // 好 处 ： (1) 不 再 额 外 存 一 份  K  (2) eviction 路 径 不 再 调  hash/==/K-copy
        // 前 提 ： map 在 生 命 周 期 内 绝 不 能  rehash（ 否 则  iterator 失 效  → UB）
        typename std::unordered_map<K, int32_t>::iterator map_it;
        std::shared_ptr<V> value;
        int32_t next, prev;
        Node(int32_t nxt, int32_t prv) noexcept
            : map_it{}, value(nullptr), next(nxt), prev(prv) {}
    };
    std::vector<Node> pool;
    std::unordered_map<K, int32_t> key2idx;
    int32_t _head_free;
    int32_t _capacity;
            const int32_t used_head_sentinel_;
            const int32_t used_tail_sentinel_;
            static constexpr int32_t free_tail_sentinel_ = -1;

public:
    explicit lrucache_v2(int32_t capacity)
        : _capacity(capacity), _head_free(0),
          used_head_sentinel_(capacity), used_tail_sentinel_(capacity + 1)
    {
        // abort 改 成  throw， 让 调 用 方 能  catch、 其 它 对 象 能 正 常 析 构
        if (capacity <= 0) {
            throw std::invalid_argument("lrucache_v2: capacity must be > 0");
        }
        // ← 关 键 ： 预 分 配  bucket 锁 死  iterator 稳 定 性
        // reserve(n) 保 证 之 后  ≤ n 次  insert 都 不 会  rehash
        key2idx.reserve(static_cast<size_t>(capacity));

        pool.reserve(static_cast<size_t>(capacity + 2));
        for (int32_t i = 0; i < capacity; ++i) {
            pool.emplace_back(i + 1, i - 1);
        }
        pool[capacity - 1].next = free_tail_sentinel_;
        pool.emplace_back(used_tail_sentinel_, used_head_sentinel_);
        pool.emplace_back(used_tail_sentinel_, used_head_sentinel_);
    }

    // basic guarantee
    // 抛 点 只 有  try_emplace 的  bad_alloc； 抛 了 的 话  cache 处 于 "少 一 条 数 据 但 自 洽 "的 状 态
    void push(const K& key, std::shared_ptr<V> value) {
        auto it = key2idx.find(key);
        if (it != key2idx.end()) {
            pool[it->second].value = std::move(value);
            moveToHead(it->second);
            return;
        }

        if (_head_free == free_tail_sentinel_) {
            int32_t tail = pool[used_tail_sentinel_].prev;
            eraseByIdx(tail);  // 全  noexcept（ 见 下 ）
        }

        int32_t addPage = _head_free;

        // 唯 一 抛 点 ： try_emplace 可 能  bad_alloc
        // try_emplace 比  operator[] 好 ： 不 需 要  K 默 认 构 造  + 多 余 的 赋 值
        auto [new_it, ok] = key2idx.try_emplace(key, addPage);
        // ok 一 定  true（ 前 面  find 已 确 认 不 存 在 ）

        // ↓ 以 下 全 部  noexcept： iterator 已 拿 到 ， map 已 更 新 成 功
        _head_free = pool[_head_free].next;
        pool[addPage].map_it = new_it;
        pool[addPage].value = std::move(value);
        insertAtHead(addPage);
    }

    // strong guarantee（ key2idx.erase by iterator 是  noexcept）
    void erase(const K& key) {
        auto it = key2idx.find(key);
        if (it != key2idx.end()) {
            eraseByIdx(it->second);
        }
    }

private:
    // 全 程  noexcept： iterator erase 不 调  hash/==
    void eraseByIdx(int32_t idx) noexcept {
        key2idx.erase(pool[idx].map_it);   // ← 关 键 ： 用  iterator， 不 用  key
        // unlink from used list
        pool[pool[idx].next].prev = pool[idx].prev;
        pool[pool[idx].prev].next = pool[idx].next;
        pool[idx].value.reset();
        pool[idx].next = _head_free;
        _head_free = idx;
    }

    // moveToHead / insertAtHead 跟 原 版 一 样 ， 纯 指 针 ， 加  noexcept
    void insertAtHead(int32_t addPage) noexcept{
                // addPage变为新头
                pool[pool[used_head_sentinel_].next].prev = addPage; // 原来的头往前指向addPage
                pool[addPage].next = pool[used_head_sentinel_].next; // addPage往后指向原来头
                pool[used_head_sentinel_].next = addPage; // 哨兵头往后指向addpage
                pool[addPage].prev = used_head_sentinel_; // addPage往前指向哨兵头
            }
            void moveToHead(int32_t addPage) noexcept{
                // 断开addPage两端
                pool[pool[addPage].next].prev = pool[addPage].prev;
                pool[pool[addPage].prev].next = pool[addPage].next;
                // addPage变为新头
                insertAtHead(addPage);
            }
};
// ```

// ## 这 份 范 例 想 让 你 看 到 的 几 个 点

// 1. **`Node::key` → `Node::map_it`**： slot 里 不 再 独 立 持 有  K， 只 持 有 "指 向  map 那 份  K 的 句 柄 "。
// 所 有 权 清 晰 ： **map 拥 有  K， Node 是 非 拥 有 引 用 **。
// 2. **`key2idx.reserve(capacity)`**： 把  iterator 稳 定 性 从 "靠 运 气 "变 成 "靠 数 据 结 构 契 约 "。 这 是 整
// 个 改 动 能 成 立 的 前 提 ， 不 能 漏 。
// 3. **`try_emplace` 替 代  `operator[] = ...`**：
//    - 不 需 要  K 默 认 构 造 （ 你  `static_assert` 那 条 约 束 以 后 可 以 删 ）
//    - 不 会 做 "先 默 认 构 造 再 赋 值 "两 步 ， 性 能 也 更 好
//    - 返 回  `<iterator, bool>`， 让 你 直 接 拿 到  iterator
// 4. **`eraseByIdx` 全  noexcept**： 用  iterator erase 完 全 绕 开 了  hash/==， 对 通 用  K 也 安 全 。
// 5. **`abort()` 改  `throw std::invalid_argument`**： API 礼 貌 问 题 ， 独 立 于 异 常 安 全 。
// 6. **每 个 改 动 点 上 方 都 有 一 行 注 释 说 明 "为 什 么 这 么 写 "**： 将 来 你 回 来 看 代 码 不 用 重 新 推 一 遍 。

// ## 给 你 做 的 时 候 的 几 个 小 提 醒

// - **不 要 直 接 整 段 贴 **， 对 照 原 来 的  `lru_base.hpp` 一 段 一 段 改 ， 每 改 完 一 段  build + 跑 下 既 有 测 试
// 。 这 样 出 问 题 能 定 位 到 具 体 段 。
// - **iterator 稳 定 性 是 隐 含 契 约 **， 将 来 你 想 给  `key2idx` 加 更 多  entry（ 比 如  prefix 索 引 ） 、 或 者
// 改  `_capacity` 动 态 扩 缩 容 ， **reserve 那 一 行 就 保 护 不 住 了 **。 在  `key2idx` 那 个 字 段 旁 边 加 个 注 释
// 提 醒 未 来 的 自 己 。
// - 改 完 之 后  `static_assert(std::is_default_constructible_v<K>, ...)` 那 行 可 以 删 了  —— 用  itera
// tor 不 再 需 要  K 默 认 构 造 。
// - 跑 测 试 时 特 别 关 注 ： **多 次  evict → 重 新  push 同 样 的  key**， 确 保  iterator 在 多 次  erase/insert
//  后 仍 然 指 向 正 确 的  entry。