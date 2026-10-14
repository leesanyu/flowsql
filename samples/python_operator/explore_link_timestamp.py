# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.

"""为 link_data_2_month.csv 生成 Baseliner 输入。

通过 Web「算子管理 → Python 算子」上传并激活本文件；原始 CSV 导入为
dataframe.link_data_2_month 后执行：

    SELECT * FROM dataframe.link_data_2_month
    USING explore.link_timestamp
    INTO dataframe.link_data_2_month_baseliner;

契约：time 为非空文本，格式为 %Y/%m/%d %H:%M，按 Asia/Shanghai 解释。
保留原列及行序，生成 time_s:int64（Unix 秒）；缺少 link 时添加 link_1。
再以 time_s/unit=s、bucket_seconds=60、series_keys=["link"] 调用 Baseliner。
"""

import polars as pl

from flowsql import OperatorBase, register_operator


@register_operator(
    category="explore",
    name="link_timestamp",
    description="链路 CSV 时间按上海时区转换为 Unix 秒，并补充单链路身份",
)
class LinkTimestampOperator(OperatorBase):
    def work(self, df_in: pl.DataFrame) -> pl.DataFrame:
        if "time" not in df_in.columns:
            raise ValueError("缺少 time 列")
        if df_in.schema["time"] != pl.String:
            raise ValueError("time 列必须是 utf8 文本")
        if df_in["time"].null_count():
            raise ValueError("time 列不能包含空值")

        try:
            df_out = df_in.with_columns(
                pl.col("time")
                .str.strptime(pl.Datetime, format="%Y/%m/%d %H:%M", strict=True, exact=True)
                .dt.replace_time_zone("Asia/Shanghai")
                .dt.epoch("s")
                .alias("time_s")
            )
        except pl.exceptions.PolarsError as exc:
            raise ValueError("time 列必须是有效日期，格式为 YYYY/M/D H:MM") from exc

        if "link" not in df_out.columns:
            df_out = df_out.with_columns(pl.lit("link_1").alias("link"))
        return df_out
