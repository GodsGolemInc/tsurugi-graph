#include <tateyama/framework/graph/core/parser.h>
#include <iostream>
#include <cassert>
#include <string>
#include <vector>

using namespace tateyama::framework::graph::core;

// ======== Property tests: parse consistency ========

// Property: parsing a query twice should produce structurally identical ASTs
void test_parse_deterministic() {
    std::string queries[] = {
        "CREATE (n:Person {name: 'Alice', age: 30})",
        "MATCH (n:Person) WHERE n.name = 'Alice' RETURN n",
        "MATCH (n:Person) SET n.age = 31 RETURN n",
        "MATCH (n:Person) DELETE n",
        "MATCH (n) RETURN n",
        "CREATE (a:Person {name: 'A'})-[r:KNOWS]->(b:Person {name: 'B'})",
    };

    for (auto& q : queries) {
        lexer l1(q);
        parser p1(l1.tokenize());
        auto stmt1 = p1.parse();

        lexer l2(q);
        parser p2(l2.tokenize());
        auto stmt2 = p2.parse();

        assert(stmt1.clauses.size() == stmt2.clauses.size());
        for (size_t i = 0; i < stmt1.clauses.size(); ++i) {
            assert(stmt1.clauses[i]->type() == stmt2.clauses[i]->type());
        }
    }
    std::cout << "test_parse_deterministic: PASS" << std::endl;
}

// ======== All clause types ========

void test_create_clause_parsing() {
    // Simple node
    {
        lexer l("CREATE (n:Person {name: 'Alice'})");
        parser p(l.tokenize());
        auto stmt = p.parse();
        assert(stmt.clauses.size() == 1);
        assert(stmt.clauses[0]->type() == clause_type::create);
        auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
        assert(cr->paths.size() == 1);
        assert(cr->paths[0].elements[0].node->label == "Person");
        assert(cr->paths[0].elements[0].node->variable == "n");
    }

    // Node without properties
    {
        lexer l("CREATE (n:Person)");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
        assert(cr->paths[0].elements[0].node->properties.empty());
    }

    // Node without label
    {
        lexer l("CREATE (n {name: 'Alice'})");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
        assert(cr->paths[0].elements[0].node->label.empty());
    }

    std::cout << "test_create_clause_parsing: PASS" << std::endl;
}

void test_match_clause_parsing() {
    // Simple match
    {
        lexer l("MATCH (n:Person) RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        assert(stmt.clauses[0]->type() == clause_type::match);
        auto mc = std::dynamic_pointer_cast<match_clause>(stmt.clauses[0]);
        assert(mc->paths.size() == 1);
        assert(mc->paths[0].elements[0].node->label == "Person");
    }

    // Match all
    {
        lexer l("MATCH (n) RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto mc = std::dynamic_pointer_cast<match_clause>(stmt.clauses[0]);
        assert(mc->paths[0].elements[0].node->label.empty());
    }

    // Match with relationship
    {
        lexer l("MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN a");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto mc = std::dynamic_pointer_cast<match_clause>(stmt.clauses[0]);
        assert(mc->paths[0].elements.size() >= 2);
    }

    std::cout << "test_match_clause_parsing: PASS" << std::endl;
}

void test_where_clause_parsing() {
    // Equality
    {
        lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto wc = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
        assert(wc->variable == "n");
        assert(wc->property == "name");
        assert(wc->op == "=");
    }

    // Greater than
    {
        lexer l("MATCH (n:Person) WHERE n.age > 30 RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto wc = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
        assert(wc->op == ">");
    }

    // Less than
    {
        lexer l("MATCH (n:Person) WHERE n.age < 30 RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto wc = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
        assert(wc->op == "<");
    }

    std::cout << "test_where_clause_parsing: PASS" << std::endl;
}

void test_return_clause_parsing() {
    // Simple return
    {
        lexer l("MATCH (n) RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto rc = std::dynamic_pointer_cast<return_clause>(stmt.clauses[1]);
        assert(rc->items.size() == 1);
        assert(rc->items[0].expr->type() == node_type::variable);
    }

    // Return with alias
    {
        lexer l("MATCH (n:Person) RETURN n.name AS person_name");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto rc = std::dynamic_pointer_cast<return_clause>(stmt.clauses[1]);
        assert(rc->items[0].alias == "person_name");
        assert(rc->items[0].expr->type() == node_type::property_access);
    }

    // Multiple return items
    {
        lexer l("MATCH (n:Person) RETURN n.name AS name, n.age AS age");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto rc = std::dynamic_pointer_cast<return_clause>(stmt.clauses[1]);
        assert(rc->items.size() == 2);
    }

    std::cout << "test_return_clause_parsing: PASS" << std::endl;
}

void test_set_clause_parsing() {
    // Single assignment
    {
        lexer l("MATCH (n:Person) SET n.age = 31 RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto sc = std::dynamic_pointer_cast<set_clause>(stmt.clauses[1]);
        assert(sc->assignments.size() == 1);
        assert(sc->assignments[0].variable == "n");
        assert(sc->assignments[0].property == "age");
    }

    // String value
    {
        lexer l("MATCH (n:Person) SET n.name = 'Bob' RETURN n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto sc = std::dynamic_pointer_cast<set_clause>(stmt.clauses[1]);
        auto lit = std::dynamic_pointer_cast<literal>(sc->assignments[0].value);
        assert(lit != nullptr);
        assert(lit->value == "Bob");
        assert(lit->is_string);
    }

    std::cout << "test_set_clause_parsing: PASS" << std::endl;
}

void test_delete_clause_parsing() {
    // Simple DELETE
    {
        lexer l("MATCH (n:Person) DELETE n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto dc = std::dynamic_pointer_cast<delete_clause>(stmt.clauses[1]);
        assert(dc->variables.size() == 1);
        assert(dc->variables[0] == "n");
        assert(!dc->detach);
    }

    // DETACH DELETE
    {
        lexer l("MATCH (n:Person) DETACH DELETE n");
        parser p(l.tokenize());
        auto stmt = p.parse();
        // DETACH DELETE might be parsed differently depending on parser
        bool found_delete = false;
        for (auto& c : stmt.clauses) {
            if (c->type() == clause_type::delete_clause) {
                found_delete = true;
                auto dc = std::dynamic_pointer_cast<delete_clause>(c);
                assert(dc->detach == true);
            }
        }
        assert(found_delete);
    }

    std::cout << "test_delete_clause_parsing: PASS" << std::endl;
}

void test_unwind_clause_parsing() {
    lexer l("UNWIND ['a', 'b', 'c'] AS x CREATE (n:Item {name: x})");
    parser p(l.tokenize());
    auto stmt = p.parse();

    bool has_unwind = false;
    for (auto& c : stmt.clauses) {
        if (c->type() == clause_type::unwind) {
            has_unwind = true;
            auto uc = std::dynamic_pointer_cast<unwind_clause>(c);
            assert(uc->alias == "x");
            assert(uc->list_expr != nullptr);
            assert(uc->list_expr->type() == node_type::list_literal);
            auto ll = std::dynamic_pointer_cast<list_literal_expr>(uc->list_expr);
            assert(ll->elements.size() == 3);
        }
    }
    assert(has_unwind);
    std::cout << "test_unwind_clause_parsing: PASS" << std::endl;
}

// ======== Edge cases ========

void test_empty_string_property() {
    lexer l("CREATE (n:Person {name: ''})");
    parser p(l.tokenize());
    auto stmt = p.parse();
    auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    auto name_it = cr->paths[0].elements[0].node->properties.find("name");
    assert(name_it != cr->paths[0].elements[0].node->properties.end());
    auto lit = std::dynamic_pointer_cast<literal>(name_it->second);
    assert(lit != nullptr);
    assert(lit->value.empty());
    assert(lit->is_string);
    std::cout << "test_empty_string_property: PASS" << std::endl;
}

void test_numeric_boundary_values() {
    // Zero
    {
        lexer l("CREATE (n:Test {val: 0})");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
        auto it = cr->paths[0].elements[0].node->properties.find("val");
        assert(it != cr->paths[0].elements[0].node->properties.end());
        auto lit = std::dynamic_pointer_cast<literal>(it->second);
        assert(lit != nullptr);
        assert(lit->value == "0");
    }

    // Large number
    {
        lexer l("CREATE (n:Test {val: 999999})");
        parser p(l.tokenize());
        auto stmt = p.parse();
        auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
        auto it = cr->paths[0].elements[0].node->properties.find("val");
        auto lit = std::dynamic_pointer_cast<literal>(it->second);
        assert(lit->value == "999999");
    }

    std::cout << "test_numeric_boundary_values: PASS" << std::endl;
}

void test_large_number_in_where() {
    lexer l("MATCH (n:Test) WHERE n.val > 99999 RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();
    auto wc = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
    assert(wc->op == ">");
    std::cout << "test_large_number_in_where: PASS" << std::endl;
}

// ======== Compound queries ========

void test_compound_match_where_set_return() {
    lexer l("MATCH (n:Person) WHERE n.name = 'Alice' SET n.age = 31 RETURN n.name AS name");
    parser p(l.tokenize());
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 4);
    assert(stmt.clauses[0]->type() == clause_type::match);
    assert(stmt.clauses[1]->type() == clause_type::where);
    assert(stmt.clauses[2]->type() == clause_type::set_clause);
    assert(stmt.clauses[3]->type() == clause_type::return_clause);

    std::cout << "test_compound_match_where_set_return: PASS" << std::endl;
}

void test_create_edge_pattern() {
    lexer l("CREATE (a:Person {name: 'Alice'})-[r:KNOWS {since: 2020}]->(b:Person {name: 'Bob'})");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto cr = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    assert(cr->paths.size() == 1);
    assert(cr->paths[0].elements.size() >= 2);

    // First element: node a
    assert(cr->paths[0].elements[0].node->variable == "a");
    assert(cr->paths[0].elements[0].node->label == "Person");

    // Relationship
    assert(cr->paths[0].elements[0].relationship != nullptr);
    assert(cr->paths[0].elements[0].relationship->type == "KNOWS");
    assert(cr->paths[0].elements[0].relationship->direction == "->");

    std::cout << "test_create_edge_pattern: PASS" << std::endl;
}

// ======== Literal diversity ========

void test_single_quote_string() {
    lexer l("CREATE (n:Test {name: 'hello'})");
    auto tokens = l.tokenize();
    bool found_string = false;
    for (auto& t : tokens) {
        if (t.type == token_type::string_literal && t.text == "hello") {
            found_string = true;
        }
    }
    assert(found_string);
    std::cout << "test_single_quote_string: PASS" << std::endl;
}

void test_double_quote_string() {
    lexer l("CREATE (n:Test {name: \"hello\"})");
    auto tokens = l.tokenize();
    bool found_string = false;
    for (auto& t : tokens) {
        if (t.type == token_type::string_literal && t.text == "hello") {
            found_string = true;
        }
    }
    assert(found_string);
    std::cout << "test_double_quote_string: PASS" << std::endl;
}

void test_integer_literal() {
    lexer l("CREATE (n:Test {val: 42})");
    auto tokens = l.tokenize();
    bool found_num = false;
    for (auto& t : tokens) {
        if (t.type == token_type::number_literal && t.text == "42") {
            found_num = true;
        }
    }
    assert(found_num);
    std::cout << "test_integer_literal: PASS" << std::endl;
}

void test_float_literal() {
    lexer l("CREATE (n:Test {val: 3.14})");
    auto tokens = l.tokenize();
    bool found_num = false;
    for (auto& t : tokens) {
        if (t.type == token_type::number_literal && t.text == "3.14") {
            found_num = true;
        }
    }
    assert(found_num);
    std::cout << "test_float_literal: PASS" << std::endl;
}

// ======== Lexer token completeness ========

void test_all_token_types() {
    // Query that uses most token types
    lexer l("MATCH (n:Person)-[r:KNOWS]->(m) WHERE n.age > 30 SET n.name = 'X' DELETE m RETURN n.name AS alias");
    auto tokens = l.tokenize();

    bool has_match = false, has_where = false, has_set = false;
    bool has_delete = false, has_return = false, has_as = false;
    bool has_identifier = false, has_string = false, has_number = false;
    bool has_colon = false, has_dot = false, has_equals = false;
    bool has_greater = false, has_arrow = false;
    bool has_lparen = false, has_rparen = false;
    bool has_lbracket = false, has_rbracket = false;

    for (auto& t : tokens) {
        switch (t.type) {
            case token_type::keyword_match: has_match = true; break;
            case token_type::keyword_where: has_where = true; break;
            case token_type::keyword_set: has_set = true; break;
            case token_type::keyword_delete: has_delete = true; break;
            case token_type::keyword_return: has_return = true; break;
            case token_type::keyword_as: has_as = true; break;
            case token_type::identifier: has_identifier = true; break;
            case token_type::string_literal: has_string = true; break;
            case token_type::number_literal: has_number = true; break;
            case token_type::colon: has_colon = true; break;
            case token_type::dot: has_dot = true; break;
            case token_type::equals: has_equals = true; break;
            case token_type::greater_than: has_greater = true; break;
            case token_type::arrow_right: has_arrow = true; break;
            case token_type::lparen: has_lparen = true; break;
            case token_type::rparen: has_rparen = true; break;
            case token_type::lbracket: has_lbracket = true; break;
            case token_type::rbracket: has_rbracket = true; break;
            default: break;
        }
    }

    assert(has_match);
    assert(has_where);
    assert(has_set);
    assert(has_delete);
    assert(has_return);
    assert(has_as);
    assert(has_identifier);
    assert(has_string);
    assert(has_number);
    assert(has_colon);
    assert(has_dot);
    assert(has_equals);
    assert(has_greater);
    assert(has_arrow);
    assert(has_lparen);
    assert(has_rparen);
    assert(has_lbracket);
    assert(has_rbracket);

    std::cout << "test_all_token_types: PASS" << std::endl;
}

// ======== Property test: any valid query always produces non-empty clauses ========

void test_parse_always_has_clauses() {
    std::string queries[] = {
        "CREATE (n:Person)",
        "MATCH (n) RETURN n",
        "MATCH (n:Person) WHERE n.name = 'A' RETURN n",
        "MATCH (n:Person) DELETE n",
        "MATCH (n:Person) SET n.age = 30 RETURN n",
        "CREATE (a:P {name: 'A'})-[r:K]->(b:P {name: 'B'})",
    };
    for (auto& q : queries) {
        lexer l(q);
        parser p(l.tokenize());
        auto stmt = p.parse();
        assert(!stmt.clauses.empty());
    }
    std::cout << "test_parse_always_has_clauses: PASS" << std::endl;
}

int main() {
    // Deterministic parsing
    test_parse_deterministic();

    // All clause types
    test_create_clause_parsing();
    test_match_clause_parsing();
    test_where_clause_parsing();
    test_return_clause_parsing();
    test_set_clause_parsing();
    test_delete_clause_parsing();
    test_unwind_clause_parsing();

    // Edge cases
    test_empty_string_property();
    test_numeric_boundary_values();
    test_large_number_in_where();

    // Compound queries
    test_compound_match_where_set_return();
    test_create_edge_pattern();

    // Literal diversity
    test_single_quote_string();
    test_double_quote_string();
    test_integer_literal();
    test_float_literal();

    // Token completeness
    test_all_token_types();

    // Property test
    test_parse_always_has_clauses();

    std::cout << "\nAll parser property tests passed!" << std::endl;
    return 0;
}
