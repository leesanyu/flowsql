#ifndef _FLOWSQL_SERVICES_BINADDON_BINADDON_OPERATOR_PROXY_H_
#define _FLOWSQL_SERVICES_BINADDON_BINADDON_OPERATOR_PROXY_H_

#include <framework/interfaces/ioperator.h>

#include <memory>

#include "binaddon_host_plugin.h"

namespace flowsql {
namespace binaddon {

class BinAddonOperatorProxy : public IOperator {
 public:
    BinAddonOperatorProxy(IOperator* impl, const std::shared_ptr<BinAddonHostPlugin::LoadedPlugin>& plugin)
        : impl_(impl), plugin_(plugin) {
        if (plugin_) plugin_->active_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~BinAddonOperatorProxy() override {
        if (plugin_ && impl_ && plugin_->destroy_fn) {
            plugin_->destroy_fn(impl_);
            impl_ = nullptr;
        }
        if (plugin_) plugin_->active_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::string Category() override { return impl_ ? impl_->Category() : ""; }
    std::string Name() override { return impl_ ? impl_->Name() : ""; }
    std::string Description() override { return impl_ ? impl_->Description() : ""; }
    OperatorPosition Position() override { return impl_ ? impl_->Position() : OperatorPosition::DATA; }
    int Work(IChannel* in, IChannel* out) override { return impl_ ? impl_->Work(in, out) : -1; }
    int Work(Span<IChannel*> inputs, IChannel* out) override { return impl_ ? impl_->Work(inputs, out) : -1; }
    int Configure(const char* key, const char* value) override { return impl_ ? impl_->Configure(key, value) : -1; }
    std::string LastError() override { return impl_ ? impl_->LastError() : "binaddon proxy invalid"; }

 private:
    IOperator* impl_ = nullptr;
    std::shared_ptr<BinAddonHostPlugin::LoadedPlugin> plugin_;
};

}  // namespace binaddon
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_BINADDON_BINADDON_OPERATOR_PROXY_H_
