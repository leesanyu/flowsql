FROM ubuntu:24.04 AS runtime

# 安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-venv \
    libssl3t64 \
    libmysqlclient21 \
    libpq5 \
    librte-acl24 \
    librte-eal24 \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/flowsql

# Python Worker 使用独立环境，Arrow 版本与当前 C++ 构建产物的 ABI 保持一致。
COPY src/python/ ./python/
RUN python3 -m venv /opt/flowsql/venv \
    && /opt/flowsql/venv/bin/pip install --no-cache-dir "pyarrow==22.0.0" -r ./python/requirements.txt

# 复制构建产物
COPY build/output/flowsql          ./bin/flowsql
COPY build/output/lib*.so          ./bin/
COPY .thirdparts_installed/yaml-cpp/lib/libyaml-cpp.so.0.9.0 ./bin/libyaml-cpp.so.0.9
COPY build/output/static/          ./bin/static/
COPY config/                       ./config/
COPY config/docker/flowsql.yml     ./config/flowsql.yml
COPY src/plugins/npi/conf/protocols.yml ./config/protocols.yml

ENV PATH="/opt/flowsql/venv/bin:/opt/flowsql/bin:${PATH}"
ENV PYTHONPATH="/opt/flowsql/python"
ENV LD_LIBRARY_PATH="/opt/flowsql/bin:/opt/flowsql/venv/lib/python3.12/site-packages/pyarrow"

# 默认工作目录（插件和配置文件相对路径基准）
WORKDIR /opt/flowsql/bin
