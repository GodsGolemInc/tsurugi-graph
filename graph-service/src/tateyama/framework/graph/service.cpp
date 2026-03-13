#include <tateyama/framework/graph/service.h>

#include <glog/logging.h>
#include <tateyama/framework/boot_mode.h>
#include <tateyama/framework/repository.h>
#include <tateyama/api/server/request.h>
#include <tateyama/api/server/response.h>
#include <tateyama/proto/graph/request.pb.h>
#include <tateyama/proto/graph/response.pb.h>

#include <tateyama/framework/graph/storage.h>
#include <regex>

namespace tateyama::framework::graph {

// Internal helper for simple parsing
struct cypher_parser {
    enum class command_type {
        create_node,
        match_node,
        unknown
    };

    std::string label;
    std::string properties;

    command_type parse(const std::string& query) {
        // Simple regex for CREATE (n:Label {prop: val})
        // Note: Very limited parser for prototype
        std::regex create_pattern(R"(CREATE\s*\(\w+:(\w+)\s*(\{.*\})\))");
        std::smatch match;
        if (std::regex_search(query, match, create_pattern)) {
            if (match.size() > 2) {
                label = match[1];
                properties = match[2];
                return command_type::create_node;
            }
        }
        
        // Simple regex for MATCH (n) RETURN n
        if (query.find("MATCH (n) RETURN n") != std::string::npos) {
             return command_type::match_node;
        }

        return command_type::unknown;
    }
};

bool service::setup(framework::environment&) {
    return true;
}

bool service::start(framework::environment& env) {
    if(env.mode() == framework::boot_mode::maintenance_standalone ||
       env.mode() == framework::boot_mode::maintenance_server || 
       env.mode() == framework::boot_mode::quiescent_server) {
        return true;
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
        auto type = parser.parse(cypher.query());
        
        auto* success = proto_res.mutable_success();
        auto* cypher_success = success->mutable_cypher();

        if (type == cypher_parser::command_type::create_node) {
             // In real implementation, we would start a transaction here,
             // call storage::create_node, and commit.
             // Here we simulate success.
             std::string result = "{\"created\": 1, \"label\": \"" + parser.label + "\"}";
             cypher_success->set_result_json(result);
        } else if (type == cypher_parser::command_type::match_node) {
             // Simulate returning all nodes
             cypher_success->set_result_json("{\"nodes\": []}");
        } else {
             cypher_success->set_result_json("{\"status\": \"not_implemented_or_syntax_error\"}");
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
