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

    // Navigation & Search
    bool get_outgoing_edges(sharksfin::TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids);
    bool find_nodes_by_label(sharksfin::TransactionHandle tx, std::string_view label, std::vector<uint64_t>& out_node_ids);

private:
    sharksfin::StorageHandle nodes_handle_{};
    sharksfin::StorageHandle edges_handle_{};
    sharksfin::StorageHandle out_index_handle_{};
    sharksfin::StorageHandle in_index_handle_{};
    sharksfin::StorageHandle label_index_handle_{};
    sharksfin::SequenceHandle sequence_handle_{};
};

} // namespace tateyama::framework::graph
