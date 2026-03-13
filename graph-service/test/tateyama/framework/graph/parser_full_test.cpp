#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/ast.h>
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph::core;

void test_full_create() {
    std::string query = "CREATE (n:Person {name: 'Alice', age: 30})";
    lexer l(query);
    parser p(l.tokenize());
    statement stmt = p.parse();

    assert(stmt.clauses.size() == 1);
    assert(stmt.clauses[0]->type() == clause_type::create);
    auto c = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    assert(c->paths.size() == 1);
    assert(c->paths[0].elements.size() == 1);
    auto node = c->paths[0].elements[0].node;
    assert(node->variable == "n");
    assert(node->label == "Person");
    assert(node->properties.size() == 2);
    
    std::cout << "test_full_create: PASS" << std::endl;
}

void test_full_match() {
    std::string query = "MATCH (n)-[r:KNOWS]->(m)";
    lexer l(query);
    parser p(l.tokenize());
    statement stmt = p.parse();

    assert(stmt.clauses.size() == 1);
    auto c = std::dynamic_pointer_cast<match_clause>(stmt.clauses[0]);
    assert(c->paths.size() == 1);
    assert(c->paths[0].elements.size() == 2);
    
    auto elem1 = c->paths[0].elements[0];
    assert(elem1.node->variable == "n");
    assert(elem1.relationship->variable == "r");
    assert(elem1.relationship->type == "KNOWS");
    assert(elem1.relationship->direction == "->");

    auto elem2 = c->paths[0].elements[1];
    assert(elem2.node->variable == "m");

    std::cout << "test_full_match: PASS" << std::endl;
}

int main() {
    try {
        test_full_create();
        test_full_match();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
