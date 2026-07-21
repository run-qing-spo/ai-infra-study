# kv_uring_tier — 把 io_uring 磁盘引擎注册为 vLLM 的 secondary tier 类型 "uring"。
#
# 注册走 vLLM 的 general plugin 机制:pyproject.toml 里声明了
# entry point (vllm.general_plugins → register), vLLM 每个进程启动时
# 会自动调用 register(), 不需要改 vLLM 一行代码。

import logging

logger = logging.getLogger(__name__)


def register() -> None:
    try:
        from vllm.v1.kv_offload.tiering.factory import SecondaryTierFactory
    except ImportError:
        # 老版本 vLLM 没有 tiering;装了本包但用不上, 静默跳过
        return
    try:
        SecondaryTierFactory.register_tier(
            "uring", "kv_uring_tier.manager", "UringSecondaryTierManager"
        )
        logger.info("Registered secondary tier type 'uring'")
    except ValueError:
        pass  # 已注册(plugin 被加载了多次), 幂等处理

    # 调度层 tier IO 计时埋点。只在设了 KVTIER_SCHED_LEDGER 时真装, 否则 no-op。
    # 借同一个 plugin 钩子 = fs 组也会装(plugin 与配哪个 tier 无关), 这样 fs
    # 才有 IO 账本可算占比。见 sched_ledger.py。
    try:
        from . import sched_ledger
        sched_ledger.maybe_install()
    except Exception as e:  # 埋点绝不能影响注册主流程
        logger.warning("sched_ledger 未装载(不影响 serve): %r", e)
