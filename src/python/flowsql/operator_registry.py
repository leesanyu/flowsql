"""算子注册中心 — 自动发现 + 装饰器注册"""

import importlib
import os
import sys
import traceback
from typing import Dict, List

from .operator_base import OperatorBase, OperatorAttribute, get_registered_operators, clear_registered_operators


class OperatorRegistry:
    """算子注册中心：管理所有已注册的 Python 算子实例"""

    def __init__(self):
        self._operators: Dict[str, OperatorBase] = {}

    def discover(self, operators_dir: str):
        """扫描目录，自动发现并注册算子

        1. 先导入目录下所有 .py 文件（触发 @register_operator 装饰器）
        2. 实例化所有通过装饰器注册的算子类
        3. 扫描所有已导入模块中的 OperatorBase 子类（辅助发现）
        """
        if not os.path.isdir(operators_dir):
            print(f"OperatorRegistry: operators_dir not found: {operators_dir}")
            return

        # 将目录加入 sys.path
        abs_dir = os.path.abspath(operators_dir)
        if abs_dir not in sys.path:
            sys.path.insert(0, abs_dir)

        # 导入目录下所有 .py 文件
        for filename in sorted(os.listdir(abs_dir)):
            if not filename.endswith('.py') or filename.startswith('_'):
                continue
            module_name = filename[:-3]
            try:
                # 如果模块已导入，使用 reload 获取最新代码（问题 6）
                if module_name in sys.modules:
                    importlib.reload(sys.modules[module_name])
                else:
                    importlib.import_module(module_name)
                print(f"OperatorRegistry: loaded module '{module_name}'")
            except Exception:
                # 导入失败不阻塞其他算子
                print(f"OperatorRegistry: failed to load '{module_name}':")
                traceback.print_exc()

        # 实例化装饰器注册的算子
        for key, cls in get_registered_operators().items():
            if key not in self._operators:
                try:
                    instance = cls()
                    self._operators[key] = instance
                    attr = instance.attribute()
                    print(f"OperatorRegistry: registered [{attr.category}.{attr.name}] "
                          f"({attr.description})")
                except Exception:
                    print(f"OperatorRegistry: failed to instantiate '{key}':")
                    traceback.print_exc()

    def reload(self, operators_dir: str):
        """重新加载算子：清空现有注册，重新发现（问题 6）"""
        self._operators.clear()
        clear_registered_operators()  # 清空模块级装饰器注册表，避免已删除算子残留
        self.discover(operators_dir)

    def get(self, category: str, name: str) -> OperatorBase | None:
        key = f"{category}.{name}"
        return self._operators.get(key)

    def list_operators(self) -> List[dict]:
        """返回所有已注册算子的元数据列表"""
        result = []
        for key, op in self._operators.items():
            attr = op.attribute()
            result.append({
                "category": attr.category,
                "name": attr.name,
                "description": attr.description,
                "position": attr.position,
            })
        return result

    def configure(self, category: str, name: str, key: str, value: str) -> bool:
        op = self.get(category, name)
        if op:
            op.configure(key, value)
            return True
        return False
