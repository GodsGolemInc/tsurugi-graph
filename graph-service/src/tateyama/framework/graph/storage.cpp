#include <tateyama/framework/graph/storage.h>
#include <glog/logging.h>
#include <cstring>
#include <iostream>
#include <sstream>

namespace tateyama::framework::graph {

using namespace sharksfin;

bool storage::init(DatabaseHandle db_handle, TransactionHandle tx_handle) {
    if (!db_handle) return false;

    StorageOptions options;
    StatusCode rc = storage_create(tx_handle, Slice(STORAGE_NAME_NODES), options, &nodes_handle_);
    if (rc != StatusCode::OK && rc != StatusCode::ALREADY_EXISTS) {
        return false;
    }
    if (rc == StatusCode::ALREADY_EXISTS) {
        storage_get(tx_handle, Slice(STORAGE_NAME_NODES), &nodes_handle_);
    }

    rc = storage_create(tx_handle, Slice(STORAGE_NAME_EDGES), options, &edges_handle_);
    if (rc != StatusCode::OK && rc != StatusCode::ALREADY_EXISTS) {
        return false;
    }
    if (rc == StatusCode::ALREADY_EXISTS) {
        storage_get(tx_handle, Slice(STORAGE_NAME_EDGES), &edges_handle_);
    }

    return true;
}

static uint64_t generate_id() {
    static uint64_t counter = 1;
    return counter++;
}

bool storage::create_node(TransactionHandle tx, std::string_view properties, uint64_t& out_id) {
    if (!tx || !nodes_handle_) return false;
    out_id = generate_id();
    uint64_t key_val = out_id;
    Slice key(&key_val, sizeof(key_val));
    Slice value(properties.data(), properties.size());
    return content_put(tx, nodes_handle_, key, value, PutOperation::CREATE) == StatusCode::OK;
}

bool storage::get_node(TransactionHandle tx, uint64_t node_id, std::string& out_properties) {
    if (!tx || !nodes_handle_) return false;
    uint64_t key_val = node_id;
    Slice key(&key_val, sizeof(key_val));
    Slice value;
    if (content_get(tx, nodes_handle_, key, &value) != StatusCode::OK) return false;
    out_properties.assign(value.data<char>(), value.size());
    return true;
}

bool storage::create_edge(TransactionHandle tx, uint64_t from_id, uint64_t to_id, std::string_view label, std::string_view properties, uint64_t& out_id) {
    if (!tx || !edges_handle_) return false;
    out_id = generate_id();
    uint64_t key_val = out_id;
    Slice key(&key_val, sizeof(key_val));

    // Simple serialization: [from_id][to_id][label_size][label][properties]
    std::string val_buf;
    val_buf.append(reinterpret_cast<char*>(&from_id), sizeof(from_id));
    val_buf.append(reinterpret_cast<char*>(&to_id), sizeof(to_id));
    uint32_t label_size = static_cast<uint32_t>(label.size());
    val_buf.append(reinterpret_cast<char*>(&label_size), sizeof(label_size));
    val_buf.append(label);
    val_buf.append(properties);

    Slice value(val_buf.data(), val_buf.size());
    return content_put(tx, edges_handle_, key, value, PutOperation::CREATE) == StatusCode::OK;
}

bool storage::get_edge(TransactionHandle tx, uint64_t edge_id, edge_data& out_edge) {
    if (!tx || !edges_handle_) return false;
    uint64_t key_val = edge_id;
    Slice key(&key_val, sizeof(key_val));
    Slice value;
    if (content_get(tx, edges_handle_, key, &value) != StatusCode::OK) return false;

    const char* ptr = value.data<char>();
    size_t offset = 0;
    std::memcpy(&out_edge.from_id, ptr + offset, sizeof(uint64_t)); offset += sizeof(uint64_t);
    std::memcpy(&out_edge.to_id, ptr + offset, sizeof(uint64_t)); offset += sizeof(uint64_t);
    uint32_t label_size;
    std::memcpy(&label_size, ptr + offset, sizeof(uint32_t)); offset += sizeof(uint32_t);
    out_edge.label.assign(ptr + offset, label_size); offset += label_size;
    out_edge.properties.assign(ptr + offset, value.size() - offset);
    
    return true;
}

} // namespace tateyama::framework::graph
