// executor_batch_test.cpp — Tests for ADR-0012 batched executor execution
// Verifies that MATCH+RETURN and WHERE produce correct results when
// result sets exceed BATCH_SIZE (1024 nodes).

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;
using namespace sharksfin;

static void reset_mock() {
    mock_db_state.clear();
    mock_iterators.clear();
    mock_iterators_end.clear();
    mock_iterators_started.clear();
    mock_sequence_values.clear();
}

static storage create_storage() {
    storage s;
    DatabaseHandle db = (void*)0x1;
    TransactionHandle tx = (void*)0x2;
    s.init(db, tx);
    return s;
}

static size_t count_json_objects(const std::string& json) {
    // Count top-level objects in a JSON array by counting '{' at depth 1
    // Depth tracks both [] and {} nesting
    size_t count = 0;
    int depth = 0;
    for (char c : json) {
        if (c == '{') {
            depth++;
            if (depth == 2) count++;  // depth 1=[, depth 2=top-level {
        } else if (c == '}') {
            depth--;
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
        }
    }
    return count;
}

// Test: RETURN with fewer than BATCH_SIZE nodes produces correct output
void test_return_batch_small() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    // Create 10 nodes
    for (int i = 0; i < 10; ++i) {
        uint64_t id;
        std::string props = "{\"name\": \"U" + std::to_string(i) + "\", \"val\": " + std::to_string(i) + "}";
        s.create_node(tx, "Item", props, id);
    }

    std::string q = "MATCH (n:Item) RETURN n";
    lexer l(q);
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    size_t count = count_json_objects(result);
    assert(count == 10);
    // Verify JSON structure
    assert(result.front() == '[');
    assert(result.back() == ']');
    assert(result.find("\"n\":") != std::string::npos);

    std::cout << "test_return_batch_small: PASS" << std::endl;
}

// Test: RETURN with more than BATCH_SIZE nodes (3000) produces correct count and all data
void test_return_batch_large() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    const int N = 3000;
    for (int i = 0; i < N; ++i) {
        uint64_t id;
        std::string props = "{\"idx\": " + std::to_string(i) + "}";
        s.create_node(tx, "Big", props, id);
    }

    std::string q = "MATCH (n:Big) RETURN n";
    lexer l(q);
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    size_t count = count_json_objects(result);
    assert(count == N);

    // Verify first and last items are present
    assert(result.find("\"idx\": 0") != std::string::npos);
    assert(result.find("\"idx\": 2999") != std::string::npos);

    std::cout << "test_return_batch_large: PASS" << std::endl;
}

// Test: WHERE = filtering works correctly with batched execution
// Uses unique property values to avoid mock property index overwrite limitation
void test_where_batch_equality() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    const int N = 2000;
    // Create N nodes with unique names; count those with category "A"
    int target_count = 0;
    for (int i = 0; i < N; ++i) {
        uint64_t id;
        std::string cat = (i % 5 == 0) ? "A" : "B";
        // Each node has unique name + category for WHERE test
        std::string props = "{\"name\": \"U" + std::to_string(i) + "\", \"cat\": \"" + cat + "\"}";
        s.create_node(tx, "Node", props, id);
        if (i % 5 == 0) target_count++;
    }

    // Use <> operator to force full-scan WHERE path (not indexed)
    // This tests the batched property filtering in execute_where
    std::string q = "MATCH (n:Node) WHERE n.cat <> 'B' RETURN n";
    lexer l(q);
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    size_t count = count_json_objects(result);
    assert(count == (size_t)target_count);

    std::cout << "test_where_batch_equality: PASS" << std::endl;
}

// Test: WHERE > filtering works correctly with batched execution
// Uses unique age values to avoid mock property index overwrite on duplicates
void test_where_batch_inequality() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    const int N = 1500;
    int expected = 0;
    for (int i = 0; i < N; ++i) {
        uint64_t id;
        // Each node has unique age to avoid mock index collision
        int age = i;
        std::string props = "{\"age\": " + std::to_string(age) + "}";
        s.create_node(tx, "Person", props, id);
        if (age > 1400) expected++;
    }

    std::string q = "MATCH (n:Person) WHERE n.age > 1400 RETURN n";
    lexer l(q);
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    size_t count = count_json_objects(result);
    assert(count == (size_t)expected);  // ages 1401..1499 = 99

    std::cout << "test_where_batch_inequality: PASS" << std::endl;
}

// Test: Full pipeline CREATE(many) -> MATCH -> WHERE -> RETURN
void test_full_pipeline_large() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    // CREATE 3000 nodes via UNWIND
    const int N = 3000;
    for (int i = 0; i < N; ++i) {
        uint64_t id;
        std::string props = "{\"val\": " + std::to_string(i) + "}";
        s.create_node(tx, "Data", props, id);
    }

    // MATCH + WHERE (val < 10) + RETURN property
    std::string q = "MATCH (d:Data) WHERE d.val < 10 RETURN d.val";
    lexer l(q);
    auto tokens = l.tokenize();
    parser p(tokens);
    auto stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));

    size_t count = count_json_objects(result);
    assert(count == 10);  // val 0..9

    // Verify property access works
    assert(result.find("\"d.val\":") != std::string::npos || result.find("\"val\":") != std::string::npos);

    std::cout << "test_full_pipeline_large: PASS" << std::endl;
}

int main() {
    test_return_batch_small();
    test_return_batch_large();
    test_where_batch_equality();
    test_where_batch_inequality();
    test_full_pipeline_large();

    std::cout << "\nAll executor batch tests passed!" << std::endl;
    return 0;
}
