// pybind 绑定:把 KvTierEngine 暴露给 Python 侧的 UringSecondaryTierManager。
//
// 边界约定(谁持有什么):
//   - primary CPU 区域的 memoryview 由 vLLM 持有, 这里只借指针。
//     PyEngine 持一个 py::object 引用防止它先于引擎被 GC。
//   - slot/block_id → 字节偏移的换算在这里做(× block_bytes), Python 层
//     只讲"第几个 slot / 第几个 block", 不碰字节 —— 和 P4 的账本层一致。
//   - submit_* 返回 False = 引擎入口环满, Python 层按 job 失败处理。

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "kv_tier_engine.hpp"

namespace py = pybind11;

namespace {

class PyEngine {
public:
    PyEngine(py::buffer primary_view, const std::string& path,
             uint64_t num_slots, uint64_t block_bytes,
             size_t queue_depth, bool use_odirect)
        : keepalive_(primary_view) {
        py::buffer_info info = primary_view.request(/*writable=*/true);
        size_t mem_bytes = static_cast<size_t>(info.size) * info.itemsize;
        block_bytes_ = block_bytes;
        num_slots_   = num_slots;
        engine_ = std::make_unique<p5::KvTierEngine>(
            path, num_slots * block_bytes, info.ptr, mem_bytes,
            static_cast<uint32_t>(block_bytes), queue_depth, use_odirect);
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
    std::unique_ptr<p5::KvTierEngine> engine_;
    uint64_t block_bytes_ = 0;
    uint64_t num_slots_   = 0;
    size_t   mem_bytes_   = 0;
};

} // namespace

PYBIND11_MODULE(_kvtier, m) {
    m.doc() = "io_uring + O_DIRECT KV tier engine (project2 SPSC + project3 uring + project4 slab)";

    py::class_<PyEngine>(m, "Engine")
        .def(py::init<py::buffer, const std::string&, uint64_t, uint64_t, size_t, bool>(),
             py::arg("primary_view"), py::arg("path"),
             py::arg("num_slots"), py::arg("block_bytes"),
             py::arg("queue_depth") = 512, py::arg("use_odirect") = true)
        .def("submit_store",
             [](PyEngine& e, uint64_t job_id,
                const std::vector<uint64_t>& slots,
                const std::vector<uint64_t>& bids) {
                 return e.submit(job_id, /*is_write=*/true, slots, bids);
             },
             py::arg("job_id"), py::arg("disk_slots"), py::arg("mem_block_ids"))
        .def("submit_load",
             [](PyEngine& e, uint64_t job_id,
                const std::vector<uint64_t>& slots,
                const std::vector<uint64_t>& bids) {
                 return e.submit(job_id, /*is_write=*/false, slots, bids);
             },
             py::arg("job_id"), py::arg("disk_slots"), py::arg("mem_block_ids"))
        .def("poll_finished", &PyEngine::poll_finished, py::arg("max_n") = 1024)
        // drain 会长时间阻塞, 必须放 GIL, 否则把整个 scheduler 进程卡死
        .def("drain", &PyEngine::drain, py::call_guard<py::gil_scoped_release>())
        .def("stats", &PyEngine::stats);
}
