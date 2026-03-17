#include <tateyama/framework/graph/storage.h>
#include <glog/logging.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <unordered_map>

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

// Zero-allocation key: writes into caller-provided buffer, returns Slice
static inline Slice to_key_slice(char* buf, uint64_t val) {
    write_key(buf, val);
    return Slice(buf, 8);
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

// Helper: prefix scan using content_scan with PREFIXED_INCLUSIVE endpoint kind
static inline StatusCode prefix_scan(TransactionHandle tx, StorageHandle storage, Slice prefix, IteratorHandle* it) {
    return content_scan(tx, storage,
        prefix, EndPointKind::PREFIXED_INCLUSIVE,
        prefix, EndPointKind::PREFIXED_INCLUSIVE,
        it);
}

bool storage::init(DatabaseHandle db_handle, TransactionHandle tx_handle) {
    if (!db_handle) return false;
    db_handle_ = db_handle;
    StorageOptions options;

    auto init_storage = [&](std::string_view name, StorageHandle& handle) -> bool {
        // Use DatabaseHandle overload (TransactionHandle overload may not be implemented)
        StatusCode rc = storage_create(db_handle, Slice(name), options, &handle);
        if (rc == StatusCode::ALREADY_EXISTS) {
            rc = storage_get(db_handle, Slice(name), &handle);
        }
        if (rc != StatusCode::OK) {
            LOG(ERROR) << "Failed to init storage '" << name << "': rc=" << static_cast<int>(rc);
            return false;
        }
        return true;
    };

    if (!init_storage(STORAGE_NAME_NODES, nodes_handle_) ||
        !init_storage(STORAGE_NAME_EDGES, edges_handle_) ||
        !init_storage(STORAGE_NAME_OUT_INDEX, out_index_handle_) ||
        !init_storage(STORAGE_NAME_IN_INDEX, in_index_handle_) ||
        !init_storage(STORAGE_NAME_LABEL_INDEX, label_index_handle_) ||
        !init_storage(STORAGE_NAME_PROPERTY_INDEX, property_index_handle_)) {
        return false;
    }

    // Create or recover sequence
    auto rc = sequence_create(db_handle, &sequence_id_);
    if (rc != StatusCode::OK) {
        LOG(WARNING) << "sequence_create returned rc=" << static_cast<int>(rc) << " (may be existing)";
    }

    // Try to get current sequence state
    SequenceVersion ver = 0;
    SequenceValue val = 0;
    if (sequence_get(db_handle, sequence_id_, &ver, &val) == StatusCode::OK) {
        sequence_version_ = ver;
        next_id_ = static_cast<uint64_t>(val) + 1;
    } else {
        sequence_version_ = 0;
        next_id_ = 1;
    }

    return true;
}

uint64_t storage::allocate_id(TransactionHandle tx) {
    uint64_t id = next_id_++;
    if (sequence_batch_remaining_ == 0) {
        // Reserve a batch of IDs via sequence_put
        sequence_version_++;
        auto rc = sequence_put(tx, sequence_id_, sequence_version_,
                               static_cast<SequenceValue>(next_id_ + SEQUENCE_BATCH_SIZE - 2));
        if (rc != StatusCode::OK) {
            LOG(WARNING) << "Failed to batch-update sequence at id " << id;
        }
        sequence_batch_remaining_ = SEQUENCE_BATCH_SIZE - 1;
    } else {
        sequence_batch_remaining_--;
    }
    return id;
}

bool storage::create_node(TransactionHandle tx, std::string_view label, std::string_view properties, uint64_t& out_id) {
    if (!tx || !nodes_handle_) return false;

    out_id = allocate_id(tx);

    char key_buf[8];
    Slice node_key = to_key_slice(key_buf, out_id);
    if (content_put(tx, nodes_handle_, node_key, properties, PutOperation::CREATE) != StatusCode::OK) return false;

    if (!label.empty()) {
        thread_local std::string label_key;
        label_key.clear();
        label_key.reserve(label.size() + 8);
        label_key.append(label);
        label_key.append(key_buf, 8);
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
    char key_buf[8];
    Slice value;
    if (content_get(tx, nodes_handle_, to_key_slice(key_buf, node_id), &value) != StatusCode::OK) return false;
    out_properties.assign(value.data<char>(), value.size());
    return true;
}

bool storage::update_node(TransactionHandle tx, uint64_t node_id, std::string_view properties) {
    if (!tx || !nodes_handle_) return false;
    char key_buf[8];
    return content_put(tx, nodes_handle_, to_key_slice(key_buf, node_id), properties, PutOperation::CREATE_OR_UPDATE) == StatusCode::OK;
}

// Forward declarations for helpers defined later in this file
static std::vector<std::pair<std::string, std::string>> parse_json_properties(std::string_view json);
static Slice build_prop_index_key(std::string_view label, std::string_view prop_key, std::string_view prop_value);
static std::string append_packed_id(std::string_view existing, uint64_t node_id);
static std::string remove_packed_id(std::string_view existing, uint64_t node_id);

bool storage::update_node_with_label(TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view new_properties) {
    if (!tx || !nodes_handle_) return false;

    // Delta property index update: only modify entries for changed properties
    if (!label.empty()) {
        std::string old_props_str;
        if (get_node(tx, node_id, old_props_str) && !old_props_str.empty() && old_props_str != "{}") {
            auto old_props = parse_json_properties(old_props_str);
            auto new_props = parse_json_properties(new_properties);

            // Build maps for O(1) lookup
            std::unordered_map<std::string, std::string> old_map(old_props.begin(), old_props.end());
            std::unordered_map<std::string, std::string> new_map(new_props.begin(), new_props.end());

            // Remove index entries for properties that changed or were deleted
            for (const auto& [key, old_val] : old_map) {
                auto it = new_map.find(key);
                if (it == new_map.end() || it->second != old_val) {
                    // Property removed or value changed — remove old index entry
                    Slice idx_key = build_prop_index_key(label, key, old_val);
                    Slice existing;
                    std::string existing_str;
                    if (content_get(tx, property_index_handle_, idx_key, &existing) == StatusCode::OK) {
                        existing_str.assign(existing.data<char>(), existing.size());
                        std::string updated = remove_packed_id(existing_str, node_id);
                        idx_key = build_prop_index_key(label, key, old_val);
                        content_put(tx, property_index_handle_, idx_key, Slice(updated), PutOperation::CREATE_OR_UPDATE);
                    }
                }
            }

            // Add index entries for properties that changed or were added
            for (const auto& [key, new_val] : new_map) {
                auto it = old_map.find(key);
                if (it == old_map.end() || it->second != new_val) {
                    // Property added or value changed — add new index entry
                    char id_buf[8];
                    write_key(id_buf, node_id);
                    Slice idx_key = build_prop_index_key(label, key, new_val);
                    auto rc = content_put(tx, property_index_handle_, idx_key, Slice(id_buf, 8), PutOperation::CREATE);
                    if (rc != StatusCode::OK) {
                        idx_key = build_prop_index_key(label, key, new_val);
                        Slice existing;
                        std::string existing_str;
                        if (content_get(tx, property_index_handle_, idx_key, &existing) == StatusCode::OK) {
                            existing_str.assign(existing.data<char>(), existing.size());
                        }
                        std::string appended = append_packed_id(existing_str, node_id);
                        idx_key = build_prop_index_key(label, key, new_val);
                        content_put(tx, property_index_handle_, idx_key, Slice(appended), PutOperation::CREATE_OR_UPDATE);
                    }
                }
            }
        } else if (!new_properties.empty() && new_properties != "{}") {
            // No old properties — just index new ones
            index_node_properties(tx, node_id, label, new_properties);
        }
    }

    // Update node data
    char key_buf[8];
    if (content_put(tx, nodes_handle_, to_key_slice(key_buf, node_id), new_properties, PutOperation::CREATE_OR_UPDATE) != StatusCode::OK) {
        return false;
    }

    return true;
}

bool storage::delete_node(TransactionHandle tx, uint64_t node_id, std::string_view label) {
    if (!tx || !nodes_handle_) return false;

    char key_buf[8];
    Slice node_key = to_key_slice(key_buf, node_id);

    if (!label.empty()) {
        thread_local std::string label_key;
        label_key.clear();
        label_key.reserve(label.size() + 8);
        label_key.append(label);
        label_key.append(key_buf, 8);
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

    out_id = allocate_id(tx);

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

    char edge_buf[8], from_buf[8], to_buf[8];
    Slice edge_key = to_key_slice(edge_buf, out_id);
    if (content_put(tx, edges_handle_, edge_key, val_buf, PutOperation::CREATE) != StatusCode::OK) return false;

    write_key(from_buf, from_id);
    write_key(to_buf, to_id);

    char out_key_buf[16];
    std::memcpy(out_key_buf, from_buf, 8);
    std::memcpy(out_key_buf + 8, edge_buf, 8);
    if (content_put(tx, out_index_handle_, Slice(out_key_buf, 16), Slice(to_buf, 8), PutOperation::CREATE) != StatusCode::OK) {
        LOG(WARNING) << "Failed to create out_index for edge " << out_id;
    }

    char in_key_buf[16];
    std::memcpy(in_key_buf, to_buf, 8);
    std::memcpy(in_key_buf + 8, edge_buf, 8);
    if (content_put(tx, in_index_handle_, Slice(in_key_buf, 16), Slice(from_buf, 8), PutOperation::CREATE) != StatusCode::OK) {
        LOG(WARNING) << "Failed to create in_index for edge " << out_id;
    }

    return true;
}

bool storage::get_edge(TransactionHandle tx, uint64_t edge_id, edge_data& out_edge) {
    if (!tx || !edges_handle_) return false;
    char key_buf[8];
    Slice value;
    if (content_get(tx, edges_handle_, to_key_slice(key_buf, edge_id), &value) != StatusCode::OK) return false;

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
    char prefix_buf[8];
    Slice prefix = to_key_slice(prefix_buf, node_id);
    IteratorHandle it;
    if (prefix_scan(tx, out_index_handle_, prefix, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < 16) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, prefix_buf, 8) != 0) break;
        out_edge_ids.push_back(from_key_ptr(kp + 8));
    }
    return true;
}

bool storage::get_incoming_edges(TransactionHandle tx, uint64_t node_id, std::vector<uint64_t>& out_edge_ids) {
    if (!tx || !in_index_handle_) return false;
    char prefix_buf[8];
    Slice prefix = to_key_slice(prefix_buf, node_id);
    IteratorHandle it;
    if (prefix_scan(tx, in_index_handle_, prefix, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < 16) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, prefix_buf, 8) != 0) break;
        out_edge_ids.push_back(from_key_ptr(kp + 8));
    }
    return true;
}

bool storage::find_nodes_by_label(TransactionHandle tx, std::string_view label, std::vector<uint64_t>& out_node_ids) {
    if (!tx || !label_index_handle_) return false;
    if (label.empty()) return false;

    Slice prefix(label);
    size_t prefix_len = label.size();
    IteratorHandle it;
    if (prefix_scan(tx, label_index_handle_, prefix, &it) != StatusCode::OK) return false;
    iterator_guard guard(it);

    while (iterator_next(it) == StatusCode::OK) {
        Slice key;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (key.size() < prefix_len + 8) continue;
        const char* kp = key.data<char>();
        if (std::memcmp(kp, label.data(), prefix_len) != 0) break;
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

// ADR-0007: Inverted property index key: label + '\0' + prop_key + '\0' + prop_value
// Value is packed node_id list (8 bytes each, big-endian)
// Returns Slice pointing to thread_local buffer — valid until next call
static Slice build_prop_index_key(std::string_view label, std::string_view prop_key, std::string_view prop_value) {
    thread_local std::string key;
    key.clear();
    key.reserve(label.size() + 1 + prop_key.size() + 1 + prop_value.size());
    key.append(label);
    key.push_back('\0');
    key.append(prop_key);
    key.push_back('\0');
    key.append(prop_value);
    return Slice(key);
}

// Append a node_id to a packed ID list
static std::string append_packed_id(std::string_view existing, uint64_t node_id) {
    std::string result;
    result.reserve(existing.size() + 8);
    result.append(existing);
    char buf[8];
    write_key(buf, node_id);
    result.append(buf, 8);
    return result;
}

// Remove a node_id from a packed ID list
static std::string remove_packed_id(std::string_view existing, uint64_t node_id) {
    std::string result;
    result.reserve(existing.size());
    char target[8];
    write_key(target, node_id);
    for (size_t i = 0; i + 8 <= existing.size(); i += 8) {
        if (std::memcmp(existing.data() + i, target, 8) != 0) {
            result.append(existing.data() + i, 8);
        }
    }
    return result;
}

// Deserialize packed ID list into vector
static void unpack_ids(std::string_view packed, std::vector<uint64_t>& out) {
    out.reserve(out.size() + packed.size() / 8);
    for (size_t i = 0; i + 8 <= packed.size(); i += 8) {
        out.push_back(from_key_ptr(packed.data() + i));
    }
}

bool storage::index_node_properties(TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view properties) {
    if (!tx || !property_index_handle_ || label.empty()) return false;

    auto props = parse_json_properties(properties);
    for (const auto& [key, value] : props) {
        // Pack single node_id as value
        char id_buf[8];
        write_key(id_buf, node_id);

        Slice idx_key = build_prop_index_key(label, key, value);

        // Optimistic path: try CREATE first (succeeds for unique property values)
        auto rc = content_put(tx, property_index_handle_, idx_key, Slice(id_buf, 8), PutOperation::CREATE);
        if (rc == StatusCode::OK) {
            continue;  // Unique value — no content_get needed
        }

        // Key already exists — fall back to read-modify-write
        // Regenerate key (thread_local may have been overwritten)
        idx_key = build_prop_index_key(label, key, value);
        Slice existing;
        std::string existing_str;
        if (content_get(tx, property_index_handle_, idx_key, &existing) == StatusCode::OK) {
            existing_str.assign(existing.data<char>(), existing.size());
        }

        std::string new_value = append_packed_id(existing_str, node_id);
        idx_key = build_prop_index_key(label, key, value);
        if (content_put(tx, property_index_handle_, idx_key, Slice(new_value), PutOperation::CREATE_OR_UPDATE) != StatusCode::OK) {
            LOG(WARNING) << "Failed to index property " << key << " for node " << node_id;
        }
    }
    return true;
}

bool storage::find_nodes_by_property(TransactionHandle tx, std::string_view label, std::string_view prop_key, std::string_view prop_value, std::vector<uint64_t>& out_node_ids) {
    if (!tx || !property_index_handle_) return false;

    Slice idx_key = build_prop_index_key(label, prop_key, prop_value);
    Slice packed;
    if (content_get(tx, property_index_handle_, idx_key, &packed) != StatusCode::OK) {
        return true;  // No matches — return empty vector (not an error)
    }

    unpack_ids(std::string_view(packed.data<char>(), packed.size()), out_node_ids);
    return true;
}

bool storage::find_nodes_by_property_range(TransactionHandle tx, std::string_view label, std::string_view prop_key, const std::string& op, const std::string& compare_value, std::vector<uint64_t>& out_node_ids) {
    if (!tx || !property_index_handle_ || label.empty()) return false;

    // Build prefix: label\0prop_key\0
    thread_local std::string prefix_str;
    prefix_str.clear();
    prefix_str.append(label);
    prefix_str.push_back('\0');
    prefix_str.append(prop_key);
    prefix_str.push_back('\0');
    Slice prefix(prefix_str);

    // Scan all entries with this prefix
    IteratorHandle it = nullptr;
    if (prefix_scan(tx, property_index_handle_, prefix, &it) != StatusCode::OK) {
        return true;  // No index entries — empty result
    }
    iterator_guard ig(it);

    bool is_numeric = false;
    double compare_num = 0;
    try { compare_num = std::stod(compare_value); is_numeric = true; } catch (...) {}

    size_t prefix_len = prefix_str.size();

    while (iterator_next(it) == StatusCode::OK) {
        Slice key, value;
        if (iterator_get_key(it, &key) != StatusCode::OK) break;
        if (iterator_get_value(it, &value) != StatusCode::OK) break;

        // Check prefix match (iterator may go beyond prefix in mock/real)
        if (key.size() < prefix_len ||
            std::memcmp(key.data<char>(), prefix_str.data(), prefix_len) != 0) break;

        // Extract prop_value from key (after prefix)
        std::string_view prop_value(key.data<char>() + prefix_len, key.size() - prefix_len);

        bool match = false;
        if (is_numeric) {
            double val_num;
            try { val_num = std::stod(std::string(prop_value)); } catch (...) { continue; }
            if (op == ">") match = val_num > compare_num;
            else if (op == "<") match = val_num < compare_num;
            else if (op == ">=") match = val_num >= compare_num;
            else if (op == "<=") match = val_num <= compare_num;
            else if (op == "<>") match = val_num != compare_num;
        } else {
            std::string pv(prop_value);
            if (op == ">") match = pv > compare_value;
            else if (op == "<") match = pv < compare_value;
            else if (op == ">=") match = pv >= compare_value;
            else if (op == "<=") match = pv <= compare_value;
            else if (op == "<>") match = pv != compare_value;
        }

        if (match) {
            unpack_ids(std::string_view(value.data<char>(), value.size()), out_node_ids);
        }
    }

    return true;
}

bool storage::remove_property_index(TransactionHandle tx, uint64_t node_id, std::string_view label, std::string_view properties) {
    if (!tx || !property_index_handle_ || label.empty()) return false;

    auto props = parse_json_properties(properties);
    for (const auto& [key, value] : props) {
        Slice idx_key = build_prop_index_key(label, key, value);

        Slice existing;
        std::string existing_str;
        if (content_get(tx, property_index_handle_, idx_key, &existing) == StatusCode::OK) {
            existing_str.assign(existing.data<char>(), existing.size());
        } else {
            continue;  // Nothing to remove
        }

        std::string new_value = remove_packed_id(existing_str, node_id);
        idx_key = build_prop_index_key(label, key, value);  // regenerate after content_get
        content_put(tx, property_index_handle_, idx_key, Slice(new_value), PutOperation::CREATE_OR_UPDATE);
    }
    return true;
}

bool storage::get_nodes_batch(TransactionHandle tx, const std::vector<uint64_t>& node_ids, std::vector<std::pair<uint64_t, std::string>>& out_results) {
    if (!tx || !nodes_handle_) return false;
    out_results.clear();
    out_results.reserve(node_ids.size());
    char key_buf[8];
    for (uint64_t id : node_ids) {
        Slice key = to_key_slice(key_buf, id);
        Slice value;
        if (content_get(tx, nodes_handle_, key, &value) == StatusCode::OK) {
            out_results.emplace_back(id, std::string(value.data<char>(), value.size()));
        }
    }
    return true;
}

} // namespace tateyama::framework::graph
