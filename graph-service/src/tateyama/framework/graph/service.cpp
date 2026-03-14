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

namespace tateyama::framework::graph {

static resource* graph_resource_ = nullptr;
static sharksfin::DatabaseHandle db_handle_ = nullptr;

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
        LOG(ERROR) << "Required resources (graph or transactional_kvs) not found";
        return false;
    }

    db_handle_ = kvs_resource->core_object();
    LOG(INFO) << "Graph service started with real database handle";
    return true;
}

bool service::shutdown(framework::environment&) {
    LOG(INFO) << "Graph service shutting down";
    return true;
}

bool service::operator()(std::shared_ptr<request> req, std::shared_ptr<response> res) {
    tateyama::proto::graph::request::Request proto_req;
    if (!proto_req.ParseFromString(std::string(req->payload()))) {
        return false;
    }

    tateyama::proto::graph::response::Response proto_res;
    
    if (proto_req.has_cypher()) {
        const auto& cypher = proto_req.cypher();
        
        // 1. Transaction Begin (Real ACID Transaction)
        sharksfin::TransactionHandle tx{};
        sharksfin::TransactionOptions options{}; // Default options (OCC in Shirakami)
        if (sharksfin::transaction_begin(db_handle_, options, &tx) != sharksfin::StatusCode::OK) {
            auto* error = proto_res.mutable_error();
            error->set_code(100);
            error->set_message("Failed to begin transaction");
        } else {
            try {
                // 2. Lex & Parse
                core::lexer lexer(cypher.query());
                core::parser parser(lexer.tokenize());
                auto stmt = parser.parse();

                // 3. Execute within Transaction
                core::executor exec(graph_resource_->storage(), tx);
                std::string result_json;
                if (exec.execute(stmt, result_json)) {
                    // 4. Commit
                    if (sharksfin::transaction_commit(tx) == sharksfin::StatusCode::OK) {
                        auto* success = proto_res.mutable_success();
                        success->mutable_cypher()->set_result_json(result_json);
                    } else {
                        throw std::runtime_error("Failed to commit transaction");
                    }
                } else {
                    sharksfin::transaction_abort(tx);
                    throw std::runtime_error("Execution failed");
                }
            } catch (const std::exception& e) {
                sharksfin::transaction_abort(tx);
                auto* error = proto_res.mutable_error();
                error->set_code(1);
                error->set_message(e.what());
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
