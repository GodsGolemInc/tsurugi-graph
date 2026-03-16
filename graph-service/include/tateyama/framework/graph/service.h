#pragma once

#include <memory>
#include <string_view>

#include <tateyama/api/server/request.h>
#include <tateyama/api/server/response.h>
#include <tateyama/framework/component.h>
#include <tateyama/framework/component_ids.h>
#include <tateyama/framework/environment.h>
#include <tateyama/framework/service.h>
#include <sharksfin/api.h>
#include <tateyama/framework/graph/core/query_cache.h>

namespace tateyama::framework::graph {

class resource;

using tateyama::api::server::request;
using tateyama::api::server::response;

/**
 * @brief Graph service for tateyama framework.
 * This service handles graph queries (e.g. Cypher) and maps them to Shirakami.
 */
class service : public framework::service {
public:
    static constexpr id_type tag = framework::service_id_graph;
    static constexpr std::string_view component_label = "graph_service";

    service() = default;

    [[nodiscard]] id_type id() const noexcept override {
        return tag;
    }

    bool setup(framework::environment& env) override;
    bool start(framework::environment& env) override;
    bool shutdown(framework::environment& env) override;

    bool operator()(std::shared_ptr<request> req, std::shared_ptr<response> res) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return component_label;
    }

    ~service() override = default;

private:
    resource* graph_resource_ = nullptr;
    sharksfin::DatabaseHandle db_handle_ = nullptr;

    static constexpr int MAX_RETRIES = 5;
    static constexpr int INITIAL_BACKOFF_MS = 10;

    static bool is_retryable(sharksfin::StatusCode code);

    // Query cache (ADR-0003)
    core::query_cache query_cache_;
};

} // namespace tateyama::framework::graph
