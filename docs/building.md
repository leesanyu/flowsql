# 从源码构建 FlowSQL

当前自动依赖安装支持 **Ubuntu 24.04，Linux x86_64**。先安装 CMake 3.28 或更新版本；其余系统构建包由 CMake 在探测编译器之前检查。

在仓库根目录执行：

```bash
cmake -B build src
cmake --build build -j$(nproc)
./start.sh
```

首次配置和构建需要访问 Ubuntu 软件源、PyPI、npm，以及所选第三方源码下载站点。CMake 会列出缺失的系统包，只安装缺失项；依赖已满足时不运行 apt。普通用户需要可用的非交互 sudo 授权；可先在终端执行 `sudo -v`，再运行 CMake。无需使用 sudo 编译整个项目。权限不足、软件源失败、安装失败或安装后仍缺包，配置直接失败。

## 系统依赖

CMake 检查编译器与 make、pkg-config、patch、Git、curl、CA、Python 开发工具，以及以下库：

| 能力 | Ubuntu 开发包 |
| --- | --- |
| 加密与 Baseline | `libssl-dev libicu-dev` |
| 数据库驱动 | `libmysqlclient-dev libpq-dev` |
| Kafka 认证与压缩 | `libcurl4-openssl-dev libsasl2-dev libsasl2-modules-gssapi-mit libkrb5-dev krb5-user zlib1g-dev liblz4-dev libzstd-dev` |
| Hyperscan 源码编译 | `libboost-dev ragel` |
| AF_PACKET / AF_XDP 配套 | `libpcap-dev libxdp-dev libbpf-dev` |
| Flow Labeling | `libdpdk-dev`，默认要求安装并构建 |

Python 需要新建虚拟环境且缺少 `ensurepip` 时，另外检查并安装 `python3-venv`。

PF_RING Classic 不属于上述 Ubuntu 软件源自动安装范围。需要该采集后端时，先安装兼容的用户态库，再使用 `-DFLOWSQL_PFRING_ROOT=/path/to/pfring`；内核模块和设备权限按 [NetAdapter 说明](netadapter.md)准备。没有 PF_RING 库时，该 provider 明确返回不可用。构建流程不安装内核模块、不修改网卡或系统网络。

## 构建选项

| 选项 | 默认值 | 行为 |
| --- | --- | --- |
| `FLOWSQL_INSTALL_SYSTEM_DEPS` | `ON` | Ubuntu 24.04 下安装缺失系统包；`OFF` 只检查，缺少必需包就报错 |
| `FLOWSQL_FLOW_LABELING` | `ON` | 必须有 DPDK 并生成插件，适用于默认部署 |
| `FLOWSQL_FLOW_LABELING=AUTO` | 显式选择 | 自动安装开启时准备 DPDK；关闭自动安装时允许缺 DPDK，并省略插件；测试构建不强制依赖该插件目标 |
| `FLOWSQL_FLOW_LABELING=OFF` | 显式选择 | 不要求 DPDK，不生成 Flow Labeling 插件；相关功能回归仍需要具备该插件的完整构建 |
| `BUILD_TESTING` | `ON` | 构建并注册测试；`OFF` 不加入测试目录，仍构建完整的所选产品组件 |

已有受管系统依赖或不允许修改系统包时：

```bash
cmake -B build src -DFLOWSQL_INSTALL_SYSTEM_DEPS=OFF
cmake --build build -j$(nproc)
```

其他 Linux 发行版需手动安装等价依赖并关闭自动安装；这不代表已经完成该发行版的兼容性验收。显式关闭 Flow Labeling 或使用缺少 DPDK 的 AUTO 构建时，应自行提供不加载该插件的部署配置，且不能作为默认产品的完整能力构建。

## Python、Arrow 与前端

C++ 使用 **PyArrow 22.0.0** 提供的 Arrow 头文件、`libarrow.so.2200` 和 `libarrow_compute.so.2200`。Python Worker 与 Docker 使用同一个版本，避免构建与运行时 ABI 不一致。

CMake 通过选定的 Python 3.10+ 解释器查找 PyArrow，不依赖固定的 Python 安装路径。若该解释器已满足 `src/python/requirements.txt`，直接复用；否则在主工作区的 `.thirdparts_installed/python-arrow22-<Python版本>/` 建立私有虚拟环境并安装依赖，不向系统 Python 执行 pip install。源码版 Arrow 18 的旧回退路径已移除。

前端需要 Node.js 20.19+（20.x）或 22.12+ 和 npm。可用时复用现有工具；否则下载官方 Node 22.14.0 Linux 二进制，按官方 SHA256 清单校验，安装到共享依赖缓存。CMake 使用 `package-lock.json` 执行 `npm ci`，通过 `flowsql_frontend` 目标直接生成 `build/output/static`。前端源文件或 lockfile 改动后，再执行普通构建即可更新资源，无需重新链接 Web 插件。

## 产物与运行

`build/output` 包含 C++ 程序与插件、`static/`、`config/`、Worker 源码 `python/flowsql/` 和 `flowsql-worker` 启动器。构建生成的单进程、多进程配置使用该启动器，选择已经准备好的 Python 环境；原始配置模板保留 `python3 -m flowsql.worker`，供自管 Python 的部署使用。

```bash
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config config/deploy-multi.yaml
```

`flowsql.yml` 仍仅在目标不存在时初始化，不覆盖已有用户通道配置。原生构建产物会引用构建主机的共享依赖缓存或 Python 环境；跨机器发布使用 README 中的 Docker 镜像流程。Dockerfile 打包预构建产物，宿主机的标准构建需要先完成。

## 验证

系统包安装逻辑的隔离回归不会实际调用主机 apt；它覆盖缺包安装、重复执行、只检查模式、DPDK 可选模式、权限失败、apt 失败，以及安装后复查。

```bash
python3 src/tests/test_environment/test_product_build.py
./tools/test.sh unit --filter '^(test_product_build|test_native_deploy_config|test_docker_deploy_config|test_environment_config)$'
```

全新环境验收需要独立环境中无 `build/`、无第三方缓存、无前端 `dist`、无预装 PyArrow，从依赖准备开始执行标准构建，并验证默认部署启动。系统包安装逻辑、配置成功、已有缓存上的编译成功，分别只能证明相应阶段，不能单独替代全新环境完整验收。完整测试使用 [统一测试入口](testing.md)，环境跳过和未执行项不能计为通过。
