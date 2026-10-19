"""FlowSQL Python Worker — 算子运行时框架"""

__all__ = ["OperatorBase", "register_operator", "OperatorRegistry"]


def __getattr__(name):
    # Keep metadata/--version usable without loading the operator runtime.
    if name in ("OperatorBase", "register_operator"):
        from . import operator_base
        return getattr(operator_base, name)
    if name == "OperatorRegistry":
        from .operator_registry import OperatorRegistry
        return OperatorRegistry
    raise AttributeError(name)
