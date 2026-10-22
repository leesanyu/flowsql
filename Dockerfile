FROM ubuntu:24.04 AS runtime

# 安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-venv \
    libssl3t64 \
    libmysqlclient21 \
    libpq5 \
    librte-acl24 \
    librte-eal24 \
    libpcap0.8t64 libxdp1 libbpf1 \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/flowsql

# Python Worker 使用独立环境，Arrow 版本与当前 C++ 构建产物的 ABI 保持一致。
COPY src/python/requirements.txt ./python/requirements.txt
RUN python3 -m venv /opt/flowsql/venv \
    && /opt/flowsql/venv/bin/pip install --no-cache-dir "pyarrow==22.0.0" -r ./python/requirements.txt

# Kafka authentication runtime: shared libraries, dynamically loaded GSSAPI SASL module,
# private-ticket kinit tool and system CA. No development packages enter the runtime.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libcurl4t64 libsasl2-2 libsasl2-modules-gssapi-mit \
    libgssapi-krb5-2 libkrb5-3 krb5-user ca-certificates \
    zlib1g liblz4-1 libzstd1 libicu74 \
    && rm -rf /var/lib/apt/lists/*

# 复制构建产物
COPY build/output/python/ ./python/
COPY build/output/flowsql          ./bin/flowsql
COPY build/output/lib*.so*         ./bin/
COPY .thirdparts_installed/yaml-cpp/lib/libyaml-cpp.so.0.9.0 ./bin/libyaml-cpp.so.0.9
COPY build/output/static/          ./bin/static/
COPY build/output/exploration/     ./bin/exploration/
COPY build/output/build-info.json  ./bin/build-info.json
COPY tools/product_version.py tools/release.py ./tools/
ARG PRODUCT_VERSION
ARG PRODUCT_REVISION
LABEL org.opencontainers.image.version="${PRODUCT_VERSION}" \
      org.opencontainers.image.revision="${PRODUCT_REVISION}"
RUN python3 tools/release.py artifacts --output /opt/flowsql/bin \
    --worker /opt/flowsql/python/flowsql/_build_info.json \
    --expect-version "${PRODUCT_VERSION}" --expect-revision "${PRODUCT_REVISION}"
COPY config/                       ./config/
COPY build/output/share/kafka/ ./share/kafka/
COPY config/kafka/provider.example.json ./config/kafka/provider.example.json
COPY config/docker/flowsql.yml     ./config/flowsql.yml
COPY src/plugins/npi/conf/protocols.yml ./config/protocols.yml

# Versioned optional runtimes (PF_RING) are staged with the native build.
RUN ldconfig /opt/flowsql/bin

ENV PATH="/opt/flowsql/venv/bin:/opt/flowsql/bin:${PATH}"
ENV PYTHONPATH="/opt/flowsql/python"
ENV LD_LIBRARY_PATH="/opt/flowsql/bin:/opt/flowsql/venv/lib/python3.12/site-packages/pyarrow"
ENV FLOWSQL_KAFKA_STATE_DIR="/opt/flowsql/kafka-state"

RUN printf '#!/bin/sh\nexec /opt/flowsql/venv/bin/python -m flowsql.worker "$@"\n' \
    > /opt/flowsql/bin/flowsql-worker && chmod +x /opt/flowsql/bin/flowsql-worker

# 默认工作目录（插件和配置文件相对路径基准）
WORKDIR /opt/flowsql/bin
