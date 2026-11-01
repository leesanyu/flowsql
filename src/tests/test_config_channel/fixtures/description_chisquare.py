# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
# Representative existing chisquare algorithm with an author declaration for the upload workflow.
import pandas as pd
import polars as pl
from scipy.stats import chi2_contingency
from flowsql.operator_base import OperatorBase, register_operator


@register_operator(
    data_contract={
        "schema_id": "flowsql.operator-data-contract.v1",
        "title": "卡方独立性检验",
        "operator": {
            "category": "explore",
            "name": "chisquare",
            "version": "1"
        },
        "configuration": {
            "invocation_schema": {
                "type": "object",
                "properties": {
                    "col1": {
                        "type": "string",
                        "default": "",
                        "description": "分类字段；两项未同时填写时按算法原逻辑选择前两个分类列"
                    },
                    "col2": {
                        "type": "string",
                        "default": "",
                        "description": "分类字段；两项未同时填写时按算法原逻辑选择前两个分类列"
                    }
                },
                "additionalProperties": False
            },
            "affects": {
                "col1": [
                    "#/inputs",
                    "#/outputs"
                ],
                "col2": [
                    "#/inputs",
                    "#/outputs"
                ]
            }
        },
        "inputs": [
            {
                "port_id": "input",
                "min_count": 1,
                "max_count": 1,
                "shape": "dataframe",
                "roles": [
                    {
                        "role_id": "col1",
                        "required": False,
                        "types": [],
                        "nullable": None,
                        "semantic_requirements": {},
                        "selector": {
                            "kind": "parameter",
                            "key": "col1"
                        }
                    },
                    {
                        "role_id": "col2",
                        "required": False,
                        "types": [],
                        "nullable": None,
                        "semantic_requirements": {},
                        "selector": {
                            "kind": "parameter",
                            "key": "col2"
                        }
                    }
                ]
            }
        ],
        "outputs": [
            {
                "port_id": "results",
                "shape": "dataframe",
                "mode": "unknown",
                "rules": [],
                "structure_ref": "#/resources/results"
            }
        ],
        "resources": {
            "results": {
                "type": "object",
                "description": "汇总行与列联表明细混合。首行为汇总：col1/col2 是选择的字段名，统计量仅此行有效，observed/expected 和动态分类字段为 NULL；后续为明细：动态字段名来自输入，observed/expected 为频数，汇总字段为 NULL。明确的缺列/不足两列输入返回仅含 error 的一行；其他统计错误仍可能抛出。pandas 混合行可能把 dof/observed 物理编码为浮点数，数值含义仍为计数/自由度。",
                "properties": {
                    "col1": {
                        "type": [
                            "string",
                            "null"
                        ],
                        "description": "仅汇总行：第一个分类字段的名称。"
                    },
                    "col2": {
                        "type": [
                            "string",
                            "null"
                        ],
                        "description": "仅汇总行：第二个分类字段的名称。"
                    },
                    "chi2": {
                        "type": [
                            "number",
                            "null"
                        ]
                    },
                    "p_value": {
                        "type": [
                            "number",
                            "null"
                        ]
                    },
                    "dof": {
                        "type": [
                            "integer",
                            "null"
                        ]
                    },
                    "observed": {
                        "type": [
                            "number",
                            "null"
                        ]
                    },
                    "expected": {
                        "type": [
                            "number",
                            "null"
                        ]
                    },
                    "significant_0.05": {
                        "type": [
                            "string",
                            "null"
                        ]
                    },
                    "error": {
                        "type": [
                            "string",
                            "null"
                        ]
                    }
                },
                "additionalProperties": True
            }
        },
        "limitations": [
            "检验两个分类变量的独立性；不能据此断言因果关系。",
            "未同时指定 col1/col2 时，取前两个 object/category/int64/int32 列；至少需要两个分类列。",
            "汇总与明细混合；分类列名来自输入，不承诺固定输出字段集合。",
            "预检只检查元数据，不运行卡方检验或验证频数适用条件。",
            "Python Worker 传入单个 Polars DataFrame；组件内部转 pandas，输出经 Arrow IPC 编码。卡方适用条件、缺失值处理及动态字段名冲突不由预检保证；算法异常不保证转为 error 行。"
        ]
    },
    category="explore",
    name="chisquare",
    description="卡方独立性检验：对两个分类变量构建列联表并计算卡方统计量",
    position="DATA",
)
class ChiSquareOperator(OperatorBase):

  def work(self, df_in: pl.DataFrame) -> pd.DataFrame:
    df_in = df_in.to_pandas()
    col1 = self.get_config("col1", "")
    col2 = self.get_config("col2", "")

    if not col1 or not col2:
      # 未指定列名时，取前两列
      cols = df_in.select_dtypes(include=["object", "category", "int64", "int32"]).columns.tolist()
      if len(cols) < 2:
        return pd.DataFrame([{"error": "至少需要两个分类列，请通过 WITH col1='列名', col2='列名' 指定"}])
      col1, col2 = cols[0], cols[1]

    if col1 not in df_in.columns or col2 not in df_in.columns:
      return pd.DataFrame([{"error": f"列 '{col1}' 或 '{col2}' 不存在"}])

    # 构建列联表
    ct = pd.crosstab(df_in[col1], df_in[col2])

    # 卡方检验
    chi2, p, dof, expected = chi2_contingency(ct)

    # 汇总结果
    summary = pd.DataFrame([{
      "col1": col1,
      "col2": col2,
      "chi2": round(chi2, 6),
      "p_value": round(p, 6),
      "dof": int(dof),
      "significant_0.05": "yes" if p < 0.05 else "no",
    }])

    # 列联表明细（展开为长表）
    detail = ct.stack().reset_index()
    detail.columns = [col1, col2, "observed"]

    expected_df = pd.DataFrame(expected, index=ct.index, columns=ct.columns)
    expected_long = expected_df.stack().reset_index()
    expected_long.columns = [col1, col2, "expected"]

    detail = detail.merge(expected_long, on=[col1, col2])
    detail["expected"] = detail["expected"].round(4)

    # 合并：汇总行 + 明细行
    return pd.concat([summary, detail], ignore_index=True)
