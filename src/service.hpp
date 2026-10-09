#pragma once
#include "config.hpp"
#include "runtime.hpp"
#include <optional>
#include <xgc2/xrpc/diagnostics.hpp>
#include <xgc2/xrpc/http.hpp>

namespace router {
// Serialization/fencing live at this facade, never in the tracker callbacks.
// The application supplies the actual reference after native startup.
class Service {
  public:
    Service(Runtime& router, Json::Value service_ref, const xgc2::xrpc::RuntimePolicy& policy,
            xgc2::xrpc::Diagnostics& diagnostics);
    void dispatch(xgc2::xrpc::HttpRequest request, xgc2::xrpc::HttpReply reply);
    void set_host(xgc2::xrpc::HttpServer& host) { host_ = &host; }
    void begin_drain() noexcept { draining_ = true; }
    void complete_native_update();

  private:
    Json::Value describe() const;
    Json::Value health() const;
    Json::Value mappings() const;
    Json::Value configuration() const;
    void apply(const Json::Value& request, const xgc2::xrpc::HttpRequest& metadata, xgc2::xrpc::HttpReply reply);
    Json::Value runtime_policy() const;
    Json::Value update_policy(const Json::Value& request);
    Runtime& router_;
    Json::Value service_ref_;
    const xgc2::xrpc::RuntimePolicy& policy_;
    xgc2::xrpc::Diagnostics& diagnostics_;
    xgc2::xrpc::HttpServer* host_ = nullptr;
    Clock::time_point started_ = Clock::now();
    std::optional<xgc2::xrpc::HttpReply> pending_;
    bool draining_ = false;
};
} // namespace router
