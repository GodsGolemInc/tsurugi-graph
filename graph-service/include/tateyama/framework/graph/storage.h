#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <sharksfin/api.h>

namespace tateyama::framework::graph {

struct edge_data {
    uint64_t from_id;
    uint64_t to_id;
    std::string label;
    std::string properties;
};

class storage {
public:
    static constexpr std::string_view STORAGE_NAME_NODES = "graph_nodes";
    static constexpr std::string_view STORAGE_NAME_EDGES = "graph_edges";
    static constexpr std::string_view STORAGE_NAME_OUT_INDEX = "graph_out_index";
    static constexpr std::string_view STORAGE_NAME_IN_INDEX = "graph_in_index";
    static constexpr std::string_view STORAGE_NAME_LABEL_INDEX = "graph_label_index";
    static constexpr std::string_view STORAGE_NAME_PROPERTY_INDEX = "graph_prop_index";
    static constexpr std::string_view SEQUENCE_NAME = "graph_id_sequence";

    storage() = default;
    ~storage() = default;

    // Open/Create storages
    bool init(sharksfin::DatabaseHandle db_handle, sharksfin::TransactionHandle tx_handle);

    // Node operations
    bool create_node(sharksfin::TransactionHandle tx, std::string_view label, std::string_view properties, uint64_t& out_id);
    bool get_node(sharksfin::TransactionHandle tx, uint64_t node_id, std::string& out_properties);

    // Edge operations
    bool create_edge(sharksfin::TransactionHandle tx, uint64_t from_id, uint64_t to_id, std::string_view label, std::string_view properties, uint64_t& out_id);
    bool get_edge(sharksfin::TransactionHandle tx, uint64_t edge_id, edge_data& out_edge);

    // Update operations
    bool update_node(sharksfin::TransactionHandle tx, uint64_t node_id, std::string_view properties);
    bool update_node_with_label(sharksfin::TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view new_properties);
    bool delete_node(sharksfin::TransactionHandle tx, uint64_t node_id, std::string_view label);
    bool delete_edge(sharksfin::TransactionHandle tx, uint64_t edge_id);

    // Navigation & Search
    bool get_outgoing_edges(sharksfin::TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids);
    bool get_incoming_edges(sharksfin::TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids);
    bool find_nodes_by_label(sharksfin::TransactionHandle tx, std::string_view label, std::vector<uint64_t>& out_node_ids);

    // Property index operations (ADR-0002)
    bool index_node_properties(sharksfin::TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view properties);
    bool find_nodes_by_property(sharksfin::TransactionHandle tx, std::string_view label, std::string_view prop_key, std::string_view prop_value, std::vector<uint64_t>& out_node_ids);
    bool remove_property_index(sharksfin::TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view properties);

    // Batch operations (ADR-0004)
    bool get_nodes_batch(sharksfin::TransactionHandle tx, const std::vector<uint64_t>& node_ids, std::vector<std::pair<uint64_t, std::string>>& out_results);

private:
    sharksfin::StorageHandle nodes_handle_{};
    sharksfin::StorageHandle edges_handle_{};
    sharksfin::StorageHandle out_index_handle_{};
    sharksfin::StorageHandle in_index_handle_{};
    sharksfin::StorageHandle label_index_handle_{};
    sharksfin::StorageHandle property_index_handle_{};
    sharksfin::SequenceHandle sequence_handle_{};
};

} // namespace tateyama::framework::graph
