#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <iostream>
#include <cassert>

using namespace tateyama::framework::graph;

void test_storage_lifecycle() {
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;

    assert(s.init(db, tx));
    
    uint64_t n1_id, n2_id, e1_id;
    assert(s.create_node(tx, "{\"name\": \"Alice\"}", n1_id));
    assert(s.create_node(tx, "{\"name\": \"Bob\"}", n2_id));
    
    std::string props;
    assert(s.get_node(tx, n1_id, props));
    assert(props == "{\"name\": \"Alice\"}");

    assert(s.create_edge(tx, n1_id, n2_id, "KNOWS", "{\"since\": \"2020\"}", e1_id));
    
    edge_data e;
    assert(s.get_edge(tx, e1_id, e));
    assert(e.from_id == n1_id);
    assert(e.to_id == n2_id);
    assert(e.label == "KNOWS");
    assert(e.properties == "{\"since\": \"2020\"}");

    std::cout << "test_storage_lifecycle: PASS" << std::endl;
}

int main() {
    test_storage_lifecycle();
    return 0;
}
