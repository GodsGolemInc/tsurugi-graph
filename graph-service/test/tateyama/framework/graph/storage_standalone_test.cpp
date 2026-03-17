#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <iostream>
#include <cassert>

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
    sharksfin::mock_commit_failure_count = 0;
}

void test_storage_lifecycle() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;

    assert(s.init(db, tx));

    uint64_t n1_id, n2_id, e1_id;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", n1_id));
    assert(s.create_node(tx, "Person", "{\"name\": \"Bob\"}", n2_id));

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

void test_update_node() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", id));
    assert(s.update_node(tx, id, "{\"name\": \"Alice\", \"age\": 31}"));

    std::string props;
    assert(s.get_node(tx, id, props));
    assert(props.find("31") != std::string::npos);

    std::cout << "test_update_node: PASS" << std::endl;
}

void test_delete_edge() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t n1, n2, eid;
    assert(s.create_node(tx, "Person", "{}", n1));
    assert(s.create_node(tx, "Person", "{}", n2));
    assert(s.create_edge(tx, n1, n2, "KNOWS", "{}", eid));

    assert(s.delete_edge(tx, eid));

    std::cout << "test_delete_edge: PASS" << std::endl;
}

void test_incoming_edges() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t n1, n2, eid;
    assert(s.create_node(tx, "", "{}", n1));
    assert(s.create_node(tx, "", "{}", n2));
    assert(s.create_edge(tx, n1, n2, "FOLLOWS", "{}", eid));

    std::vector<uint64_t> in_edges;
    assert(s.get_incoming_edges(tx, n2, in_edges));
    assert(!in_edges.empty());
    assert(in_edges[0] == eid);

    std::cout << "test_incoming_edges: PASS" << std::endl;
}

int main() {
    test_storage_lifecycle();
    test_update_node();
    test_delete_edge();
    test_incoming_edges();

    std::cout << "\nAll storage tests passed!" << std::endl;
    return 0;
}
