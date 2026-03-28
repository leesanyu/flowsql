#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBINADDON_HOST_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBINADDON_HOST_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <string>

namespace flowsql {

// {0x5a9d193e-59fd-4610-b8ca-5bf0fd15f2ba}
const Guid IID_BINADDON_HOST = {
    0x5a9d193e, 0x59fd, 0x4610, {0xb8, 0xca, 0x5b, 0xf0, 0xfd, 0x15, 0xf2, 0xba}};

// BinAddon 宿主接口：供 CatalogPlugin 统一 /operators/* 入口委派 cpp 分支
interface IBinAddonHost {
    virtual ~IBinAddonHost() = default;

    virtual int ListCppPlugins(std::string& rsp) = 0;
    virtual int UploadCppPlugin(const std::string& filename, const std::string& tmp_path, std::string& rsp) = 0;
    virtual int ActivateCppPlugin(const std::string& plugin_id, std::string& rsp) = 0;
    virtual int DeactivateCppPlugin(const std::string& plugin_id, std::string& rsp) = 0;
    virtual int DeleteCppPlugin(const std::string& plugin_id, std::string& rsp) = 0;
    virtual int GetCppPluginDetail(const std::string& plugin_id, std::string& rsp) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBINADDON_HOST_H_
