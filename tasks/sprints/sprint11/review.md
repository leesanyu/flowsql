# Sprint 11 评审记录

## 评审信息

- 评审日期：2026-03-28
- 评审范围：
  - Sprint 11 设计与实现落地（PostgreSQL 支持、算子类型统一、C++ 插件链路、前端算子页改造）
  - 本轮新增修复（Bridge 并发安全、前端静态产物体积治理第一阶段）
- 评审目标：
  - 功能完整性与可实施性
  - 并发/一致性风险识别
  - 未解决问题留痕与后续行动建议

## 本轮结论

1. Sprint 11 主体功能可运行，核心链路（`builtin/python/cpp` 三类算子、统一 `/operators/*`、PostgreSQL 通道类型）已贯通。
2. 本轮已补齐两个关键工程问题：
   - Bridge 并发读写数据竞争（已修复并有测试）。
   - 前端 `build/output/static` 历史产物堆积（已修复）。
3. 仍有若干中高优先级工程问题未处理（见“未解决问题”），其中 6 项为本轮已识别但暂缓实施项。

## 已解决问题

### 1. BridgePlugin 并发读写竞态（已解决）

- 问题：`FindOperator/TraverseOperators` 与 `Refresh` 并发时，旧实现存在无锁共享容器访问。
- 处理：
  - 用 `shared_mutex + unordered_map` 保护算子快照。
  - `DiscoverOperators` 改为“本地构建 + Catalog 同步成功后一次性 swap”。
  - `TraverseOperators` 改为快照遍历，避免锁内回调风险。
  - 增加并发与去重回归测试。
- 结果：`test_bridge` 全通过。

### 2. 前端静态目录膨胀（已解决）

- 问题：`build/output/static` 持续累积历史 hash 文件，目录体积异常增大。
- 根因：CMake 使用 `copy_directory` 同步前端产物，但未先清理目标目录。
- 处理：
  - `src/services/web/CMakeLists.txt` 在复制前增加：
    - `remove_directory ${CMAKE_BINARY_DIR}/output/static`
    - `make_directory ${CMAKE_BINARY_DIR}/output/static`
- 结果：`build/output/static` 已从约 49MB 回落到与当前 `dist` 一致的约 1.5MB。

### 3. 前端主包过大（第一阶段已处理）

- 问题：路由页全部静态导入，首屏包过大。
- 处理：`src/frontend/src/router/index.js` 改为动态 import 懒加载。
- 结果：页面已拆分为独立 chunk（Dashboard/Channels/Operators/Tasks）。
- 备注：主入口 chunk 仍偏大，见未解决问题 U-07。

## 未解决问题（含暂缓项）

| ID | 优先级 | 问题 | 影响 | 建议 | 当前状态 |
|---|---|---|---|---|---|
| U-01 | P1 | BinAddon 激活流程缺少统一事务边界（注册工厂 + DB 更新非原子） | 中途失败可能产生半状态 | 将激活流程改为显式事务 + 可回滚步骤（建议引入 `activating` 状态） | 暂缓 |
| U-02 | P1 | Deactivate/Delete 一致性校验不充分，部分步骤返回值处理不严格 | 可能出现 metadata 与 catalog/工厂状态不一致 | Deactivate/Delete 全链路“步骤必检 + 失败即停止”，Delete 使用事务删除 | 暂缓 |
| U-03 | P1 | BinAddon `mu_` 锁内含外部组件调用（registry/db 组合路径） | 后续扩展时存在死锁/阻塞放大风险 | 拆分阶段，明确锁顺序，减少锁持有时长 | 暂缓 |
| U-04 | P2 | 部分错误响应仍有字符串拼接 JSON 的写法 | 错误信息含特殊字符时易产生非法 JSON | 统一改用 JSON Writer 生成错误响应结构 | 暂缓 |
| U-05 | P2 | 上传链路文件写入健壮性不足（写入长度校验、失败回收） | 极端情况下可能留下脏临时文件 | 补充 `fwrite` 返回值校验与失败清理 | 暂缓 |
| U-06 | P1 | BinAddon 失败回滚/并发冲突专项测试不足 | 回归时难及早发现一致性问题 | 在 `test_builtin` 增加失败注入与并发用例 | 暂缓 |
| U-07 | P2 | 前端主入口 chunk 仍偏大（构建仍有 >500KB 提示） | 首屏加载/解析压力仍偏高 | Element Plus 按需引入 + 视图进一步组件拆分 + 手工分包策略 | 待处理 |
| U-08 | P3 | 前端 API `baseURL` 硬编码为 `http://localhost:8081` | 非本地环境部署不灵活 | 改为环境变量（如 `VITE_API_BASE_URL`）并保留默认相对路径 | 待处理 |

## 建议的后续处理顺序

1. 先处理一致性与原子性：`U-01 -> U-02 -> U-03`。
2. 再补稳定性与可维护性：`U-06 -> U-04 -> U-05`。
3. 最后做前端性能与部署性：`U-07 -> U-08`。

## 验收与追踪建议

1. 对 U-01/U-02/U-03/U-06 建立专项回归用例，纳入 CI 必跑。
2. 对 U-07 增加前端包体积门禁（gzip 后阈值），避免后续反弹。
3. 每次 Sprint 收尾同步更新本文件“状态列”，确保问题闭环。

