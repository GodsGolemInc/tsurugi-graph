#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <sharksfin/api.h>

namespace tateyama::framework::graph {

class storage {
public:
    static constexpr std::string_view STORAGE_NAME_NODES = "graph_nodes";
    static constexpr std::string_view STORAGE_NAME_EDGES = "graph_edges";

    storage() = default;
    ~storage() = default;

    // Open/Create storages
    bool init(sharksfin::DatabaseHandle db_handle, sharksfin::TransactionHandle tx_handle);

    // Node operations
    bool create_node(sharksfin::TransactionHandle tx, std::string_view properties, uint64_t& out_id);
    bool get_node(sharksfin::TransactionHandle tx, uint64_t node_id, std::string& out_properties);

    // Edge operations
    // ... (To be implemented)

private:
    sharksfin::StorageHandle nodes_handle_{};
    sharksfin::StorageHandle edges_handle_{};
};

} // namespace tateyama::framework::graph
