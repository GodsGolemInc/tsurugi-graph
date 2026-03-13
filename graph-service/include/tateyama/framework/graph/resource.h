#pragma once

#include <memory>
#include <string_view>

#include <tateyama/framework/component.h>
#include <tateyama/framework/component_ids.h>
#include <tateyama/framework/environment.h>
#include <tateyama/framework/resource.h>

#include <tateyama/framework/graph/storage.h>

namespace tateyama::framework::graph {

/**
 * @brief Graph resource component for tateyama framework.
 * This resource manages graph storages.
 */
class resource : public framework::resource {
public:
    static constexpr id_type tag = framework::resource_id_graph;
    static constexpr std::string_view component_label = "graph_resource";

    resource() = default;

    [[nodiscard]] id_type id() const noexcept override {
        return tag;
    }

    bool setup(framework::environment& env) override;
    bool start(framework::environment& env) override;
    bool shutdown(framework::environment& env) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return component_label;
    }

    [[nodiscard]] class storage& storage() noexcept {
        return *storage_;
    }

    ~resource() override = default;

private:
    std::unique_ptr<class storage> storage_{};
};

} // namespace tateyama::framework::graph
