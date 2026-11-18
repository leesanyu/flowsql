// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBUILTIN_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBUILTIN_REGISTRY_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/istream_channel.h>

#include <rapidjson/document.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flowsql {

// {0xd09f7b6c-6d41-4a2c-a3f5-9d6016a0c2b7}
const Guid IID_BUILTIN_REGISTRY = {0xd09f7b6c, 0x6d41, 0x4a2c,
                                   {0xa3, 0xf5, 0x9d, 0x60, 0x16, 0xa0, 0xc2, 0xb7}};

struct StreamOptionField {
    std::string key;
    std::string type;  // int|string|bool|enum|array
    bool required = false;
    std::string default_value;
    std::vector<std::string> enum_values;
    int64_t min_value = 0;
    int64_t max_value = 0;
    bool has_range = false;
    bool power_of_two = false;
    std::string desc;
};

struct StreamChannelTypeDescriptor {
    std::string type;       // ring|tcp_session_mock|stream_hub
    std::string display_name;
    std::vector<std::string> allowed_roles;  // source|sink|both
    std::vector<StreamOptionField> option_schema;

    std::function<int(const rapidjson::Value& options,
                      std::string* normalized_json,
                      std::string* err)> validate_and_normalize;

    std::function<int(const std::string& category,
                      const std::string& name,
                      const std::string& normalized_json,
                      std::shared_ptr<IStreamChannel>* out,
                      std::string* err)> build;
};

struct BuiltinOperatorDescriptor {
    std::string category;
    std::string name;
    OperatorFactory factory;
};

/**
 * @brief 内置能力注册中心接口，统一管理内置 stream 通道类型与内置算子描述。
 */
interface IBuiltinRegistry {
    virtual ~IBuiltinRegistry() = default;

    /**
     * @brief 按类型名查询 stream 通道类型定义。
     * @param type 通道类型名（如 ring、stream_hub）。
     * @param out 输出类型描述。
     * @return 0 表示成功，非 0 表示未找到或失败。
     */
    virtual int FindStreamChannelType(const std::string& type,
                                      StreamChannelTypeDescriptor* out) = 0;
    /**
     * @brief 枚举所有 stream 通道类型定义。
     * @param callback 枚举回调，参数为单个类型描述。
     */
    virtual void ListStreamChannelTypes(
        std::function<void(const StreamChannelTypeDescriptor&)> callback) = 0;

    /**
     * @brief 枚举所有内置算子定义。
     * @param callback 枚举回调，参数为单个算子描述。
     */
    virtual void ListBuiltinOperators(
        std::function<void(const BuiltinOperatorDescriptor&)> callback) = 0;
    /**
     * @brief 创建内置算子实例。
     * @param category 算子分类。
     * @param name 算子名称。
     * @return 新建算子实例（调用方负责释放），失败返回 nullptr。
     */
    virtual IOperator* CreateBuiltinOperator(const std::string& category,
                                             const std::string& name) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBUILTIN_REGISTRY_H_
