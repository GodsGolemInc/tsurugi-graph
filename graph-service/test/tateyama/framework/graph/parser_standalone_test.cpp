#include <tateyama/framework/graph/core/parser.h>
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph::core;

void test_create_node() {
    std::string query = "CREATE (n:Person {name: 'Alice', age: '30'})";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 1);
    assert(stmt.clauses[0]->type() == clause_type::create);
    auto create = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    assert(create->paths.size() == 1);
    assert(create->paths[0].elements.size() == 1);
    assert(create->paths[0].elements[0].node->label == "Person");
    assert(create->paths[0].elements[0].node->variable == "n");
    assert(create->paths[0].elements[0].node->properties.size() == 2);

    std::cout << "test_create_node: PASS" << std::endl;
}

void test_match_all() {
    std::string query = "MATCH (n) RETURN n";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 2);
    assert(stmt.clauses[0]->type() == clause_type::match);
    assert(stmt.clauses[1]->type() == clause_type::return_clause);

    std::cout << "test_match_all: PASS" << std::endl;
}

void test_where_clause() {
    std::string query = "MATCH (n:Person) WHERE n.name = 'Alice' RETURN n";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 3);
    assert(stmt.clauses[1]->type() == clause_type::where);
    auto where = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
    assert(where->variable == "n");
    assert(where->property == "name");
    assert(where->op == "=");

    std::cout << "test_where_clause: PASS" << std::endl;
}

void test_return_alias() {
    std::string query = "MATCH (n:Person) RETURN n.name AS person_name";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 2);
    auto ret = std::dynamic_pointer_cast<return_clause>(stmt.clauses[1]);
    assert(ret->items.size() == 1);
    assert(ret->items[0].alias == "person_name");
    assert(ret->items[0].expr->type() == node_type::property_access);

    std::cout << "test_return_alias: PASS" << std::endl;
}

void test_delete_clause() {
    std::string query = "MATCH (n:Person) DELETE n";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 2);
    assert(stmt.clauses[1]->type() == clause_type::delete_clause);
    auto del = std::dynamic_pointer_cast<delete_clause>(stmt.clauses[1]);
    assert(del->variables.size() == 1);
    assert(del->variables[0] == "n");
    assert(!del->detach);

    std::cout << "test_delete_clause: PASS" << std::endl;
}

void test_set_clause() {
    std::string query = "MATCH (n:Person) SET n.age = 31 RETURN n";
    lexer l(query);
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 3);
    assert(stmt.clauses[1]->type() == clause_type::set_clause);
    auto set = std::dynamic_pointer_cast<set_clause>(stmt.clauses[1]);
    assert(set->assignments.size() == 1);
    assert(set->assignments[0].variable == "n");
    assert(set->assignments[0].property == "age");

    std::cout << "test_set_clause: PASS" << std::endl;
}

int main() {
    test_create_node();
    test_match_all();
    test_where_clause();
    test_return_alias();
    test_delete_clause();
    test_set_clause();

    std::cout << "\nAll parser tests passed!" << std::endl;
    return 0;
}
