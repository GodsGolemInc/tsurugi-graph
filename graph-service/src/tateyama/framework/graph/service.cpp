#include <tateyama/framework/graph/service.h>

#include <glog/logging.h>
#include <tateyama/framework/boot_mode.h>
#include <tateyama/framework/repository.h>
#include <tateyama/api/server/request.h>
#include <tateyama/api/server/response.h>
#include <tateyama/proto/graph/request.pb.h>
#include <tateyama/proto/graph/response.pb.h>

#include <tateyama/framework/graph/cypher_parser.h>
#include <tateyama/framework/graph/storage.h>
#include <tateyama/framework/graph/resource.h>

namespace tateyama::framework::graph {

// Internal pointer to resource (to be obtained during start)
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
        LOG(ERROR) << "Graph resource not found in service";
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
        LOG(ERROR) << "Failed to parse request payload";
        return false;
    }

    tateyama::proto::graph::response::Response proto_res;
    
    if (proto_req.has_cypher()) {
        const auto& cypher = proto_req.cypher();
        LOG(INFO) << "Received Cypher query: " << cypher.query();
        
        cypher_parser parser;
        auto res_parse = parser.parse(cypher.query());
        
        auto* success = proto_res.mutable_success();
        auto* cypher_success = success->mutable_cypher();

        if (res_parse.type == cypher_parser::command_type::create_node) {
             // Logic to create a node in storage
             // In real implementation, you would use a transaction handle from sharksfin.
             // Here we simulate successful creation since we are in mock/prototype mode.
             
             // std::string properties_json = ... serialize res_parse.properties ...
             
             std::string result = "{\"created\": 1, \"label\": \"" + res_parse.label + "\"}";
             cypher_success->set_result_json(result);
        } else if (res_parse.type == cypher_parser::command_type::match_node) {
             cypher_success->set_result_json("{\"nodes\": []}");
        } else {
             cypher_success->set_result_json("{\"status\": \"syntax_error_or_not_supported\"}");
        }

    } else {
        auto* error = proto_res.mutable_error();
        error->set_code(1); 
        error->set_message("Unknown command");
    }

    std::string response_body;
    if (!proto_res.SerializeToString(&response_body)) {
        LOG(ERROR) << "Failed to serialize response";
        return false;
    }

    res->session_id(req->session_id());
    res->body(response_body);
    return true;
}

} // namespace tateyama::framework::graph
