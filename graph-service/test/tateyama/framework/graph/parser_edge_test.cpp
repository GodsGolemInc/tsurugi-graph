#include <tateyama/framework/graph/cypher_parser.h>
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph;

void test_create_edge() {
    cypher_parser parser;
    auto res = parser.parse("CREATE (n1)-[r:KNOWS {since: '2020'}]->(n2)");
    assert(res.type == cypher_parser::command_type::create_edge);
    assert(res.from_var == "n1");
    assert(res.label == "KNOWS");
    assert(res.properties.at("since") == "'2020'");
    assert(res.to_var == "n2");
    std::cout << "test_create_edge: PASS" << std::endl;
}

int main() {
    test_create_edge();
    return 0;
}
