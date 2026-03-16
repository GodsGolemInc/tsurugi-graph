#include <tateyama/framework/graph/service.h>

#include <glog/logging.h>
#include <tateyama/framework/boot_mode.h>
#include <tateyama/framework/repository.h>
#include <tateyama/framework/transactional_kvs_resource.h>
#include <tateyama/api/server/request.h>
#include <tateyama/api/server/response.h>
#include <tateyama/proto/graph/request.pb.h>
#include <tateyama/proto/graph/response.pb.h>

#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include <tateyama/framework/graph/storage.h>
#include <tateyama/framework/graph/resource.h>
#include <sharksfin/api.h>

#include <thread>
#include <chrono>

namespace tateyama::framework::graph {

bool service::is_retryable(sharksfin::StatusCode code) {
    using sharksfin::StatusCode;
    return code == StatusCode::ERR_SERIALIZATION_FAILURE ||
           code == StatusCode::ERR_CONFLICT_ON_WRITE_PRESERVE ||
           code == StatusCode::ERR_WAITING_FOR_OTHER_TRANSACTION;
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

    graph_resource_ = env.resource_repository().find<framework::graph::resource>();
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

static sharksfin::TransactionOptions convert_options(const tateyama::proto::graph::request::TransactionOptions& proto_opt) {
    sharksfin::TransactionOptions opt{};

    if (proto_opt.type() == tateyama::proto::graph::request::TransactionOptions_Type_LTX) {
        opt.add_write_preserve(sharksfin::Slice(storage::STORAGE_NAME_NODES));
        opt.add_write_preserve(sharksfin::Slice(storage::STORAGE_NAME_EDGES));
        opt.add_write_preserve(sharksfin::Slice(storage::STORAGE_NAME_OUT_INDEX));
        opt.add_write_preserve(sharksfin::Slice(storage::STORAGE_NAME_IN_INDEX));
        opt.add_write_preserve(sharksfin::Slice(storage::STORAGE_NAME_LABEL_INDEX));
        opt.add_write_preserve(sharksfin::Slice(storage::STORAGE_NAME_PROPERTY_INDEX));
    }

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
        if (cypher.has_transaction_options()) {
            tx_opt = convert_options(cypher.transaction_options());
        }

        int attempt = 0;
        auto backoff = std::chrono::milliseconds(INITIAL_BACKOFF_MS);
        bool success_flag = false;

        while (attempt < MAX_RETRIES) {
            sharksfin::TransactionHandle tx{};
            if (sharksfin::transaction_begin(db_handle_, tx_opt, &tx) != sharksfin::StatusCode::OK) {
                std::this_thread::sleep_for(backoff);
                backoff *= 2;
                attempt++;
                continue;
            }

            try {
                // ADR-0003: Check query cache before parsing
                std::shared_ptr<core::statement> stmt_ptr = query_cache_.get(cypher.query());
                if (!stmt_ptr) {
                    core::lexer lexer(cypher.query());
                    core::parser parser(lexer.tokenize());
                    stmt_ptr = std::make_shared<core::statement>(parser.parse());
                    query_cache_.put(cypher.query(), stmt_ptr);
                }

                core::executor exec(graph_resource_->storage(), tx);
                std::string result_json;

                if (exec.execute(*stmt_ptr, result_json)) {
                    auto commit_rc = sharksfin::transaction_commit(tx);
                    if (commit_rc == sharksfin::StatusCode::OK) {
                        auto* success = proto_res.mutable_success();
                        success->mutable_cypher()->set_result_json(result_json);
                        success_flag = true;
                        sharksfin::transaction_dispose(tx);
                        break;
                    } else if (is_retryable(commit_rc)) {
                        sharksfin::transaction_dispose(tx);
                        std::this_thread::sleep_for(backoff);
                        backoff *= 2;
                        attempt++;
                        continue;
                    } else {
                        sharksfin::transaction_dispose(tx);
                        auto* error = proto_res.mutable_error();
                        error->set_code(2);
                        error->set_message("Fatal commit error");
                        break;
                    }
                } else {
                    sharksfin::transaction_abort(tx);
                    sharksfin::transaction_dispose(tx);
                    auto* error = proto_res.mutable_error();
                    error->set_code(1);
                    error->set_message("Query execution failed");
                    break;
                }
            } catch (const std::exception& e) {
                sharksfin::transaction_abort(tx);
                sharksfin::transaction_dispose(tx);
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
        sharksfin::TransactionHandle tx{};
        if (sharksfin::transaction_begin(db_handle_, tx_opt, &tx) != sharksfin::StatusCode::OK) {
            auto* error = proto_res.mutable_error();
            error->set_code(4);
            error->set_message("Failed to begin transaction for node get");
        } else {
            try {
                std::string props;
                if (graph_resource_->storage().get_node(tx, node_get.node_id(), props)) {
                    auto* success = proto_res.mutable_success();
                    auto* node = success->mutable_node();
                    node->set_node_id(node_get.node_id());
                    node->set_properties_json(props);
                } else {
                    auto* error = proto_res.mutable_error();
                    error->set_code(5);
                    error->set_message("Node not found: " + std::to_string(node_get.node_id()));
                }
                auto rc = sharksfin::transaction_commit(tx);
                if (rc != sharksfin::StatusCode::OK) {
                    LOG(WARNING) << "Failed to commit read transaction for node_get";
                }
            } catch (const std::exception& e) {
                sharksfin::transaction_abort(tx);
                auto* error = proto_res.mutable_error();
                error->set_code(3);
                error->set_message(std::string("Node get error: ") + e.what());
            }
            sharksfin::transaction_dispose(tx);
        }
    } else if (proto_req.has_edge_get()) {
        const auto& edge_get = proto_req.edge_get();

        sharksfin::TransactionOptions tx_opt{};
        sharksfin::TransactionHandle tx{};
        if (sharksfin::transaction_begin(db_handle_, tx_opt, &tx) != sharksfin::StatusCode::OK) {
            auto* error = proto_res.mutable_error();
            error->set_code(4);
            error->set_message("Failed to begin transaction for edge get");
        } else {
            try {
                edge_data ed;
                if (graph_resource_->storage().get_edge(tx, edge_get.edge_id(), ed)) {
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
                auto rc = sharksfin::transaction_commit(tx);
                if (rc != sharksfin::StatusCode::OK) {
                    LOG(WARNING) << "Failed to commit read transaction for edge_get";
                }
            } catch (const std::exception& e) {
                sharksfin::transaction_abort(tx);
                auto* error = proto_res.mutable_error();
                error->set_code(3);
                error->set_message(std::string("Edge get error: ") + e.what());
            }
            sharksfin::transaction_dispose(tx);
        }
    }

    std::string response_body;
    proto_res.SerializeToString(&response_body);
    res->session_id(req->session_id());
    res->body(response_body);
    return true;
}

} // namespace tateyama::framework::graph
