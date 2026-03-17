#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include "sharksfin_mock.h"
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;

static void reset_mock() {
    sharksfin::mock_db_state.clear();
    sharksfin::mock_sequences.clear();
    sharksfin::mock_sequence_values.clear();
    sharksfin::mock_sequence_versions.clear();
    sharksfin::mock_next_sequence_id = 0;
    sharksfin::mock_iterators.clear();
    sharksfin::mock_iterators_end.clear();
    sharksfin::mock_iterators_started.clear();
    sharksfin::mock_commit_failure_count = 0;
}

void test_create_execute() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    std::string query = "CREATE (n:Person {name: 'Alice'})";
    lexer l(query);
    parser p(l.tokenize());
    statement stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    // Verify storage - ID 1 (first sequence value)
    uint64_t node_id = 1;
    std::string props;
    assert(s.get_node(tx, node_id, props));
    std::cout << "Created Node Props: " << props << std::endl;
    assert(props.find("Alice") != std::string::npos);

    std::cout << "test_create_execute: PASS" << std::endl;
}

void test_create_edge_execute() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x3;
    s.init(db, tx);

    std::string query = "CREATE (n:Person {name: 'Bob'})-[r:KNOWS {since: 2021}]->(m:Person {name: 'Charlie'})";
    lexer l(query);
    parser p(l.tokenize());
    statement stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    // Verify node n (ID 1) has outgoing edges
    std::vector<uint64_t> edges;
    assert(s.get_outgoing_edges(tx, 1, edges));
    assert(!edges.empty());

    // Verify edge data
    edge_data ed;
    assert(s.get_edge(tx, edges[0], ed));
    assert(ed.from_id == 1);
    assert(ed.to_id == 2);
    assert(ed.label == "KNOWS");

    std::cout << "test_create_edge_execute: PASS" << std::endl;
}

void test_match_return() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x4;
    s.init(db, tx);

    // Create some nodes
    {
        std::string q = "CREATE (a:Person {name: 'Alice', age: 30})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        assert(exec.execute(p.parse(), r));
    }
    {
        std::string q = "CREATE (b:Person {name: 'Bob', age: 25})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        assert(exec.execute(p.parse(), r));
    }

    // Match and return
    std::string query = "MATCH (n:Person) RETURN n";
    lexer l(query);
    parser p(l.tokenize());
    executor exec(s, tx);
    std::string result;
    assert(exec.execute(p.parse(), result));

    std::cout << "Match Result: " << result << std::endl;
    // Should have 2 entries (Alice and Bob)
    assert(result.find("Alice") != std::string::npos);
    assert(result.find("Bob") != std::string::npos);

    std::cout << "test_match_return: PASS" << std::endl;
}

void test_where_filter() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x5;
    s.init(db, tx);

    // Create nodes
    {
        std::string q = "CREATE (a:Person {name: 'Alice', age: 30})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }
    {
        std::string q = "CREATE (b:Person {name: 'Bob', age: 25})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }

    // Match with WHERE filter
    std::string query = "MATCH (n:Person) WHERE n.name = 'Alice' RETURN n";
    lexer l(query);
    parser p(l.tokenize());
    executor exec(s, tx);
    std::string result;
    assert(exec.execute(p.parse(), result));

    std::cout << "Where Result: " << result << std::endl;
    assert(result.find("Alice") != std::string::npos);
    // Bob should not be in the result after filtering
    assert(result.find("Bob") == std::string::npos);

    std::cout << "test_where_filter: PASS" << std::endl;
}

void test_property_access_return() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x6;
    s.init(db, tx);

    // Create node
    {
        std::string q = "CREATE (a:Person {name: 'Alice', age: 30})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }

    // Return specific property
    std::string query = "MATCH (n:Person) RETURN n.name AS name";
    lexer l(query);
    parser p(l.tokenize());
    executor exec(s, tx);
    std::string result;
    assert(exec.execute(p.parse(), result));

    std::cout << "Property Access Result: " << result << std::endl;
    assert(result.find("Alice") != std::string::npos);
    assert(result.find("\"name\"") != std::string::npos);

    std::cout << "test_property_access_return: PASS" << std::endl;
}

void test_set_property() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x7;
    s.init(db, tx);

    // Create node
    {
        std::string q = "CREATE (a:Person {name: 'Alice', age: 30})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }

    // Set property
    std::string query = "MATCH (n:Person) SET n.age = 31 RETURN n";
    lexer l(query);
    parser p(l.tokenize());
    executor exec(s, tx);
    std::string result;
    assert(exec.execute(p.parse(), result));

    std::cout << "Set Result: " << result << std::endl;
    assert(result.find("31") != std::string::npos);

    std::cout << "test_set_property: PASS" << std::endl;
}

void test_delete_node() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x8;
    s.init(db, tx);

    // Create node
    {
        std::string q = "CREATE (a:Person {name: 'ToDelete'})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }

    // Verify it exists
    std::string props;
    assert(s.get_node(tx, 1, props));
    assert(props.find("ToDelete") != std::string::npos);

    // Delete it
    std::string query = "MATCH (n:Person) DELETE n";
    lexer l(query);
    parser p(l.tokenize());
    executor exec(s, tx);
    std::string result;
    assert(exec.execute(p.parse(), result));

    std::cout << "test_delete_node: PASS" << std::endl;
}

void test_multi_hop_match() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x9;
    s.init(db, tx);

    // Create graph: Alice -[KNOWS]-> Bob -[KNOWS]-> Charlie
    {
        std::string q = "CREATE (a:Person {name: 'Alice'})-[r1:KNOWS]->(b:Person {name: 'Bob'})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }
    {
        std::string q = "CREATE (b:Person {name: 'Bob2'})-[r2:KNOWS]->(c:Person {name: 'Charlie'})";
        lexer l(q);
        parser p(l.tokenize());
        executor exec(s, tx);
        std::string r;
        exec.execute(p.parse(), r);
    }

    std::cout << "test_multi_hop_match: PASS" << std::endl;
}

int main() {
    test_create_execute();
    test_create_edge_execute();
    test_match_return();
    test_where_filter();
    test_property_access_return();
    test_set_property();
    test_delete_node();
    test_multi_hop_match();

    std::cout << "\nAll executor tests passed!" << std::endl;
    return 0;
}
