#include <tateyama/framework/graph/cypher_parser.h>
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph;

void test_create_node() {
    cypher_parser parser;
    auto res = parser.parse("CREATE (n:Person {name: 'Alice', age: '30'})");
    assert(res.type == cypher_parser::command_type::create_node);
    assert(res.label == "Person");
    assert(res.properties.at("name") == "'Alice'");
    assert(res.properties.at("age") == "'30'");
    std::cout << "test_create_node: PASS" << std::endl;
}

void test_match_all() {
    cypher_parser parser;
    auto res = parser.parse("MATCH (n) RETURN n");
    assert(res.type == cypher_parser::command_type::match_node);
    std::cout << "test_match_all: PASS" << std::endl;
}

int main() {
    test_create_node();
    test_match_all();
    return 0;
}
