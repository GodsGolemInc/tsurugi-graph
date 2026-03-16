#include <tateyama/framework/graph/storage.h>
#include <glog/logging.h>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace tateyama::framework::graph {

using namespace sharksfin;

// Reusable key buffer to avoid per-call allocation
static inline void write_key(char* buf, uint64_t val) {
    uint64_t be = __builtin_bswap64(val);
    std::memcpy(buf, &be, 8);
}

static inline std::string to_key(uint64_t val) {
    char buf[8];
    write_key(buf, val);
    return std::string(buf, 8);
}

static inline uint64_t from_key_ptr(const char* p) {
    uint64_t be;
    std::memcpy(&be, p, sizeof(be));
    return __builtin_bswap64(be);
}

static inline uint64_t from_key(std::string_view s) {
    if (s.size() < sizeof(uint64_t)) return 0;
    return from_key_ptr(s.data());
}

// RAII guard for iterator handles to prevent resource leaks
class iterator_guard {
public:
    explicit iterator_guard(IteratorHandle h) : handle_(h) {}
    ~iterator_guard() { if (handle_) iterator_dispose(handle_); }
    iterator_guard(const iterator_guard&) = delete;
    iterator_guard& operator=(const iterator_guard&) = delete;
private:
    IteratorHandle handle_;
};

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
        !init_storage(STORAGE_NAME_LABEL_INDEX, label_index_handle_) ||
        !init_storage(STORAGE_NAME_PROPERTY_INDEX, property_index_handle_)) {
        return false;
    }

    auto rc = sequence_create(db_handle, Slice(SEQUENCE_NAME), &sequence_handle_);
    if (rc == StatusCode::ALREADY_EXISTS) {
        rc = sequence_get(db_handle, Slice(SEQUENCE_NAME), &sequence_handle_);
        if (rc != StatusCode::OK) {
            LOG(ERROR) << "Failed to get existing graph_id_sequence";
            return false;
        }
    } else if (rc != StatusCode::OK) {
        LOG(ERROR) << "Failed to create graph_id_sequence";
        return false;
    }

    return true;
}

bool storage::create_node(TransactionHandle tx, std::string_view label, std::string_view properties, uint64_t& out_id) {
    if (!tx || !nodes_handle_) return false;

    if (sequence_next(tx, sequence_handle_, &out_id) != StatusCode::OK) return false;

    std::string node_key = to_key(out_id);
    if (content_put(tx, nodes_handle_, node_key, properties, PutOperation::CREATE) != StatusCode::OK) return false;

    if (!label.empty()) {
        std::string label_key;
        label_key.reserve(label.size() + 8);
        label_key.append(label);
        label_key.append(node_key);
        if (content_put(tx, label_index_handle_, label_key, Slice(), PutOperation::CREATE) != StatusCode::OK) {
            LOG(WARNING) << "Failed to update label index for node " << out_id;
        }

        // Index properties for secondary property index (ADR-0002)
        if (!properties.empty() && properties != "{}") {
            index_node_properties(tx, out_id, label, properties);
        }
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

bool storage::update_node(TransactionHandle tx, uint64_t node_id, std::string_view properties) {
    if (!tx || !nodes_handle_) return false;
    return content_put(tx, nodes_handle_, to_key(node_id), properties, PutOperation::CREATE_OR_UPDATE) == StatusCode::OK;
}

bool storage::update_node_with_label(TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view new_properties) {
    if (!tx || !nodes_handle_) return false;

    // Remove old property index entries
    if (!label.empty()) {
        std::string old_props;
        if (get_node(tx, node_id, old_props) && !old_props.empty() && old_props != "{}") {
            remove_property_index(tx, node_id, label, old_props);
        }
    }

    // Update node data
    if (content_put(tx, nodes_handle_, to_key(node_id), new_properties, PutOperation::CREATE_OR_UPDATE) != StatusCode::OK) {
        return false;
    }

    // Add new property index entries
    if (!label.empty() && !new_properties.empty() && new_properties != "{}") {
        index_node_properties(tx, node_id, label, new_properties);
    }

    return true;
}

bool storage::delete_node(TransactionHandle tx, uint64_t node_id, std::string_view label) {
    if (!tx || !nodes_handle_) return false;

    std::string node_key = to_key(node_id);

    if (!label.empty()) {
        std::string label_key;
        label_key.reserve(label.size() + 8);
        label_key.append(label);
        label_key.append(node_key);
        auto rc = content_put(tx, label_index_handle_, label_key, Slice(), PutOperation::CREATE_OR_UPDATE);
        if (rc != StatusCode::OK) {
            LOG(WARNING) << "Failed to remove label index for node " << node_id;
        }
    }

    return content_put(tx, nodes_handle_, node_key, Slice(), PutOperation::CREATE_OR_UPDATE) == StatusCode::OK;
}

bool storage::delete_edge(TransactionHandle tx, uint64_t edge_id) {
    if (!tx || !edges_handle_) return false;

    edge_data ed;
    if (!get_edge(tx, edge_id, ed)) return false;

    std::string edge_key = to_key(edge_id);
    std::string from_key_str = to_key(ed.from_id);
    std::string to_key_str = to_key(ed.to_id);

    // Remove from out_index
    std::string out_key;
    out_key.reserve(16);
    out_key.append(from_key_str);
    out_key.append(edge_key);
    if (content_put(tx, out_index_handle_, out_key, Slice(), PutOperation::CREATE_OR_UPDATE) != StatusCode::OK) {
        LOG(WARNING) << "Failed to remove out_index for edge " << edge_id;
    }

    // Remove from in_index
    std::string in_key;
    in_key.reserve(16);
    in_key.append(to_key_str);
    in_key.append(edge_key);
    if (content_put(tx, in_index_handle_, in_key, Slice(), PutOperation::CREATE_OR_UPDATE) != StatusCode::OK) {
        LOG(WARNING) << "Failed to remove in_index for edge " << edge_id;
    }

    return content_put(tx, edges_handle_, edge_key, Slice(), PutOperation::CREATE_OR_UPDATE) == StatusCode::OK;
}

bool storage::create_edge(TransactionHandle tx, uint64_t from_id, uint64_t to_id, std::string_view label, std::string_view properties, uint64_t& out_id) {
    if (!tx || !edges_handle_) return false;
    if (sequence_next(tx, sequence_handle_, &out_id) != StatusCode::OK) return false;

    // Build edge value: from(8) + to(8) + label_size(4) + label + properties
    std::string val_buf;
    val_buf.reserve(20 + label.size() + properties.size());
    char be_buf[8];
    write_key(be_buf, from_id);
    val_buf.append(be_buf, 8);
    write_key(be_buf, to_id);
    val_buf.append(be_buf, 8);
    uint32_t label_size = static_cast<uint32_t>(label.size());
    val_buf.append(reinterpret_cast<char*>(&label_size), 4);
    val_buf.append(label);
    val_buf.append(properties);

    std::string edge_key = to_key(out_id);
    if (content_put(tx, edges_handle_, edge_key, val_buf, PutOperation::CREATE) != StatusCode::OK) return false;

    std::string from_key_str = to_key(from_id);
    std::string to_key_str = to_key(to_id);

    std::string out_key;
    out_key.reserve(16);
    out_key.append(from_key_str);
    out_key.append(edge_key);
    if (content_put(tx, out_index_handle_, out_key, to_key_str, PutOperation::CREATE) != StatusCode::OK) {
        LOG(WARNING) << "Failed to create out_index for edge " << out_id;
    }

    std::string in_key;
    in_key.reserve(16);
    in_key.append(to_key_str);
    in_key.append(edge_key);
    if (content_put(tx, in_index_handle_, in_key, from_key_str, PutOperation::CREATE) != StatusCode::OK) {
        LOG(WARNING) << "Failed to create in_index for edge " << out_id;
    }

    return true;
}

bool storage::get_edge(TransactionHandle tx, uint64_t edge_id, edge_data& out_edge) {
    if (!tx || !edges_handle_) return false;
    Slice value;
    if (content_get(tx, edges_handle_, to_key(edge_id), &value) != StatusCode::OK) return false;

    if (value.size() < 20) {
        LOG(WARNING) << "Edge data too small for edge " << edge_id;
        return false;
    }

    const char* ptr = value.data<char>();
    out_edge.from_id = from_key_ptr(ptr);
    out_edge.to_id = from_key_ptr(ptr + 8);
    uint32_t label_size;
    std::memcpy(&label_size, ptr + 16, 4);

    if (20 + label_size > value.size()) {
        LOG(WARNING) << "Edge label_size exceeds buffer for edge " << edge_id;
        return false;
    }

    out_edge.label.assign(ptr + 20, label_size);
    out_edge.properties.assign(ptr + 20 + label_size, value.size() - (20 + label_size));
    return true;
}

bool storage::get_outgoing_edges(TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids) {
    if (!tx || !out_index_handle_) return false;
    std::string prefix = to_key(node_id);
    IteratorHandle it;
    if (content_scan(tx, out_index_handle_, prefix, 0, prefix, 0, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < 16) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, prefix.data(), 8) != 0) break;
        out_edge_ids.push_back(from_key_ptr(kp + 8));
    }
    return true;
}

bool storage::get_incoming_edges(TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids) {
    if (!tx || !in_index_handle_) return false;
    std::string prefix = to_key(node_id);
    IteratorHandle it;
    if (content_scan(tx, in_index_handle_, prefix, 0, prefix, 0, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < 16) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, prefix.data(), 8) != 0) break;
        out_edge_ids.push_back(from_key_ptr(kp + 8));
    }
    return true;
}

bool storage::find_nodes_by_label(TransactionHandle tx, std::string_view label, std::vector<uint64_t>& out_node_ids) {
    if (!tx || !label_index_handle_) return false;
    if (label.empty()) return false;

    std::string prefix(label);
    size_t prefix_len = prefix.size();
    IteratorHandle it;
    if (content_scan(tx, label_index_handle_, prefix, 0, prefix, 0, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < prefix_len + 8) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, prefix.data(), prefix_len) != 0) break;
        out_node_ids.push_back(from_key_ptr(kp + prefix_len));
    }
    return true;
}

// --- JSON Property Parsing Helper ---

static std::vector<std::pair<std::string, std::string>> parse_json_properties(std::string_view json) {
    std::vector<std::pair<std::string, std::string>> result;
    if (json.size() < 2 || json.front() != '{') return result;

    size_t pos = 1;
    while (pos < json.size()) {
        // Find key
        pos = json.find('"', pos);
        if (pos == std::string_view::npos) break;
        pos++; // skip opening quote
        size_t key_end = json.find('"', pos);
        if (key_end == std::string_view::npos) break;
        std::string key(json.substr(pos, key_end - pos));
        pos = key_end + 1;

        // Skip to colon
        pos = json.find(':', pos);
        if (pos == std::string_view::npos) break;
        pos++;

        // Skip whitespace
        while (pos < json.size() && json[pos] == ' ') pos++;
        if (pos >= json.size()) break;

        // Read value
        std::string value;
        if (json[pos] == '"') {
            pos++; // skip opening quote
            size_t val_end = json.find('"', pos);
            if (val_end == std::string_view::npos) break;
            value = std::string(json.substr(pos, val_end - pos));
            pos = val_end + 1;
        } else {
            size_t val_end = pos;
            while (val_end < json.size() && json[val_end] != ',' && json[val_end] != '}' && json[val_end] != ' ') val_end++;
            value = std::string(json.substr(pos, val_end - pos));
            pos = val_end;
        }

        result.emplace_back(std::move(key), std::move(value));

        // Skip comma
        while (pos < json.size() && (json[pos] == ',' || json[pos] == ' ')) pos++;
        if (pos < json.size() && json[pos] == '}') break;
    }
    return result;
}

// Build property index key: label + '\0' + prop_key + '\0' + prop_value + node_id(8byte BE)
static std::string build_prop_index_key(std::string_view label, std::string_view prop_key, std::string_view prop_value, uint64_t node_id) {
    std::string key;
    key.reserve(label.size() + 1 + prop_key.size() + 1 + prop_value.size() + 8);
    key.append(label);
    key.push_back('\0');
    key.append(prop_key);
    key.push_back('\0');
    key.append(prop_value);
    char buf[8];
    write_key(buf, node_id);
    key.append(buf, 8);
    return key;
}

bool storage::index_node_properties(TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view properties) {
    if (!tx || !property_index_handle_ || label.empty()) return false;

    auto props = parse_json_properties(properties);
    for (const auto& [key, value] : props) {
        std::string idx_key = build_prop_index_key(label, key, value, node_id);
        if (content_put(tx, property_index_handle_, idx_key, Slice(), PutOperation::CREATE_OR_UPDATE) != StatusCode::OK) {
            LOG(WARNING) << "Failed to index property " << key << " for node " << node_id;
        }
    }
    return true;
}

bool storage::find_nodes_by_property(TransactionHandle tx, std::string_view label, std::string_view prop_key, std::string_view prop_value, std::vector<uint64_t>& out_node_ids) {
    if (!tx || !property_index_handle_) return false;

    // Build prefix: label + '\0' + prop_key + '\0' + prop_value
    std::string prefix;
    prefix.reserve(label.size() + 1 + prop_key.size() + 1 + prop_value.size());
    prefix.append(label);
    prefix.push_back('\0');
    prefix.append(prop_key);
    prefix.push_back('\0');
    prefix.append(prop_value);

    size_t prefix_len = prefix.size();
    IteratorHandle it;
    if (content_scan(tx, property_index_handle_, prefix, 0, prefix, 0, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < prefix_len + 8) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, prefix.data(), prefix_len) != 0) break;
        out_node_ids.push_back(from_key_ptr(kp + prefix_len));
    }
    return true;
}

bool storage::remove_property_index(TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view properties) {
    if (!tx || !property_index_handle_ || label.empty()) return false;

    auto props = parse_json_properties(properties);
    for (const auto& [key, value] : props) {
        std::string idx_key = build_prop_index_key(label, key, value, node_id);
        // Overwrite with empty value (tombstone in mock; real delete in production)
        content_put(tx, property_index_handle_, idx_key, Slice(), PutOperation::CREATE_OR_UPDATE);
    }
    return true;
}

bool storage::get_nodes_batch(TransactionHandle tx, const std::vector<uint64_t>& node_ids, std::vector<std::pair<uint64_t, std::string>>& out_results) {
    if (!tx || !nodes_handle_) return false;
    out_results.clear();
    out_results.reserve(node_ids.size());
    char key_buf[8];
    for (uint64_t id : node_ids) {
        write_key(key_buf, id);
        Slice value;
        if (content_get(tx, nodes_handle_, std::string(key_buf, 8), &value) == StatusCode::OK) {
            out_results.emplace_back(id, std::string(value.data<char>(), value.size()));
        }
    }
    return true;
}

} // namespace tateyama::framework::graph
