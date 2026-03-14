#include <tateyama/framework/graph/storage.h>
#include <glog/logging.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace tateyama::framework::graph {

using namespace sharksfin;

static std::string to_key(uint64_t val) {
    uint64_t be = __builtin_bswap64(val);
    std::string s;
    s.append(reinterpret_cast<char*>(&be), sizeof(be));
    return s;
}

static uint64_t from_key(std::string_view s) {
    uint64_t be;
    std::memcpy(&be, s.data(), sizeof(be));
    return __builtin_bswap64(be);
}

bool storage::init(DatabaseHandle db_handle, TransactionHandle tx_handle) {
    if (!db_handle) return false;
    StorageOptions options;
    
    auto init_storage = [&](std::string_view name, StorageHandle& handle) {
        StatusCode rc = storage_create(tx_handle, Slice(name), options, &handle);
        if (rc == StatusCode::ALREADY_EXISTS) {
            rc = storage_get(tx_handle, Slice(name), &handle);
        }
        return rc == StatusCode::OK;
    };

    if (!init_storage(STORAGE_NAME_NODES, nodes_handle_) ||
        !init_storage(STORAGE_NAME_EDGES, edges_handle_) ||
        !init_storage(STORAGE_NAME_OUT_INDEX, out_index_handle_) ||
        !init_storage(STORAGE_NAME_IN_INDEX, in_index_handle_) ||
        !init_storage(STORAGE_NAME_LABEL_INDEX, label_index_handle_)) {
        return false;
    }

    // Initialize/Get Persistent Sequence
    if (sequence_create(db_handle, Slice(SEQUENCE_NAME), &sequence_handle_) == StatusCode::ALREADY_EXISTS) {
        sequence_get(db_handle, Slice(SEQUENCE_NAME), &sequence_handle_);
    }

    return true;
}

bool storage::create_node(TransactionHandle tx, std::string_view label, std::string_view properties, uint64_t& out_id) {
    if (!tx || !nodes_handle_) return false;
    
    // Get next ID from persistent sequence
    if (sequence_next(tx, sequence_handle_, &out_id) != StatusCode::OK) return false;

    // 1. Store node data
    if (content_put(tx, nodes_handle_, to_key(out_id), properties, PutOperation::CREATE) != StatusCode::OK) return false;

    // 2. Update Label Index: [label][node_id] -> (empty)
    if (!label.empty()) {
        std::string label_key = std::string(label) + to_key(out_id);
        content_put(tx, label_index_handle_, label_key, Slice(), PutOperation::CREATE);
    }

    return true;
}

bool storage::get_node(TransactionHandle tx, uint64_t node_id, std::string& out_properties) {
    if (!tx || !nodes_handle_) return false;
    Slice value;
    if (content_get(tx, nodes_handle_, to_key(node_id), &value) != StatusCode::OK) return false;
    out_properties.assign(value.data<char>(), value.size());
    return true;
}

bool storage::create_edge(TransactionHandle tx, uint64_t from_id, uint64_t to_id, std::string_view label, std::string_view properties, uint64_t& out_id) {
    if (!tx || !edges_handle_) return false;
    if (sequence_next(tx, sequence_handle_, &out_id) != StatusCode::OK) return false;
    
    std::string val_buf;
    uint64_t be_from = __builtin_bswap64(from_id);
    uint64_t be_to = __builtin_bswap64(to_id);
    val_buf.append(reinterpret_cast<char*>(&be_from), 8);
    val_buf.append(reinterpret_cast<char*>(&be_to), 8);
    uint32_t label_size = static_cast<uint32_t>(label.size());
    val_buf.append(reinterpret_cast<char*>(&label_size), 4);
    val_buf.append(label);
    val_buf.append(properties);
    if (content_put(tx, edges_handle_, to_key(out_id), val_buf, PutOperation::CREATE) != StatusCode::OK) return false;

    std::string out_key = to_key(from_id) + to_key(out_id);
    content_put(tx, out_index_handle_, out_key, to_key(to_id), PutOperation::CREATE);

    std::string in_key = to_key(to_id) + to_key(out_id);
    content_put(tx, in_index_handle_, in_key, to_key(from_id), PutOperation::CREATE);

    return true;
}

bool storage::get_edge(TransactionHandle tx, uint64_t edge_id, edge_data& out_edge) {
    if (!tx || !edges_handle_) return false;
    Slice value;
    if (content_get(tx, edges_handle_, to_key(edge_id), &value) != StatusCode::OK) return false;

    const char* ptr = value.data<char>();
    std::memcpy(&out_edge.from_id, ptr, 8); out_edge.from_id = __builtin_bswap64(out_edge.from_id);
    std::memcpy(&out_edge.to_id, ptr + 8, 8); out_edge.to_id = __builtin_bswap64(out_edge.to_id);
    uint32_t label_size;
    std::memcpy(&label_size, ptr + 16, 4);
    out_edge.label.assign(ptr + 20, label_size);
    out_edge.properties.assign(ptr + 20 + label_size, value.size() - (20 + label_size));
    return true;
}

bool storage::get_outgoing_edges(TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids) {
    if (!tx || !out_index_handle_) return false;
    std::string prefix = to_key(node_id);
    IteratorHandle it;
    if (content_scan(tx, out_index_handle_, prefix, 0, prefix, 0, &it) != StatusCode::OK) return false;
    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        iterator_get_key(it, &key);
        if (key.size() < 16) continue;
        std::string k = key.to_string();
        if (k.substr(0, 8) != prefix) break;
        out_edge_ids.push_back(from_key(k.substr(8, 8)));
    }
    iterator_dispose(it);
    return true;
}

bool storage::find_nodes_by_label(TransactionHandle tx, std::string_view label, std::vector<uint64_t>& out_node_ids) {
    if (!tx || !label_index_handle_) return false;
    std::string prefix(label);
    IteratorHandle it;
    if (content_scan(tx, label_index_handle_, prefix, 0, prefix, 0, &it) != StatusCode::OK) return false;
    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        iterator_get_key(it, &key);
        std::string k = key.to_string();
        if (k.size() <= prefix.size()) continue;
        if (k.substr(0, prefix.size()) != prefix) break;
        out_node_ids.push_back(from_key(k.substr(prefix.size())));
    }
    iterator_dispose(it);
    return true;
}

} // namespace tateyama::framework::graph
