"""算子基类 — 所有 Python 算子继承此类"""

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Dict
from contextvars import ContextVar
import copy
import json

import polars as pl


@dataclass
class OperatorAttribute:
    """算子元数据"""
    category: str = ""
    name: str = ""
    description: str = ""
    position: str = "DATA"  # "DATA" or "STORAGE"


class OperatorBase(ABC):
    """Python 算子基类"""

    def __init__(self):
        self._config: Dict[str, str] = {}

    def attribute(self) -> OperatorAttribute:
        """返回算子元数据（装饰器会自动注入实现）"""
        # 默认实现：从装饰器注入的属性获取
        if hasattr(self.__class__, '_decorator_attr'):
            return self.__class__._decorator_attr
        # 如果没有使用装饰器，子类必须覆盖此方法
        raise NotImplementedError("Must use @register_operator decorator or override attribute() method")

    @abstractmethod
    def work(self, df_in: pl.DataFrame) -> pl.DataFrame:
        """核心处理方法：接收输入 Polars DataFrame，返回输出 Polars DataFrame

        如需使用 pandas，可在算子内部调用 df_in.to_pandas() 转换"""
        ...

    def configure(self, key: str, value: str):
        """接收配置参数"""
        self._config[key] = value

    def get_config(self, key: str, default: str = "") -> str:
        return self._config.get(key, default)

    def resolve_invocation(self, inputs, execution_context):
        """Describe this configured instance without executing work or loading rows.

        Authors with parsed defaults override this method using the same production
        parser/state consumed by work. JSON Schema defaults never configure an instance.
        Return only non-secret effective values and exact configuration references.
        """
        return dict(effective_parameters=dict(self._config), config_refs=[], inputs=inputs)


# 装饰器注册表（模块级）
_registered_operators: Dict[str, type] = {}
_registration_target = ContextVar('flowsql_registration_target', default=None)


def register_operator(category: str, name: str, description: str = "", position: str = "DATA", data_contract=None):
    """装饰器：注册算子类

    用法:
        @register_operator(category="explore", name="chisquare", description="卡方分析")
        class ChiSquareOperator(OperatorBase):
            ...
    """
    def decorator(cls):
        if not issubclass(cls, OperatorBase):
            raise TypeError(f"{cls.__name__} must inherit from OperatorBase")

        key = f"{category}.{name}"
        declaration = copy.deepcopy(data_contract)
        if isinstance(declaration, str):
            declaration = {'contract_ref': declaration}
        if declaration is not None:
            if not isinstance(declaration, dict):
                raise TypeError('data_contract must be a JSON object or an exact reference')
            json.dumps(declaration, allow_nan=False)
        target = _registration_target.get()
        if target is None:
            target = _registered_operators
        if key in target and target[key].__module__ == cls.__module__:
            raise ValueError(f'duplicate operator {key}')
        target[key] = cls
        cls._data_contract = declaration

        # 注入默认 attribute 实现
        cls._decorator_attr = OperatorAttribute(
            category=category, name=name, description=description, position=position
        )

        # 提供 attribute 方法的默认实现
        def _attribute(self):
            return cls._decorator_attr
        cls.attribute = _attribute

        return cls
    return decorator


def get_registered_operators() -> Dict[str, type]:
    """获取通过装饰器注册的所有算子类"""
    return _registered_operators.copy()


def clear_registered_operators():
    """清空装饰器注册表（reload 前必须调用，否则已删除的算子会残留）"""
    _registered_operators.clear()
