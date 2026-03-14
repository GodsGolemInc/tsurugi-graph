#include <iostream>
#include <memory>
#include <cassert>
#include "sharksfin_mock.h"
#include <tateyama/framework/graph/service.h>
#include <tateyama/api/server/mock/request.h>
#include <tateyama/api/server/mock/response.h>
#include <tateyama/proto/graph/request.pb.h>
#include <tateyama/proto/graph/response.pb.h>

using namespace tateyama::framework::graph;

void test_retry_on_serialization_failure() {
    auto svc = std::make_shared<service>();
    auto req = std::make_shared<tateyama::api::server::mock::request>();
    auto res = std::make_shared<tateyama::api::server::mock::response>();

    tateyama::proto::graph::request::Request proto_req;
    auto* cypher = proto_req.mutable_cypher();
    cypher->set_query("CREATE (n:Person {name: 'Alice'})");

    std::string payload;
    proto_req.SerializeToString(&payload);
    req->payload(payload);

    // Inject 2 failures, should succeed on 3rd attempt
    sharksfin::mock_commit_failure_count = 2;

    std::cout << "Starting request with 2 simulated serialization failures..." << std::endl;
    bool result = (*svc)(req, res);
    assert(result);

    tateyama::proto::graph::response::Response proto_res;
    proto_res.ParseFromString(res->body_);
    
    if (proto_res.has_success()) {
        std::cout << "test_retry_on_serialization_failure: PASS (Successfully retried and committed)" << std::endl;
    } else {
        std::cout << "test_retry_on_serialization_failure: FAIL (Error: " << proto_res.error().message() << ")" << std::endl;
        assert(false);
    }
    assert(sharksfin::mock_commit_failure_count == 0);
}

int main() {
    try {
        test_retry_on_serialization_failure();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
