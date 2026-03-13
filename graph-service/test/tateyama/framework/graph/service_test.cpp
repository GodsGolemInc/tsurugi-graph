#include <tateyama/framework/graph/service.h>
#include <gtest/gtest.h>
#include <tateyama/framework/environment.h>
#include <tateyama/api/server/mock/request.h>
#include <tateyama/api/server/mock/response.h>
#include <tateyama/proto/graph/request.pb.h>
#include <tateyama/proto/graph/response.pb.h>

namespace tateyama::framework::graph {

class service_test : public ::testing::Test {
public:
    void SetUp() override {
        service_ = std::make_shared<service>();
    }
    std::shared_ptr<service> service_;
};

TEST_F(service_test, cypher_create_parse) {
    auto req = std::make_shared<tateyama::api::server::mock::request>();
    auto res = std::make_shared<tateyama::api::server::mock::response>();

    tateyama::proto::graph::request::Request proto_req;
    auto* cypher = proto_req.mutable_cypher();
    cypher->set_query("CREATE (n:Person {name: 'Alice'})");

    std::string payload;
    proto_req.SerializeToString(&payload);
    req->payload(payload);

    EXPECT_TRUE((*service_)(req, res));

    tateyama::proto::graph::response::Response proto_res;
    proto_res.ParseFromString(res->body_);
    
    EXPECT_TRUE(proto_res.has_success());
    // In mock implementation, we expect a success message with "created"
    EXPECT_NE(proto_res.success().cypher().result_json().find("created"), std::string::npos);
    EXPECT_NE(proto_res.success().cypher().result_json().find("Person"), std::string::npos);
}

} // namespace tateyama::framework::graph
