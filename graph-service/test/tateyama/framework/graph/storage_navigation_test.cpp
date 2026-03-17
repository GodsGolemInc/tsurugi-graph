#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <iostream>
#include <cassert>
#include <vector>

using namespace tateyama::framework::graph;

static void reset_mock() {
    sharksfin::mock_db_state.clear();
    sharksfin::mock_sequences.clear();
    sharksfin::mock_sequence_values.clear();
    sharksfin::mock_sequence_versions.clear();
    sharksfin::mock_next_sequence_id = 0;
    sharksfin::mock_iterators.clear();
    sharksfin::mock_iterators_end.clear();
    sharksfin::mock_iterators_started.clear();
}

void test_storage_navigation() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;

    assert(s.init(db, tx));

    uint64_t n1_id, n2_id, n3_id, e1_id, e2_id;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", n1_id));
    assert(s.create_node(tx, "Person", "{\"name\": \"Bob\"}", n2_id));
    assert(s.create_node(tx, "Person", "{\"name\": \"Charlie\"}", n3_id));

    // Alice knows Bob and Charlie
    assert(s.create_edge(tx, n1_id, n2_id, "KNOWS", "{}", e1_id));
    assert(s.create_edge(tx, n1_id, n3_id, "KNOWS", "{}", e2_id));

    std::vector<uint64_t> out_edges;
    assert(s.get_outgoing_edges(tx, n1_id, out_edges));
    assert(out_edges.size() == 2);
    assert(out_edges[0] == e1_id);
    assert(out_edges[1] == e2_id);

    // Test incoming edges
    std::vector<uint64_t> in_edges;
    assert(s.get_incoming_edges(tx, n2_id, in_edges));
    assert(in_edges.size() == 1);
    assert(in_edges[0] == e1_id);

    std::cout << "test_storage_navigation: PASS" << std::endl;
}

int main() {
    reset_mock();
    test_storage_navigation();
    return 0;
}
