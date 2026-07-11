// pybind 绑定:把 KvTierEngine 暴露给 Python 侧的 UringSecondaryTierManager。
//
// 边界约定(谁持有什么):
//   - primary CPU 区域的 memoryview 由 vLLM 持有, 这里只借指针。
//     PyEngine 持一个 py::object 引用防止它先于引擎被 GC。
//   - slot/block_id → 字节偏移的换算在这里做(× block_bytes), Python 层
//     只讲"第几个 slot / 第几个 block", 不碰字节 —— 和 P4 的账本层一致。
//   - submit_* 返回 False = 引擎入口环满, Python 层按 job 失败处理。
//
// PoolEngine(C++ 线程池对照组, BENCH_ANALYSIS §4)接口与 Engine 完全同构,
// 包装逻辑用模板共享 —— bench 侧换个类名就能跑同一条代码路径, 保证对照里
// 唯一变量是引擎内部的提交模型。

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "kv_tier_engine.hpp"
#include "pool_tier_engine.hpp"

namespace py = pybind11;

namespace {

template <class EngineT>
class PyEngine {
public:
    // depth 的语义随引擎走:KvTierEngine 是 queue_depth(io_uring SQ/CQ 深度),
    // PoolTierEngine 是 num_threads(线程池大小)。绑定处给不同的参数名。
    PyEngine(py::buffer primary_view, const std::string& path,
             uint64_t num_slots, uint64_t block_bytes,
             size_t depth, bool use_odirect, bool prewarm)
        : keepalive_(primary_view) {
        py::buffer_info info = primary_view.request(/*writable=*/true);
        size_t mem_bytes = static_cast<size_t>(info.size) * info.itemsize;
        block_bytes_ = block_bytes;
        num_slots_   = num_slots;
        {
            // prewarm=true 时构造要真写整个文件(20G 约 9s), 期间放掉 GIL,
            // 别让其他 Python 线程陪着等一次磁盘预写。
            py::gil_scoped_release nogil;
            engine_ = std::make_unique<EngineT>(
                path, num_slots * block_bytes, info.ptr, mem_bytes,
                static_cast<uint32_t>(block_bytes), depth, use_odirect,
                prewarm);
        }
        mem_bytes_ = mem_bytes;
    }

    bool submit(uint64_t job_id, bool is_write,
                const std::vector<uint64_t>& disk_slots,
                const std::vector<uint64_t>& mem_block_ids) {
        if (disk_slots.size() != mem_block_ids.size()) {
            throw std::invalid_argument("disk_slots / mem_block_ids length mismatch");
        }
        p5::JobDesc jd;
        jd.job_id   = job_id;
        jd.is_write = is_write;
        jd.xfers.reserve(disk_slots.size());
        for (size_t i = 0; i < disk_slots.size(); ++i) {
            if (disk_slots[i] >= num_slots_) {
                throw std::out_of_range("disk slot out of range");
            }
            uint64_t moff = mem_block_ids[i] * block_bytes_;
            if (moff + block_bytes_ > mem_bytes_) {
                throw std::out_of_range("mem block_id out of range");
            }
            jd.xfers.push_back({disk_slots[i] * block_bytes_, moff});
        }
        return engine_->submit(std::move(jd));
    }

    std::vector<std::pair<uint64_t, bool>> poll_finished(size_t max_n) {
        std::vector<p5::JobResult> res;
        engine_->poll(res, max_n);
        std::vector<std::pair<uint64_t, bool>> out;
        out.reserve(res.size());
        for (const auto& r : res) out.emplace_back(r.job_id, r.success);
        return out;
    }

    void drain() { engine_->drain(); }

    py::dict stats() const {
        auto s = engine_->stats();
        py::dict d;
        d["jobs_submitted"] = s.jobs_submitted;
        d["jobs_completed"] = s.jobs_completed;
        d["jobs_failed"]    = s.jobs_failed;
        d["ops_completed"]  = s.ops_completed;
        d["bytes_written"]  = s.bytes_written;
        d["bytes_read"]     = s.bytes_read;
        d["submit_calls"]   = s.submit_calls;
        d["sq_full_events"] = s.sq_full_events;
        return d;
    }

private:
    py::object keepalive_;   // 防 primary view 先死
    std::unique_ptr<EngineT> engine_;
    uint64_t block_bytes_ = 0;
    uint64_t num_slots_   = 0;
    size_t   mem_bytes_   = 0;
};

// 两个引擎绑定同一套方法, 只有类名和 depth 参数的名字/默认值不同
template <class EngineT>
void bind_engine(py::module_& m, const char* py_name,
                 const char* depth_arg, size_t depth_default) {
    using E = PyEngine<EngineT>;
    py::class_<E>(m, py_name)
        .def(py::init<py::buffer, const std::string&, uint64_t, uint64_t, size_t, bool, bool>(),
             py::arg("primary_view"), py::arg("path"),
             py::arg("num_slots"), py::arg("block_bytes"),
             py::arg(depth_arg) = depth_default, py::arg("use_odirect") = true,
             // 默认 false:微基准的预写协议不变(dd 或不预写都由 bench 定);
             // e2e 的 manager 侧默认 true(首写 punt 别混进实验数据)
             py::arg("prewarm") = false)
        .def("submit_store",
             [](E& e, uint64_t job_id,
                const std::vector<uint64_t>& slots,
                const std::vector<uint64_t>& bids) {
                 return e.submit(job_id, /*is_write=*/true, slots, bids);
             },
             py::arg("job_id"), py::arg("disk_slots"), py::arg("mem_block_ids"))
        .def("submit_load",
             [](E& e, uint64_t job_id,
                const std::vector<uint64_t>& slots,
                const std::vector<uint64_t>& bids) {
                 return e.submit(job_id, /*is_write=*/false, slots, bids);
             },
             py::arg("job_id"), py::arg("disk_slots"), py::arg("mem_block_ids"))
        .def("poll_finished", &E::poll_finished, py::arg("max_n") = 1024)
        // drain 会长时间阻塞, 必须放 GIL, 否则把整个 scheduler 进程卡死
        .def("drain", &E::drain, py::call_guard<py::gil_scoped_release>())
        .def("stats", &E::stats);
}

} // namespace

PYBIND11_MODULE(_kvtier, m) {
    m.doc() = "io_uring + O_DIRECT KV tier engine (project2 SPSC + project3 uring + project4 slab)";

    // 默认 QD=32:微基准 sweep 的 sweet spot(BENCH_ANALYSIS §2/§5);
    // 512 在 md 机器上让 io-wq 坍缩到 1 worker, dm 机器上也无收益。
    // gather threshold 在引擎内随 QD 联动(min(32, QD/2)), 低 QD 不会 lockstep。
    bind_engine<p5::KvTierEngine>(m, "Engine", "queue_depth", 32);
    // C++ 线程池对照组:拆 "C++ 加成" 和 "io_uring 加成"(BENCH_ANALYSIS §4)
    bind_engine<p5::PoolTierEngine>(m, "PoolEngine", "num_threads", 32);
}
