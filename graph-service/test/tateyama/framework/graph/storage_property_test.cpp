#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>
#include <iostream>
#include <cassert>
#include <vector>
#include <algorithm>

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

// ======== update_node_with_label ========

void test_update_node_with_label() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\", \"age\": 30}", id));

    // Verify property index was created
    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property(tx, "Person", "name", "Alice", found));
    assert(std::find(found.begin(), found.end(), id) != found.end());

    // Update with label
    assert(s.update_node_with_label(tx, id, "Person", "{\"name\": \"Bob\", \"age\": 31}"));

    // Old property index should be removed, new one created
    found.clear();
    s.find_nodes_by_property(tx, "Person", "name", "Alice", found);
    assert(std::find(found.begin(), found.end(), id) == found.end());

    found.clear();
    assert(s.find_nodes_by_property(tx, "Person", "name", "Bob", found));
    assert(std::find(found.begin(), found.end(), id) != found.end());

    // Verify node data
    std::string props;
    assert(s.get_node(tx, id, props));
    assert(props.find("Bob") != std::string::npos);

    std::cout << "test_update_node_with_label: PASS" << std::endl;
}

// ======== delete_node ========

void test_delete_node() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id;
    assert(s.create_node(tx, "Person", "{\"name\": \"ToDelete\"}", id));

    // Verify exists
    std::string props;
    assert(s.get_node(tx, id, props));

    // Delete
    assert(s.delete_node(tx, id, "Person"));

    // Verify node data is cleared (empty slice stored)
    props.clear();
    s.get_node(tx, id, props);
    // After deletion, properties should be empty
    assert(props.empty());

    std::cout << "test_delete_node: PASS" << std::endl;
}

// ======== index_node_properties ========

void test_index_node_properties() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id;
    assert(s.create_node(tx, "Employee", "{\"dept\": \"Engineering\", \"level\": 5}", id));

    // index_node_properties is called internally by create_node, but test directly
    // Create a second node and manually index
    uint64_t id2;
    assert(s.create_node(tx, "Employee", "{\"dept\": \"Sales\", \"level\": 3}", id2));

    // Find by property
    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property(tx, "Employee", "dept", "Engineering", found));
    assert(std::find(found.begin(), found.end(), id) != found.end());
    assert(std::find(found.begin(), found.end(), id2) == found.end());

    found.clear();
    assert(s.find_nodes_by_property(tx, "Employee", "dept", "Sales", found));
    assert(std::find(found.begin(), found.end(), id2) != found.end());

    std::cout << "test_index_node_properties: PASS" << std::endl;
}

// ======== find_nodes_by_property ========

void test_find_nodes_by_property() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", id1));
    assert(s.create_node(tx, "Person", "{\"name\": \"Bob\"}", id2));
    assert(s.create_node(tx, "Person", "{\"name\": \"Charlie\"}", id3));

    // Each unique property value should find exactly one node
    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property(tx, "Person", "name", "Alice", found));
    assert(!found.empty());
    assert(std::find(found.begin(), found.end(), id1) != found.end());

    found.clear();
    assert(s.find_nodes_by_property(tx, "Person", "name", "Bob", found));
    assert(!found.empty());
    assert(std::find(found.begin(), found.end(), id2) != found.end());

    // Non-existent value
    found.clear();
    s.find_nodes_by_property(tx, "Person", "name", "Nobody", found);
    assert(found.empty());

    std::cout << "test_find_nodes_by_property: PASS" << std::endl;
}

// ======== find_nodes_by_property_range ========

void test_find_nodes_by_property_range_gt() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{\"name\": \"A\", \"age\": 20}", id1));
    assert(s.create_node(tx, "Person", "{\"name\": \"B\", \"age\": 30}", id2));
    assert(s.create_node(tx, "Person", "{\"name\": \"C\", \"age\": 40}", id3));

    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property_range(tx, "Person", "age", ">", "25", found));
    assert(std::find(found.begin(), found.end(), id2) != found.end());
    assert(std::find(found.begin(), found.end(), id3) != found.end());
    assert(std::find(found.begin(), found.end(), id1) == found.end());

    std::cout << "test_find_nodes_by_property_range_gt: PASS" << std::endl;
}

void test_find_nodes_by_property_range_lt() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{\"name\": \"A\", \"age\": 20}", id1));
    assert(s.create_node(tx, "Person", "{\"name\": \"B\", \"age\": 30}", id2));
    assert(s.create_node(tx, "Person", "{\"name\": \"C\", \"age\": 40}", id3));

    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property_range(tx, "Person", "age", "<", "35", found));
    assert(std::find(found.begin(), found.end(), id1) != found.end());
    assert(std::find(found.begin(), found.end(), id2) != found.end());
    assert(std::find(found.begin(), found.end(), id3) == found.end());

    std::cout << "test_find_nodes_by_property_range_lt: PASS" << std::endl;
}

void test_find_nodes_by_property_range_gt_boundary() {
    // Test boundary: exact match should NOT be included with >
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{\"name\": \"A\", \"age\": 20}", id1));
    assert(s.create_node(tx, "Person", "{\"name\": \"B\", \"age\": 30}", id2));
    assert(s.create_node(tx, "Person", "{\"name\": \"C\", \"age\": 40}", id3));

    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property_range(tx, "Person", "age", ">", "30", found));
    // Only id3 (age=40) should match > 30
    assert(std::find(found.begin(), found.end(), id3) != found.end());
    assert(std::find(found.begin(), found.end(), id2) == found.end()); // age=30 NOT > 30

    std::cout << "test_find_nodes_by_property_range_gt_boundary: PASS" << std::endl;
}

void test_find_nodes_by_property_range_lt_boundary() {
    // Test boundary: exact match should NOT be included with <
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{\"name\": \"A\", \"age\": 20}", id1));
    assert(s.create_node(tx, "Person", "{\"name\": \"B\", \"age\": 30}", id2));
    assert(s.create_node(tx, "Person", "{\"name\": \"C\", \"age\": 40}", id3));

    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property_range(tx, "Person", "age", "<", "30", found));
    // Only id1 (age=20) should match < 30
    assert(std::find(found.begin(), found.end(), id1) != found.end());
    assert(std::find(found.begin(), found.end(), id2) == found.end()); // age=30 NOT < 30

    std::cout << "test_find_nodes_by_property_range_lt_boundary: PASS" << std::endl;
}

// ======== remove_property_index ========

void test_remove_property_index() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", id));

    // Verify indexed
    std::vector<uint64_t> found;
    assert(s.find_nodes_by_property(tx, "Person", "name", "Alice", found));
    assert(!found.empty());

    // Remove index
    assert(s.remove_property_index(tx, id, "Person", "{\"name\": \"Alice\"}"));

    // Verify removed
    found.clear();
    s.find_nodes_by_property(tx, "Person", "name", "Alice", found);
    assert(found.empty());

    std::cout << "test_remove_property_index: PASS" << std::endl;
}

// ======== get_nodes_batch ========

void test_get_nodes_batch() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", id1));
    assert(s.create_node(tx, "Person", "{\"name\": \"Bob\"}", id2));
    assert(s.create_node(tx, "Person", "{\"name\": \"Charlie\"}", id3));

    std::vector<uint64_t> ids = {id1, id3};
    std::vector<std::pair<uint64_t, std::string>> results;
    assert(s.get_nodes_batch(tx, ids, results));
    assert(results.size() == 2);

    bool has_alice = false, has_charlie = false;
    for (auto& [nid, props] : results) {
        if (props.find("Alice") != std::string::npos) has_alice = true;
        if (props.find("Charlie") != std::string::npos) has_charlie = true;
    }
    assert(has_alice);
    assert(has_charlie);

    std::cout << "test_get_nodes_batch: PASS" << std::endl;
}

void test_get_nodes_batch_with_missing() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1;
    assert(s.create_node(tx, "Person", "{\"name\": \"Alice\"}", id1));

    std::vector<uint64_t> ids = {id1, 9999}; // 9999 doesn't exist
    std::vector<std::pair<uint64_t, std::string>> results;
    s.get_nodes_batch(tx, ids, results);
    // Should return at least the existing one
    assert(results.size() >= 1);

    std::cout << "test_get_nodes_batch_with_missing: PASS" << std::endl;
}

// ======== storage handle accessors ========

void test_storage_handle_accessors() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    assert(s.nodes_storage() != nullptr);
    assert(s.edges_storage() != nullptr);
    assert(s.out_index_storage() != nullptr);
    assert(s.in_index_storage() != nullptr);
    assert(s.label_index_storage() != nullptr);
    assert(s.property_index_storage() != nullptr);

    std::cout << "test_storage_handle_accessors: PASS" << std::endl;
}

// ======== find_nodes_by_label (complement) ========

void test_find_nodes_by_label_multi() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id1, id2, id3;
    assert(s.create_node(tx, "Person", "{}", id1));
    assert(s.create_node(tx, "Company", "{}", id2));
    assert(s.create_node(tx, "Person", "{}", id3));

    std::vector<uint64_t> persons;
    assert(s.find_nodes_by_label(tx, "Person", persons));
    assert(persons.size() == 2);

    std::vector<uint64_t> companies;
    assert(s.find_nodes_by_label(tx, "Company", companies));
    assert(companies.size() == 1);

    std::cout << "test_find_nodes_by_label_multi: PASS" << std::endl;
}

// ======== Property tests: CRUD round-trips ========

void test_create_get_roundtrip() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    std::string test_props[] = {
        "{\"name\": \"Alice\"}",
        "{\"x\": 1, \"y\": 2}",
        "{\"empty\": \"\"}",
    };
    for (auto& p : test_props) {
        uint64_t id;
        assert(s.create_node(tx, "Test", p, id));
        std::string got;
        assert(s.get_node(tx, id, got));
        assert(got == p);
    }

    std::cout << "test_create_get_roundtrip: PASS" << std::endl;
}

void test_create_delete_get_empty() {
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    uint64_t id;
    assert(s.create_node(tx, "Person", "{\"name\": \"Gone\"}", id));
    assert(s.delete_node(tx, id, "Person"));

    // After deletion, get_node should return empty properties
    std::string props;
    s.get_node(tx, id, props);
    assert(props.empty());

    std::cout << "test_create_delete_get_empty: PASS" << std::endl;
}

void test_index_find_consistency() {
    // Property test: every created node should be findable by its property index
    reset_mock();
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);

    for (int i = 0; i < 10; ++i) {
        uint64_t id;
        std::string name = "User" + std::to_string(i);
        std::string props = "{\"name\": \"" + name + "\"}";
        assert(s.create_node(tx, "Person", props, id));

        std::vector<uint64_t> found;
        assert(s.find_nodes_by_property(tx, "Person", "name", name, found));
        assert(std::find(found.begin(), found.end(), id) != found.end());
    }

    std::cout << "test_index_find_consistency: PASS" << std::endl;
}

int main() {
    test_update_node_with_label();
    test_delete_node();
    test_index_node_properties();
    test_find_nodes_by_property();
    test_find_nodes_by_property_range_gt();
    test_find_nodes_by_property_range_lt();
    test_find_nodes_by_property_range_gt_boundary();
    test_find_nodes_by_property_range_lt_boundary();
    test_remove_property_index();
    test_get_nodes_batch();
    test_get_nodes_batch_with_missing();
    test_storage_handle_accessors();
    test_find_nodes_by_label_multi();

    // Property tests
    test_create_get_roundtrip();
    test_create_delete_get_empty();
    test_index_find_consistency();

    std::cout << "\nAll storage property tests passed!" << std::endl;
    return 0;
}
