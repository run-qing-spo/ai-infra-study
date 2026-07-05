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
