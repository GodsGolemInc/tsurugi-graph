#include <tateyama/framework/graph/storage.h>
#include <glog/logging.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace tateyama::framework::graph {

using namespace sharksfin;

// Helper to convert uint64 to big-endian bytes for lexicographical sorting
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

    return init_storage(STORAGE_NAME_NODES, nodes_handle_) &&
           init_storage(STORAGE_NAME_EDGES, edges_handle_) &&
           init_storage(STORAGE_NAME_OUT_INDEX, out_index_handle_) &&
           init_storage(STORAGE_NAME_IN_INDEX, in_index_handle_);
}

static uint64_t generate_id() {
    static uint64_t counter = 1;
    return counter++;
}

bool storage::create_node(TransactionHandle tx, std::string_view properties, uint64_t& out_id) {
    if (!tx || !nodes_handle_) return false;
    out_id = generate_id();
    return content_put(tx, nodes_handle_, to_key(out_id), properties, PutOperation::CREATE) == StatusCode::OK;
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
    out_id = generate_id();
    
    // 1. Store edge data
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

    // 2. Update Out-Index: [from_id][edge_id] -> [to_id]
    std::string out_key = to_key(from_id) + to_key(out_id);
    if (content_put(tx, out_index_handle_, out_key, to_key(to_id), PutOperation::CREATE) != StatusCode::OK) return false;

    // 3. Update In-Index: [to_id][edge_id] -> [from_id]
    std::string in_key = to_key(to_id) + to_key(out_id);
    if (content_put(tx, in_index_handle_, in_key, to_key(from_id), PutOperation::CREATE) != StatusCode::OK) return false;

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
        if (k.substr(0, 8) != prefix) break; // End of range
        uint64_t edge_id = from_key(k.substr(8, 8));
        out_edge_ids.push_back(edge_id);
    }
    iterator_dispose(it);
    return true;
}

} // namespace tateyama::framework::graph
