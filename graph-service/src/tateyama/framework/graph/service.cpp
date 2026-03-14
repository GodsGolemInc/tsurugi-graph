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

static resource* graph_resource_ = nullptr;
static sharksfin::DatabaseHandle db_handle_ = nullptr;

// Production Config
static constexpr int MAX_RETRIES = 5;
static constexpr std::chrono::milliseconds INITIAL_BACKOFF{10};

bool service::setup(framework::environment&) {
    return true;
}

bool service::start(framework::environment& env) {
    if(env.mode() == framework::boot_mode::maintenance_standalone ||
       env.mode() == framework::boot_mode::maintenance_server || 
       env.mode() == framework::boot_mode::quiescent_server) {
        return true;
    }

    graph_resource_ = env.resource_repository().find<framework::graph::resource>();
    auto kvs_resource = env.resource_repository().find<framework::transactional_kvs_resource>();
    
    if (!graph_resource_ || !kvs_resource) {
        LOG(ERROR) << "Required resources not found";
        return false;
    }

    db_handle_ = kvs_resource->core_object();
    return true;
}

bool service::shutdown(framework::environment&) {
    return true;
}

// Helper to determine if an error is retryable in TsurugiDB
static bool is_retryable(sharksfin::StatusCode code) {
    using sharksfin::StatusCode;
    return code == StatusCode::ERR_SERIALIZATION_FAILURE || 
           code == StatusCode::ERR_CONFLICT_ON_WRITE_PRESERVE ||
           code == StatusCode::ERR_WAITING_FOR_OTHER_TRANSACTION;
}

bool service::operator()(std::shared_ptr<request> req, std::shared_ptr<response> res) {
    tateyama::proto::graph::request::Request proto_req;
    if (!proto_req.ParseFromString(std::string(req->payload()))) {
        return false;
    }

    tateyama::proto::graph::response::Response proto_res;
    
    if (proto_req.has_cypher()) {
        const auto& cypher = proto_req.cypher();
        
        int attempt = 0;
        auto backoff = INITIAL_BACKOFF;
        bool success_flag = false;

        while (attempt < MAX_RETRIES) {
            sharksfin::TransactionHandle tx{};
            sharksfin::TransactionOptions options{}; // Default OCC
            
            if (sharksfin::transaction_begin(db_handle_, options, &tx) != sharksfin::StatusCode::OK) {
                std::this_thread::sleep_for(backoff);
                backoff *= 2;
                attempt++;
                continue;
            }

            try {
                core::lexer lexer(cypher.query());
                core::parser parser(lexer.tokenize());
                auto stmt = parser.parse();

                core::executor exec(graph_resource_->storage(), tx);
                std::string result_json;
                
                if (exec.execute(stmt, result_json)) {
                    auto commit_rc = sharksfin::transaction_commit(tx);
                    if (commit_rc == sharksfin::StatusCode::OK) {
                        auto* success = proto_res.mutable_success();
                        success->mutable_cypher()->set_result_json(result_json);
                        success_flag = true;
                        sharksfin::transaction_dispose(tx);
                        break; // Success!
                    } else if (is_retryable(commit_rc)) {
                        sharksfin::transaction_dispose(tx);
                        // Retryable commit failure (e.g. OCC conflict)
                        std::this_thread::sleep_for(backoff);
                        backoff *= 2;
                        attempt++;
                        continue;
                    } else {
                        throw std::runtime_error("Fatal commit error");
                    }
                } else {
                    sharksfin::transaction_abort(tx);
                    throw std::runtime_error("Query execution error");
                }
            } catch (const std::exception& e) {
                sharksfin::transaction_abort(tx);
                sharksfin::transaction_dispose(tx);
                
                // If it's a syntax error, don't retry
                auto* error = proto_res.mutable_error();
                error->set_code(1);
                error->set_message(e.what());
                break; 
            }
        }

        if (!success_flag && !proto_res.has_error()) {
            auto* error = proto_res.mutable_error();
            error->set_code(101);
            error->set_message("Max retries reached due to transaction conflicts");
        }
    }

    std::string response_body;
    proto_res.SerializeToString(&response_body);
    res->session_id(req->session_id());
    res->body(response_body);
    return true;
}

} // namespace tateyama::framework::graph
