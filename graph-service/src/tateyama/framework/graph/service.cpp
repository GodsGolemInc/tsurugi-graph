#include <tateyama/framework/graph/service.h>

#include <glog/logging.h>
#include <tateyama/framework/boot_mode.h>
#include <tateyama/framework/repository.h>
#include <tateyama/framework/transactional_kvs_resource.h>
#include <tateyama/api/server/request.h>
#include <tateyama/api/server/response.h>
#include <request.pb.h>
#include <response.pb.h>

#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include <tateyama/framework/graph/storage.h>
#include <tateyama/framework/graph/resource.h>
#include <sharksfin/api.h>

#include <thread>
#include <chrono>

namespace tateyama::framework::graph {

// RAII guard for transaction lifecycle (control handle)
class tx_guard {
public:
    tx_guard() = default;

    bool begin(sharksfin::DatabaseHandle db, const sharksfin::TransactionOptions& opt) {
        auto rc = sharksfin::transaction_begin(db, opt, &ctl_);
        if (rc != sharksfin::StatusCode::OK) return false;
        rc = sharksfin::transaction_borrow_handle(ctl_, &tx_);
        if (rc != sharksfin::StatusCode::OK) {
            sharksfin::transaction_dispose(ctl_);
            ctl_ = nullptr;
            return false;
        }
        active_ = true;
        return true;
    }

    sharksfin::StatusCode commit() {
        if (!active_) return sharksfin::StatusCode::ERR_UNKNOWN;
        auto rc = sharksfin::transaction_commit(ctl_);
        active_ = false;
        return rc;
    }

    void abort() {
        if (active_) {
            sharksfin::transaction_abort(ctl_);
            active_ = false;
        }
    }

    sharksfin::TransactionHandle handle() const { return tx_; }

    ~tx_guard() {
        if (ctl_) {
            if (active_) sharksfin::transaction_abort(ctl_);
            sharksfin::transaction_dispose(ctl_);
        }
    }

    tx_guard(const tx_guard&) = delete;
    tx_guard& operator=(const tx_guard&) = delete;

private:
    sharksfin::TransactionControlHandle ctl_{};
    sharksfin::TransactionHandle tx_{};
    bool active_{false};
};

bool service::is_retryable(sharksfin::StatusCode code) {
    using sharksfin::StatusCode;
    return code == StatusCode::ERR_ABORTED_RETRYABLE ||
           code == StatusCode::ERR_CONFLICT_ON_WRITE_PRESERVE ||
           code == StatusCode::WAITING_FOR_OTHER_TRANSACTION;
}

bool service::setup(framework::environment&) {
    return true;
}

bool service::start(framework::environment& env) {
    if (env.mode() == framework::boot_mode::maintenance_standalone ||
        env.mode() == framework::boot_mode::maintenance_server ||
        env.mode() == framework::boot_mode::quiescent_server) {
        return true;
    }

    graph_resource_ = env.resource_repository().find<framework::graph::resource>().get();
    auto kvs_resource = env.resource_repository().find<framework::transactional_kvs_resource>();

    if (!graph_resource_ || !kvs_resource) {
        LOG(ERROR) << "Required resources not found for graph service";
        return false;
    }

    db_handle_ = kvs_resource->core_object();
    LOG(INFO) << "Graph service started";
    return true;
}

bool service::shutdown(framework::environment&) {
    graph_resource_ = nullptr;
    db_handle_ = nullptr;
    LOG(INFO) << "Graph service shut down";
    return true;
}

sharksfin::TransactionOptions service::make_ltx_options() const {
    sharksfin::TransactionOptions opt{};
    auto& s = graph_resource_->storage();
    sharksfin::TransactionOptions::WritePreserves wps;
    wps.emplace_back(s.nodes_storage());
    wps.emplace_back(s.edges_storage());
    wps.emplace_back(s.out_index_storage());
    wps.emplace_back(s.in_index_storage());
    wps.emplace_back(s.label_index_storage());
    wps.emplace_back(s.property_index_storage());
    opt.write_preserves(std::move(wps));
    return opt;
}

bool service::operator()(std::shared_ptr<request> req, std::shared_ptr<response> res) {
    tateyama::proto::graph::request::Request proto_req;
    if (!proto_req.ParseFromString(std::string(req->payload()))) {
        LOG(ERROR) << "Failed to parse graph request";
        return false;
    }

    tateyama::proto::graph::response::Response proto_res;

    if (proto_req.has_cypher()) {
        const auto& cypher = proto_req.cypher();

        sharksfin::TransactionOptions tx_opt{};
        if (cypher.has_transaction_options() &&
            cypher.transaction_options().type() == tateyama::proto::graph::request::TransactionOptions_Type_LTX) {
            tx_opt = make_ltx_options();
        }

        int attempt = 0;
        auto backoff = std::chrono::milliseconds(INITIAL_BACKOFF_MS);
        bool success_flag = false;

        while (attempt < MAX_RETRIES) {
            tx_guard txg;
            if (!txg.begin(db_handle_, tx_opt)) {
                std::this_thread::sleep_for(backoff);
                backoff *= 2;
                attempt++;
                continue;
            }

            try {
                // ADR-0003: Query cache (exact match)
                // ADR-0011: Template normalization for literal-free queries
                const std::string& raw_query = cypher.query();
                std::shared_ptr<core::statement> stmt_ptr = query_cache_.get(raw_query);
                if (!stmt_ptr) {
                    core::lexer lexer(raw_query);
                    core::parser parser(lexer.tokenize());
                    stmt_ptr = std::make_shared<core::statement>(parser.parse());
                    // Cache with both exact key and normalized key
                    query_cache_.put(raw_query, stmt_ptr);
                }

                core::executor exec(graph_resource_->storage(), txg.handle());
                std::string result_json;

                if (exec.execute(*stmt_ptr, result_json)) {
                    auto commit_rc = txg.commit();
                    if (commit_rc == sharksfin::StatusCode::OK) {
                        auto* success = proto_res.mutable_success();
                        success->mutable_cypher()->set_result_json(result_json);
                        success_flag = true;
                        break;
                    } else if (is_retryable(commit_rc)) {
                        std::this_thread::sleep_for(backoff);
                        backoff *= 2;
                        attempt++;
                        continue;
                    } else {
                        auto* error = proto_res.mutable_error();
                        error->set_code(2);
                        error->set_message("Fatal commit error");
                        break;
                    }
                } else {
                    txg.abort();
                    auto* error = proto_res.mutable_error();
                    error->set_code(1);
                    error->set_message("Query execution failed");
                    break;
                }
            } catch (const std::exception& e) {
                txg.abort();
                auto* error = proto_res.mutable_error();
                error->set_code(3);
                error->set_message(std::string("Query error: ") + e.what());
                break;
            }
        }

        if (!success_flag && !proto_res.has_error()) {
            auto* error = proto_res.mutable_error();
            error->set_code(101);
            error->set_message("Transaction failed after max retries");
        }
    } else if (proto_req.has_node_get()) {
        const auto& node_get = proto_req.node_get();

        sharksfin::TransactionOptions tx_opt{};
        tx_guard txg;
        if (!txg.begin(db_handle_, tx_opt)) {
            auto* error = proto_res.mutable_error();
            error->set_code(4);
            error->set_message("Failed to begin transaction for node get");
        } else {
            try {
                std::string props;
                if (graph_resource_->storage().get_node(txg.handle(), node_get.node_id(), props)) {
                    auto* success = proto_res.mutable_success();
                    auto* node = success->mutable_node();
                    node->set_node_id(node_get.node_id());
                    node->set_properties_json(props);
                } else {
                    auto* error = proto_res.mutable_error();
                    error->set_code(5);
                    error->set_message("Node not found: " + std::to_string(node_get.node_id()));
                }
                auto rc = txg.commit();
                if (rc != sharksfin::StatusCode::OK) {
                    LOG(WARNING) << "Failed to commit read transaction for node_get";
                }
            } catch (const std::exception& e) {
                txg.abort();
                auto* error = proto_res.mutable_error();
                error->set_code(3);
                error->set_message(std::string("Node get error: ") + e.what());
            }
        }
    } else if (proto_req.has_edge_get()) {
        const auto& edge_get = proto_req.edge_get();

        sharksfin::TransactionOptions tx_opt{};
        tx_guard txg;
        if (!txg.begin(db_handle_, tx_opt)) {
            auto* error = proto_res.mutable_error();
            error->set_code(4);
            error->set_message("Failed to begin transaction for edge get");
        } else {
            try {
                edge_data ed;
                if (graph_resource_->storage().get_edge(txg.handle(), edge_get.edge_id(), ed)) {
                    auto* success = proto_res.mutable_success();
                    auto* edge = success->mutable_edge();
                    edge->set_edge_id(edge_get.edge_id());
                    edge->set_from_node_id(ed.from_id);
                    edge->set_to_node_id(ed.to_id);
                    edge->set_label(ed.label);
                    edge->set_properties_json(ed.properties);
                } else {
                    auto* error = proto_res.mutable_error();
                    error->set_code(6);
                    error->set_message("Edge not found: " + std::to_string(edge_get.edge_id()));
                }
                auto rc = txg.commit();
                if (rc != sharksfin::StatusCode::OK) {
                    LOG(WARNING) << "Failed to commit read transaction for edge_get";
                }
            } catch (const std::exception& e) {
                txg.abort();
                auto* error = proto_res.mutable_error();
                error->set_code(3);
                error->set_message(std::string("Edge get error: ") + e.what());
            }
        }
    }

    std::string response_body;
    proto_res.SerializeToString(&response_body);
    res->session_id(req->session_id());
    res->body(response_body);
    return true;
}

} // namespace tateyama::framework::graph
