// storage_iterator_test.cpp — Tests for ADR-0012 label_iterator streaming scan
// Verifies that label_iterator produces the same results as find_nodes_by_label
// without materializing all IDs at once.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <set>

#include "sharksfin_mock.h"
#include <tateyama/framework/graph/storage.h>

using namespace tateyama::framework::graph;
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

// Test: Basic iteration over 100 nodes returns all created IDs
void test_label_iterator_basic() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    std::set<uint64_t> created_ids;
    for (int i = 0; i < 100; ++i) {
        uint64_t id;
        std::string props = "{\"i\": " + std::to_string(i) + "}";
        s.create_node(tx, "Node", props, id);
        created_ids.insert(id);
    }

    // Use iterator
    auto iter = s.find_nodes_by_label_iter(tx, "Node");
    std::set<uint64_t> iterated_ids;
    while (iter.next()) {
        iterated_ids.insert(iter.node_id());
    }

    assert(iterated_ids == created_ids);

    // Cross-check with find_nodes_by_label
    std::vector<uint64_t> vec_ids;
    s.find_nodes_by_label(tx, "Node", vec_ids);
    std::set<uint64_t> vec_set(vec_ids.begin(), vec_ids.end());
    assert(vec_set == created_ids);

    std::cout << "test_label_iterator_basic: PASS" << std::endl;
}

// Test: Empty label returns no results
void test_label_iterator_empty() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    // Create some nodes with a different label
    uint64_t id;
    s.create_node(tx, "Exists", "{}", id);

    auto iter = s.find_nodes_by_label_iter(tx, "Missing");
    assert(!iter.next());

    std::cout << "test_label_iterator_empty: PASS" << std::endl;
}

// Test: Mixed labels — iterator only returns matching label
void test_label_iterator_mixed_labels() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    std::set<uint64_t> person_ids, company_ids;
    for (int i = 0; i < 50; ++i) {
        uint64_t id;
        s.create_node(tx, "Person", "{\"n\": " + std::to_string(i) + "}", id);
        person_ids.insert(id);
    }
    for (int i = 0; i < 30; ++i) {
        uint64_t id;
        s.create_node(tx, "Company", "{\"n\": " + std::to_string(i) + "}", id);
        company_ids.insert(id);
    }

    // Iterate Person
    auto iter_p = s.find_nodes_by_label_iter(tx, "Person");
    std::set<uint64_t> got_person;
    while (iter_p.next()) got_person.insert(iter_p.node_id());
    assert(got_person == person_ids);

    // Iterate Company
    auto iter_c = s.find_nodes_by_label_iter(tx, "Company");
    std::set<uint64_t> got_company;
    while (iter_c.next()) got_company.insert(iter_c.node_id());
    assert(got_company == company_ids);

    std::cout << "test_label_iterator_mixed_labels: PASS" << std::endl;
}

// Test: get_properties() returns correct node properties lazily
void test_label_iterator_properties() {
    reset_mock();
    auto s = create_storage();
    TransactionHandle tx = (void*)0x2;

    uint64_t id1, id2;
    s.create_node(tx, "Item", "{\"name\": \"Alpha\"}", id1);
    s.create_node(tx, "Item", "{\"name\": \"Beta\"}", id2);

    auto iter = s.find_nodes_by_label_iter(tx, "Item");
    std::vector<std::string> props_list;
    while (iter.next()) {
        std::string props;
        assert(iter.get_properties(props));
        props_list.push_back(props);
    }

    assert(props_list.size() == 2);
    // Check both properties were retrieved (order may vary)
    bool found_alpha = false, found_beta = false;
    for (auto& p : props_list) {
        if (p.find("Alpha") != std::string::npos) found_alpha = true;
        if (p.find("Beta") != std::string::npos) found_beta = true;
    }
    assert(found_alpha && found_beta);

    std::cout << "test_label_iterator_properties: PASS" << std::endl;
}

int main() {
    test_label_iterator_basic();
    test_label_iterator_empty();
    test_label_iterator_mixed_labels();
    test_label_iterator_properties();

    std::cout << "\nAll storage iterator tests passed!" << std::endl;
    return 0;
}
