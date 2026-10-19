# 产品版本与构建信息

FlowSQL 使用仓库根目录 `VERSION` 统一维护产品版本，当前开发基线为 `0.1.0`。版本号登记或构建成功
不等于正式发布；发布还需要干净源码、对应标签、实际产物一致及完整功能验收。

## 维护版本

`VERSION` 为 UTF-8 单行，不带 `v`。支持 `MAJOR.MINOR.PATCH`，以及 `-dev.N`、`-alpha.N`、`-beta.N`、
`-rc.N` 预发布后缀；数字不能有前导零，不接受 `+` 元数据。例如：

| 版本 | 用途 |
| --- | --- |
| `0.1.0-dev.1` | 开发版本 |
| `0.1.0-rc.1` | 发布候选 |
| `0.1.0` | 当前交付基线 |
| `0.1.1` | 兼容修复 |
| `0.2.0` | 新能力或 0.x 阶段的适配性调整 |

1.0.0 起确立稳定兼容承诺，不兼容变更递增主版本。日常提交由源码提交号区分，不自动递增版本。
前端 `package.json` 的包版本不参与产品展示，构建不会改写它或 lockfile。Python 分发版本从同一来源
映射为 PEP 440：`0.1.0-dev.1` → `0.1.0.dev1`，alpha/beta/rc 分别为 `a1`/`b1`/`rc1`；运行时保留原始产品版本。

## 构建与查询

标准入口不变：

```bash
cmake -B build src
cmake --build build -j$(nproc)
build/output/flowsql --version
build/output/flowsql-worker --version
PYTHONPATH=build/output/python python3 -m flowsql.worker --version
```

CLI 成功退出 0，输出三行，例如：

```text
FlowSQL 0.1.0
revision=0123456789abcdef0123456789abcdef01234567
dirty=false
```

查询无需启动服务、加载插件、读取部署配置或连接外部系统。Worker 查询不导入算子与第三方运行库。
Web 顶栏和侧栏显示前端自身的 `v<version>`，悬浮详情显示短提交及修改状态；后端不可达时仍可显示。

服务启动后可查询当前 Web 插件身份。以下端口是默认 Web 直接入口，Router 入口也支持同一路径：

```bash
curl -i http://127.0.0.1:8081/api/version
```

返回 HTTP 200、`Content-Type: application/json`、`Cache-Control: no-store`，固定三字段：

```json
{"version":"0.1.0","revision":"0123456789abcdef0123456789abcdef01234567","dirty":false}
```

`revision` 是构建时完整 Git commit ID；`dirty` 包括暂存、未暂存和未忽略的新文件，忽略的构建缓存与证据不计入。
无 Git 元数据的源码包允许构建，`revision`/`dirty` 为 `null`，CLI 显示 `unknown`，Web 显示未知。
已有产物报告内置信息，之后更改源码或环境变量不会重标旧产物。Web API 仅代表该 Web 插件，前端版本仅代表
当前静态资源，均不能证明整个集群已升级。`/api/health` 保持原响应。

普通增量构建刷新版本、提交和修改状态；无变化不重写生成文件，不重复 `npm ci` 或构建第三方依赖。
构建期间应保持源码稳定。独立前端 `npm run dev/build` 从同一 `VERSION` 读取身份，不必先运行 CMake；
开发服务监测 `VERSION` 修改。原生启动器使用构建准备的解释器，跨机器交付还需要相应运行库和 Python 环境。

## 核对产物与制作镜像

构建目录携带 `build/output/build-info.json`，主程序、所有产品共享库、Worker 与前端也内置同一身份。
复制原生交付目录时保留 `python/`、`static/` 及清单；打包前运行：

```bash
python3 tools/release.py artifacts
```

它读取实际产物，拒绝缺失身份、旧产物混用、缺失前端入口或快照不一致。只改清单不能替代重新构建。

本地制作镜像：

```bash
python3 tools/release.py image --tag flowsql:0.1.0
docker run --rm flowsql:0.1.0 flowsql --version
docker run --rm flowsql:0.1.0 flowsql-worker --version
docker image inspect flowsql:0.1.0 --format '{{json .Config.Labels}}'
```

该命令先核对实际产物与当前源码，再把产物版本、提交号传给 Dockerfile。镜像会再次校验输入，并将相同值写入
`org.opencontainers.image.version` / `org.opencontainers.image.revision`；未知 revision 的开发镜像标签为 `unknown`。
Dockerfile 复制已经构建的 Worker，Python 依赖仍按 requirements 安装，不能用后来修改的源码替代旧 Worker。
直接 `docker build` 必须传入与产物一致的 `PRODUCT_VERSION` 和 `PRODUCT_REVISION`，缺失或不符会失败。
制作开发镜像允许 `dirty=true`，不会自动上传镜像。

Compose 用同一个 `FLOWSQL_IMAGE` 选择四个产品服务镜像，默认 `flowsql:latest`：

```bash
FLOWSQL_IMAGE=flowsql:0.1.0 docker compose config --images
FLOWSQL_IMAGE=flowsql:0.1.0 docker compose up -d
```

也可在部署目录 `.env` 中设置 `FLOWSQL_IMAGE=registry.example.com/flowsql:0.1.0`。镜像选择不改变镜像内版本；
`latest` 只是部署别名。实际部署、推送或上传由操作者明确执行。

## 正式与候选发布

发布顺序为：修改并提交版本 → 标准构建及完整验收 → 明确创建相应标签 → 只读发布校验 → 交付。
标签格式为 `v<VERSION>`，例如 `v0.1.0` 或 `v0.1.0-rc.1`。发布校验命令：

```bash
python3 tools/release.py verify --version 0.1.0
```

它要求 `VERSION`、请求版本、当前提交上的精确标签和全部实际产物一致，源码干净且身份已知；缺标签、标签指向
其他提交、有本地修改、未知身份或混合产物均非零退出。标签可为 lightweight 或 annotated tag，只比较解析后的提交。
工具不会修改版本、提交、创建标签、修复产物、推送或上传。失败时修复对应原因并重新构建验收，不能覆盖清单绕过。
标签创建与推送命令由发布负责人明确执行，校验成功仍不能替代产品功能验收或供应链签名。
