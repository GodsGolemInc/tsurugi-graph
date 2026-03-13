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

namespace tateyama::framework::graph {

static resource* graph_resource_ = nullptr;

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
    if (!graph_resource_) {
        LOG(ERROR) << "Graph resource not found";
        return false;
    }

    LOG(INFO) << "Graph service started";
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
        
        try {
            // 1. Lex & Parse
            core::lexer lexer(cypher.query());
            core::parser parser(lexer.tokenize());
            auto stmt = parser.parse();

            // 2. Obtain Transaction (In real Tsurugi, we might get it from transactional_kvs_resource)
            // For now, this is where we integrate with the actual KVS ACID transaction.
            
            // NOTE: In production, we'd use transactional_kvs_resource to begin a transaction.
            // sharksfin::TransactionHandle tx = ...;
            
            // 3. Execute
            // executor exec(graph_resource_->storage(), tx);
            // std::string result_json;
            // if (exec.execute(stmt, result_json)) {
            //     auto* success = proto_res.mutable_success();
            //     success->mutable_cypher()->set_result_json(result_json);
            // }

            // Placeholder for prototype response
            auto* success = proto_res.mutable_success();
            success->mutable_cypher()->set_result_json("{\"status\": \"executed_successfully_in_prototype\"}");

        } catch (const std::exception& e) {
            auto* error = proto_res.mutable_error();
            error->set_code(1);
            error->set_message(e.what());
        }
    }

    std::string response_body;
    proto_res.SerializeToString(&response_body);
    res->session_id(req->session_id());
    res->body(response_body);
    return true;
}

} // namespace tateyama::framework::graph
