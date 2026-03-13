#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <iostream>
#include <cassert>
#include <vector>

using namespace tateyama::framework::graph;

void test_storage_navigation() {
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;

    assert(s.init(db, tx));
    
    uint64_t n1_id, n2_id, n3_id, e1_id, e2_id;
    assert(s.create_node(tx, "{\"name\": \"Alice\"}", n1_id));
    assert(s.create_node(tx, "{\"name\": \"Bob\"}", n2_id));
    assert(s.create_node(tx, "{\"name\": \"Charlie\"}", n3_id));
    
    // Alice knows Bob and Charlie
    assert(s.create_edge(tx, n1_id, n2_id, "KNOWS", "{}", e1_id));
    assert(s.create_edge(tx, n1_id, n3_id, "KNOWS", "{}", e2_id));
    
    std::vector<uint64_t> out_edges;
    assert(s.get_outgoing_edges(tx, n1_id, out_edges));
    assert(out_edges.size() == 2);
    assert(out_edges[0] == e1_id);
    assert(out_edges[1] == e2_id);

    std::cout << "test_storage_navigation: PASS" << std::endl;
}

int main() {
    test_storage_navigation();
    return 0;
}
