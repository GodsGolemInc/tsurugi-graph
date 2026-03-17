#include <tateyama/framework/graph/core/parser.h>
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph::core;

void test_create_edge() {
    std::string query = "CREATE (n1)-[r:KNOWS {since: '2020'}]->(n2)";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 1);
    auto create = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    assert(create->paths.size() == 1);
    auto& path = create->paths[0];
    assert(path.elements.size() == 2);

    // First node
    assert(path.elements[0].node->variable == "n1");
    // Relationship
    assert(path.elements[0].relationship != nullptr);
    assert(path.elements[0].relationship->variable == "r");
    assert(path.elements[0].relationship->type == "KNOWS");
    assert(path.elements[0].relationship->direction == "->");
    assert(path.elements[0].relationship->properties.size() == 1);
    // Second node
    assert(path.elements[1].node->variable == "n2");

    std::cout << "test_create_edge: PASS" << std::endl;
}

void test_create_edge_with_labels() {
    std::string query = "CREATE (a:Person {name: 'Alice'})-[r:KNOWS]->(b:Person {name: 'Bob'})";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto create = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    auto& path = create->paths[0];
    assert(path.elements[0].node->label == "Person");
    assert(path.elements[1].node->label == "Person");
    assert(path.elements[0].relationship->type == "KNOWS");

    std::cout << "test_create_edge_with_labels: PASS" << std::endl;
}

void test_left_arrow_edge() {
    std::string query = "CREATE (a)<-[r:FOLLOWS]-(b)";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto create = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    assert(create->paths[0].elements[0].relationship->direction == "<-");

    std::cout << "test_left_arrow_edge: PASS" << std::endl;
}

int main() {
    test_create_edge();
    test_create_edge_with_labels();
    test_left_arrow_edge();

    std::cout << "\nAll edge parser tests passed!" << std::endl;
    return 0;
}
