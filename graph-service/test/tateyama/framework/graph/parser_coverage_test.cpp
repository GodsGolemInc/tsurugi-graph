// parser_coverage_test.cpp — Tests for coverage gaps in parser/lexer
// Covers: >= and <= tokenization, <> operator parsing, undirected edge,
// multi-path CREATE, anonymous node, error paths

#include <cassert>
#include <iostream>
#include <string>
#include <stdexcept>

#include <tateyama/framework/graph/core/parser.h>

using namespace tateyama::framework::graph::core;

// Test: >= tokenization and WHERE parsing
void test_gte_operator() {
    lexer l("MATCH (n:N) WHERE n.val >= 10 RETURN n");
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    assert(stmt.clauses.size() == 3);
    auto where = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
    assert(where);
    assert(where->op == ">=");
    assert(where->variable == "n");
    assert(where->property == "val");

    auto val = std::dynamic_pointer_cast<literal>(where->value);
    assert(val && val->value == "10");

    std::cout << "test_gte_operator: PASS" << std::endl;
}

// Test: <= tokenization and WHERE parsing
void test_lte_operator() {
    lexer l("MATCH (n:N) WHERE n.val <= 5 RETURN n");
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    auto where = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
    assert(where);
    assert(where->op == "<=");

    std::cout << "test_lte_operator: PASS" << std::endl;
}

// Test: <> tokenization and WHERE parsing
void test_neq_operator() {
    lexer l("MATCH (n:N) WHERE n.val <> 3 RETURN n");
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    auto where = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
    assert(where);
    assert(where->op == "<>");

    std::cout << "test_neq_operator: PASS" << std::endl;
}

// Test: Undirected edge pattern -[r:TYPE]-
void test_undirected_edge() {
    lexer l("MATCH (a:A)-[r:LINK]-(b:B) RETURN a");
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    auto match = std::dynamic_pointer_cast<match_clause>(stmt.clauses[0]);
    assert(match);
    assert(match->paths[0].elements.size() == 2);
    auto& rel = match->paths[0].elements[0].relationship;
    assert(rel);
    assert(rel->direction == "-");
    assert(rel->type == "LINK");

    std::cout << "test_undirected_edge: PASS" << std::endl;
}

// Test: Multi-path CREATE with comma
void test_multi_path_create() {
    lexer l("CREATE (a:A {x: 1}), (b:B {y: 2})");
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    auto create = std::dynamic_pointer_cast<create_clause>(stmt.clauses[0]);
    assert(create);
    assert(create->paths.size() == 2);
    assert(create->paths[0].elements[0].node->label == "A");
    assert(create->paths[1].elements[0].node->label == "B");

    std::cout << "test_multi_path_create: PASS" << std::endl;
}

// Test: Anonymous node pattern ()
void test_anonymous_node() {
    lexer l("MATCH () RETURN *");
    auto tokens = l.tokenize();
    // This will fail at parse_expression for * but the node pattern should parse
    // Let's use a valid query instead
    lexer l2("CREATE ()");
    auto tokens2 = l2.tokenize();
    parser p2(tokens2);
    auto stmt2 = p2.parse();

    auto create = std::dynamic_pointer_cast<create_clause>(stmt2.clauses[0]);
    assert(create);
    assert(create->paths[0].elements[0].node->variable.empty());
    assert(create->paths[0].elements[0].node->label.empty());

    std::cout << "test_anonymous_node: PASS" << std::endl;
}

// Test: Unknown token at start of clause throws
void test_unknown_clause_throws() {
    lexer l("@@@");
    auto tokens = l.tokenize();
    parser p(tokens);
    bool threw = false;
    try {
        p.parse();
    } catch (const std::runtime_error& e) {
        threw = true;
        assert(std::string(e.what()).find("Unexpected token") != std::string::npos);
    }
    assert(threw);

    std::cout << "test_unknown_clause_throws: PASS" << std::endl;
}

// Test: Lexer unknown token type for unrecognized symbol
void test_unknown_token_type() {
    lexer l("~");
    auto tokens = l.tokenize();
    // Should have unknown token + eof
    assert(tokens.size() >= 2);
    assert(tokens[0].type == token_type::unknown);
    assert(tokens[0].text == "~");

    std::cout << "test_unknown_token_type: PASS" << std::endl;
}

// Test: All comparison operators tokenize correctly
void test_all_comparison_operators() {
    lexer l("= > < >= <= <>");
    auto tokens = l.tokenize();

    assert(tokens[0].type == token_type::equals);
    assert(tokens[0].text == "=");

    assert(tokens[1].type == token_type::greater_than);
    assert(tokens[1].text == ">");

    assert(tokens[2].type == token_type::less_than);
    assert(tokens[2].text == "<");

    assert(tokens[3].type == token_type::greater_than);
    assert(tokens[3].text == ">=");

    assert(tokens[4].type == token_type::less_than);
    assert(tokens[4].text == "<=");

    assert(tokens[5].type == token_type::greater_than);
    assert(tokens[5].text == "<>");

    std::cout << "test_all_comparison_operators: PASS" << std::endl;
}

// Test: WHERE with string literal comparison
void test_where_string_comparison() {
    lexer l("MATCH (n:N) WHERE n.name = 'Alice' RETURN n");
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    auto where = std::dynamic_pointer_cast<where_clause>(stmt.clauses[1]);
    assert(where);
    assert(where->op == "=");
    auto val = std::dynamic_pointer_cast<literal>(where->value);
    assert(val && val->value == "Alice" && val->is_string);

    std::cout << "test_where_string_comparison: PASS" << std::endl;
}

int main() {
    test_gte_operator();
    test_lte_operator();
    test_neq_operator();
    test_undirected_edge();
    test_multi_path_create();
    test_anonymous_node();
    test_unknown_clause_throws();
    test_unknown_token_type();
    test_all_comparison_operators();
    test_where_string_comparison();

    std::cout << "\nAll parser coverage tests passed!" << std::endl;
    return 0;
}
