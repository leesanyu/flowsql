# Feature: NPM 基础分析算子插件生命周期

状态：`[x]` 已完成（完整 CTest 13/13 通过）
优先级：P0
前置 Feature：`cpp-operators`、`stage-filter-pipeline`、`npm-basic-analysis`（均已完成）

## 业务意图

让按功能命名的 `npm.basic` C++ 算子遵循既有统一算子插件机制，通过 Web/API 完成上传、激活、任务租约、
去激活和重启恢复。一个 `.so` 是插件管理单元，可以导出一个或多个功能算子；
`IBlockTransformOperatorV1` 只描述 `npm.basic` 采用的数据与执行契约，不定义算子类型或插件类型。

当前 BinAddon 只支持返回 `IOperator*` 的 V1 ABI，而 `libflowsql_npm_basic.so` 通过静态 `pluginregist`
暴露 `IBlockTransformOperatorV1`，上传后因缺少 V1 工厂符号进入 `broken`。本 Feature 将统一 C++ 算子插件
ABI 扩展为可声明不同契约的 V2，并保持插件级原子生命周期与既有 packet → `npm.basic` → DataFrame
数据面不变。

## Non-Goals

- 不把 `block_transform` 增加为算子 `type`、`category` 或独立插件类别。
- 不为 `IBlockTransformOperatorV1` 建立专用 `operator_count`、工厂组、插件状态表或管理 API。
- 不修改 `npm.basic` 的会话、识别、预算、22 列 Schema、WITH 参数或 SQL 语义。
- 不修改 `IBlockTransformOperatorV1 / IBlockTransformTaskV1` V1 ABI，不让 Arrow packet 数据经过 HTTP。
- 不破坏传统 `IOperator` 插件 V1 ABI、已上传插件或 `builtin | python | cpp` 类型集合。
- 不允许上传普通框架 `IPlugin`，不动态注册 Web、Scheduler、Channel 或外部监听入口。
- 不支持执行中强制卸载、单算子独立激活/去激活、同名覆盖或热升级。
- 不自动激活构建产物，不直接编辑 SQLite 修复当前 `broken` 记录，不混入实时采集或结果持久化。

## 核心模型与不变量

```text
插件管理单元                     功能算子                         数据/执行契约
plugin_id / 一个 .so       ->   category.name             ->   contract_iid
上传、激活、去激活、删除、恢复    SQL、Catalog、用户可见身份          宿主如何创建和调用能力
                                  例：npm.basic                  例：IID_BLOCK_TRANSFORM_OPERATOR_V1
```

- `.so/plugin_id` 是生命周期与原子回滚单位；同一插件内的算子不能部分发布、部分去激活或分别恢复。
- `category.name` 是功能算子的唯一身份；Catalog 继续使用 `(category, name)` 唯一键，`type` 固定为
  `builtin | python | cpp`，其中动态 C++ 算子为 `type=cpp`。
- `contract_iid` 是能力元数据和运行时类型校验依据，不参与功能命名、Catalog 唯一键或插件状态分类。
- 每个插件只有一份 `operator_count/operators` 清单，必须枚举该 `.so` 导出的全部功能算子；不得按
  `IOperator`、Block Transform 或其他契约拆出多套 count。
- 当前 `libflowsql_npm_basic.so` 只导出一个功能算子，因此激活态元数据为
  `operator_count=1`、`operators=["npm.basic"]`。

`npm.basic` 激活后的逻辑身份为：

```json
{
  "category": "npm",
  "name": "basic",
  "type": "cpp",
  "source": "cpp_plugin",
  "active": true,
  "plugin_id": "<sha256>",
  "contract": "block_transform_v1"
}
```

## 核心契约

### 通用 C++ 算子插件 ABI V2

V2 以一份索引空间描述一个 `.so` 内的全部功能算子。公共头文件
`src/framework/interfaces/cpp_operator_plugin_abi.h` 的数据契约为：

```cpp
struct CppOperatorDescriptorV2 {
    uint32_t struct_size;
    const char* category;
    const char* name;
    const char* description;
    Guid contract_iid;
};

extern "C" int flowsql_abi_version();
extern "C" int flowsql_operator_count();
extern "C" int flowsql_describe_operator(int index, CppOperatorDescriptorV2* descriptor);
extern "C" void* flowsql_create_operator_capability(int index, IQuerier* querier);
extern "C" void flowsql_destroy_operator_capability(int index, void* capability);
```

- `flowsql_abi_version()` 返回 V2；`flowsql_operator_count()` 必须大于 0，且是插件唯一的算子计数入口。
- 每个合法 index 必须给出非空 `category/name`、宿主支持的 `contract_iid`，并创建非空的对应能力。
- 同一 `.so` 可以让不同 index 声明不同契约；创建结果只能按该 index 的 `contract_iid` 解释和调用。
- 创建时传入当前进程 `IQuerier`，供能力解析进程内依赖；异常不得跨 `.so` 边界传播。
- 描述、ABI、契约、名称、依赖或创建中任一项失败，整个插件激活失败并全量回滚。
- 调用方零初始化描述符并设置 `struct_size`；空输出、过小结构或越界 index 必须失败。
  字符串归插件所有，在 `.so` 加载期间有效，宿主发布前复制；创建返回契约 IID 对应的接口指针，
  销毁使用同一 index，禁止契约专用的第二套枚举。

同一头文件也声明 Sprint 11 已交付的 V1 `flowsql_create_operator/flowsql_destroy_operator` 工厂契约。
宿主继续兼容 V1 四符号 ABI；`abi_version=1` 时按原 `IOperator` 工厂加载；
`abi_version=2` 时按上述描述符和能力工厂加载。不得要求 V1 插件补充 V2 符号，也不得为 V2 的每种契约
增加另一套枚举符号。

### 通用动态能力租约

`src/framework/interfaces/icpp_operator_plugin_registry.h` 定义动态发现接口，
按功能身份和契约 IID 获取能力，不暴露具体实现类或 `.so` 名：

```cpp
struct CppOperatorCapabilityLeaseV1 {
    void* capability = nullptr;
    std::shared_ptr<void> lifetime;
};

interface ICppOperatorPluginRegistryV1 {
    virtual ~ICppOperatorPluginRegistryV1() = default;
    virtual int Acquire(
        const char* category,
        const char* name,
        const Guid& contract_iid,
        CppOperatorCapabilityLeaseV1* lease) = 0;
};
```

- Scheduler 为 `npm.basic` 请求 `category=npm`、`name=basic`、
  `contract_iid=IID_BLOCK_TRANSFORM_OPERATOR_V1`，成功后才把 `capability` 解释为该接口。
- `lifetime` 同时固定能力对象、provider 与所属 `.so` 的 `dlopen` handle；失败必须清空整个 lease。
- Scheduler 从发现、Schema probe、`CreateTask()`、全部 `ProcessBlock / Flush / Cancel` 调用，直到
  `ReleaseTask()` 完成后始终持有同一 lease。
- 静态能力与动态能力的同名同契约总匹配数必须恰好为 1；冲突不得通过优先级静默选择。

## 主链路

### 上传、激活与执行

1. Web 保存 `.so`，BinAddon 以 SHA-256 生成 `plugin_id` 并写入 `uploaded`；上传阶段不执行 `dlopen`。
2. 激活按 `plugin_id` 执行 `dlopen`，选择 V1 或 V2 ABI；V2 用唯一 count 枚举全部描述符，完成契约、
   依赖、名称和 Catalog 冲突预检后，创建并发布全部能力。
3. 发布与持久状态以插件为一个提交边界；任一算子失败时撤销该插件已发布的全部能力和 Catalog 记录，
   `dlclose` 并将插件置为 `broken`，保留稳定的 `last_error`。
4. 成功时只维护一份插件摘要 `operator_count/operators`，并为每个功能算子写入 `type=cpp`、
   `plugin_id` 和诊断用 `contract` 元数据。
5. Scheduler 按 `category.name + contract_iid` 获取租约，执行既有 block-transform pipeline，
   `ReleaseTask()` 后释放租约。

### 去激活与重启恢复

1. 去激活先阻止该 `.so` 内所有算子的新 Acquire；任一算子仍有 lease/task 时返回
   `409 plugin is in use`，撤销阻止标记，整个插件保持 `activated`。
2. 全部租约释放后，一次撤下该插件的所有能力，将其全部 Catalog 记录设为 inactive，销毁能力/provider，
   最后 `dlclose`，插件状态改为 `deactivated`。
3. 删除只接受未激活的 `plugin_id`，并清理该插件文件、全部 Catalog 记录和唯一插件状态记录。
4. 重启只恢复 `activated` 插件；重新校验文件哈希、ABI、全部描述符、依赖和名称后原子发布。
   任一项失败只把目标插件置为 `broken`，不得留下部分能力或影响其他插件。
5. 原生单进程、Guardian 和 Docker 标准部署不再静态加载 `libflowsql_npm_basic.so`；构建仍生成产物，
   但上传并激活前 `npm.basic` 不可用。算子插件文件与元数据库在相关进程间共享并持久化。

## 原子任务

- `[x]` T0：纠正 Feature 命名，冻结“插件—功能算子—数据契约”三层模型、统一多算子枚举和插件级原子生命周期；不修改代码。
- `[x]` T1：冻结并测试通用 C++ 算子插件 ABI V2、描述符和能力租约；fixture 单 `.so` 至少导出两个功能算子，并验证 V1 兼容。
- `[x]` T2：BinAddon 接入 V2 多算子枚举、按 IID 创建能力、插件级全量发布/回滚、租约保护和重启恢复。
- `[x]` T3：Scheduler 以 `category.name + IID_BLOCK_TRANSFORM_OPERATOR_V1` 请求 `npm.basic`，并持有租约到 `ReleaseTask()` 完成。
- `[x]` T4：完成 `npm.basic` V2 构建产物、部署解除静态加载和真实插件生命周期 E2E。
  - `[x]` T4.1：`libflowsql_npm_basic.so` 导出唯一 V2 `npm.basic + IID_BLOCK_TRANSFORM_OPERATOR_V1`
    capability；`test_npm_basic` 验证描述符、依赖失败、执行主链路与销毁。
  - `[x]` T4.2：原生单进程与 Guardian 不再静态加载 `npm.basic`，但仍构建待上传 `.so`，并共享
    Catalog/BinAddon 元数据库与上传目录；`test_native_deploy_config` 通过。
  - `[x]` T4.3：Docker 不再静态加载 `npm.basic`，补齐 Builtin、Catalog、BinAddon 及跨容器共享、持久化
    路径；`test_docker_deploy_config` 与 Compose 配置校验通过。
  - `[x]` T4.4：完成上传、激活、详情、离线 SQL、执行中去激活 409、租约释放后去激活、激活/去激活
    重启恢复 E2E，并通过 API 删除隔离环境生成的 `broken` 记录后重新上传新构建产物。

## Feature 验收

- 多算子 fixture 的同一 `.so` 只报告一份 `operator_count/operators`，至少两个不同功能算子可分别按各自
  `category.name + contract_iid` 获取；任一项失败时整插件无部分发布。
- 新构建的 `libflowsql_npm_basic.so` 上传后不再报 `missing required symbols`，激活详情显示
  `status=activated`、`operator_count=1`、`operators=["npm.basic"]`，且 `operator_details` 包含
  `{"name":"npm.basic","contract":"block_transform_v1"}`。
- 激活前 SQL 明确找不到 `npm.basic`；激活后离线 SQL 产生固定 22 列结果；去激活后再次不可用。
- 执行中去激活返回 409 且任务正常结束；租约释放后整插件去激活成功，无 capability/provider 跨
  `dlclose` 使用。
- 已激活插件可在重启后原子恢复，已去激活插件不恢复；坏插件不形成部分注册，V1 C++ 插件不受影响。
- 原生、Guardian、Docker 不静态加载 `npm.basic`，并具备共享、持久的 BinAddon 文件与状态路径。
- ABI/宿主、`npm.basic`、Scheduler E2E、原生/Docker 部署测试、完整构建和全部 CTest 通过。

Feature 完成时至少执行：

```bash
cmake -B build src
cmake --build build --target flowsql_binaddon flowsql_npm_basic \
  test_builtin test_npm_basic test_scheduler_e2e \
  test_native_deploy_config test_docker_deploy_config -j$(nproc)
ctest --test-dir build \
  -R '^(test_builtin|test_npm_basic|test_scheduler_e2e|test_native_deploy_config|test_docker_deploy_config)$' \
  --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```
