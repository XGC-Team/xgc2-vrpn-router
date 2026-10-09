#pragma once
#include "service.hpp"
#include <xgc2/xrpc/bootstrap.hpp>

namespace router {
// Resolved startup values belong to the composition root. The shared SDK owns
// endpoint validation/lease; this function owns native resources and drain.
int run(Config config, const xgc2::xrpc::BootstrapInput& bootstrap,
        const xgc2::xrpc::RuntimePolicy& policy);
} // namespace router
