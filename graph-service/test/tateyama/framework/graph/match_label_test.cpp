#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include "sharksfin_mock.h"
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;

void test_match_by_label_execute() {
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    // 1. Setup data: CREATE (n:Person {name: 'Alice'})
    uint64_t n1;
    s.create_node(tx, "Person", "{\"name\": \"Alice\"}", n1);

    // 2. Execute MATCH (p:Person) RETURN p
    std::string query = "MATCH (p:Person) RETURN p";
    lexer l(query);
    parser p(l.tokenize());
    statement stmt = p.parse();

    executor exec(s, tx);
    std::string result;
    assert(exec.execute(stmt, result));
    
    std::cout << "Match Result: " << result << std::endl;
    assert(result.find("Alice") != std::string::npos);
    
    std::cout << "test_match_by_label_execute: PASS" << std::endl;
}

int main() {
    test_match_by_label_execute();
    return 0;
}
