#include <tateyama/framework/graph/storage.h>
#include <glog/logging.h>
#include <cstring>
#include <iostream>

namespace tateyama::framework::graph {

using namespace sharksfin;

bool storage::init(DatabaseHandle db_handle, TransactionHandle tx_handle) {
    if (!db_handle) return false;

    // Create/Open storage for Nodes
    StorageOptions options;
    StatusCode rc = storage_create(tx_handle, Slice(STORAGE_NAME_NODES), options, &nodes_handle_);
    if (rc != StatusCode::OK && rc != StatusCode::ALREADY_EXISTS) {
        LOG(ERROR) << "Failed to create node storage: " << rc;
        return false;
    }
    // If it already exists, we need to open it (storage_create usually handles open if existing, but API might differ slightly)
    if (rc == StatusCode::ALREADY_EXISTS) {
        rc = storage_get(tx_handle, Slice(STORAGE_NAME_NODES), &nodes_handle_);
        if (rc != StatusCode::OK) {
             LOG(ERROR) << "Failed to open existing node storage: " << rc;
             return false;
        }
    }

    // Create/Open storage for Edges (Similar logic)
    rc = storage_create(tx_handle, Slice(STORAGE_NAME_EDGES), options, &edges_handle_);
    if (rc != StatusCode::OK && rc != StatusCode::ALREADY_EXISTS) {
        LOG(ERROR) << "Failed to create edge storage: " << rc;
        return false;
    }
    if (rc == StatusCode::ALREADY_EXISTS) {
        rc = storage_get(tx_handle, Slice(STORAGE_NAME_EDGES), &edges_handle_);
        if (rc != StatusCode::OK) {
             LOG(ERROR) << "Failed to open existing edge storage: " << rc;
             return false;
        }
    }

    return true;
}

static uint64_t generate_id() {
    // Ideally use sequence_get from sharksfin, but for prototype we use simple counter or random
    static uint64_t counter = 1;
    return counter++;
}

bool storage::create_node(TransactionHandle tx, std::string_view properties, uint64_t& out_id) {
    if (!tx || !nodes_handle_) return false;

    out_id = generate_id();
    
    // Key: Node ID (uint64_t big endian for sort order, but sharksfin slice is byte array)
    // We need to serialize uint64_t to Slice.
    uint64_t key_val = out_id; // Simple copy for now. Real implementation should handle endianness if needed.
    Slice key(&key_val, sizeof(key_val));
    Slice value(properties.data(), properties.size());

    StatusCode rc = content_put(tx, nodes_handle_, key, value, PutOperation::CREATE);
    if (rc != StatusCode::OK) {
        LOG(ERROR) << "Failed to put node: " << rc;
        return false;
    }
    return true;
}

bool storage::get_node(TransactionHandle tx, uint64_t node_id, std::string& out_properties) {
    if (!tx || !nodes_handle_) return false;

    uint64_t key_val = node_id;
    Slice key(&key_val, sizeof(key_val));
    Slice value;

    StatusCode rc = content_get(tx, nodes_handle_, key, &value);
    if (rc == StatusCode::NOT_FOUND) {
        return false;
    }
    if (rc != StatusCode::OK) {
        LOG(ERROR) << "Failed to get node: " << rc;
        return false;
    }
    out_properties.assign(value.data<char>(), value.size());
    return true;
}

} // namespace tateyama::framework::graph
