FROM ubuntu:22.04 AS runtime

# 安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-pip \
    libssl3 \
    libmysqlclient21 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Python 依赖（Python Worker）
RUN pip3 install --no-cache-dir fastapi uvicorn pyarrow

WORKDIR /opt/flowsql

# 复制构建产物
COPY build/output/flowsql          ./bin/flowsql
COPY build/output/lib*.so          ./bin/
COPY build/output/static/          ./bin/static/
COPY config/                       ./config/

# Python Worker 源码（如果存在）
COPY src/python/                   ./python/

ENV PATH="/opt/flowsql/bin:${PATH}"
ENV PYTHONPATH="/opt/flowsql/python"
ENV LD_LIBRARY_PATH="/opt/flowsql/bin"

# 默认工作目录（插件和配置文件相对路径基准）
WORKDIR /opt/flowsql/bin
