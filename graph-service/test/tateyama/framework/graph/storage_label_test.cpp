#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <iostream>
#include <cassert>
#include <vector>

using namespace tateyama::framework::graph;

void test_label_index() {
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    assert(s.init(db, tx));
    
    uint64_t n1, n2, n3;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", n1));
    assert(s.create_node(tx, "Person", "{\"name\": \"Bob\"}", n2));
    assert(s.create_node(tx, "City", "{\"name\": \"Tokyo\"}", n3));
    
    std::vector<uint64_t> persons;
    assert(s.find_nodes_by_label(tx, "Person", persons));
    assert(persons.size() == 2);
    assert(persons[0] == n1);
    assert(persons[1] == n2);

    std::vector<uint64_t> cities;
    assert(s.find_nodes_by_label(tx, "City", cities));
    assert(cities.size() == 1);
    assert(cities[0] == n3);

    std::cout << "test_label_index: PASS" << std::endl;
}

int main() {
    test_label_index();
    return 0;
}
