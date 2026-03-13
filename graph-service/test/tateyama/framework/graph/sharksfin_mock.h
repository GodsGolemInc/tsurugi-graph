#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstring>

namespace sharksfin {

enum class StatusCode {
    OK = 0,
    NOT_FOUND = 1,
    ALREADY_EXISTS = 2,
    ERR_UNKNOWN = 99
};

enum class PutOperation {
    CREATE = 0,
    CREATE_OR_UPDATE = 1,
    UPDATE = 2
};

struct Slice {
    const void* data_ptr;
    size_t size_val;
    Slice() : data_ptr(nullptr), size_val(0) {}
    Slice(const void* data, size_t size) : data_ptr(data), size_val(size) {}
    Slice(const std::string& s) : data_ptr(s.data()), size_val(s.size()) {}
    Slice(const std::string_view& s) : data_ptr(s.data()), size_val(s.size()) {}
    template<typename T> T* data() const { return static_cast<T*>(const_cast<void*>(data_ptr)); }
    size_t size() const { return size_val; }
    std::string to_string() const { return std::string(static_cast<const char*>(data_ptr), size_val); }
};

using DatabaseHandle = void*;
using TransactionHandle = void*;
using StorageHandle = std::string*; // Mock storage handle as its name

struct StorageOptions {};

// Mock Global State
inline std::map<std::string, std::map<std::string, std::string>> mock_db_state;

inline StatusCode storage_create(TransactionHandle, Slice key, StorageOptions const&, StorageHandle* result) {
    std::string name = key.to_string();
    if (mock_db_state.find(name) != mock_db_state.end()) {
        *result = new std::string(name);
        return StatusCode::ALREADY_EXISTS;
    }
    mock_db_state[name] = {};
    *result = new std::string(name);
    return StatusCode::OK;
}

inline StatusCode storage_get(TransactionHandle, Slice key, StorageHandle* result) {
    std::string name = key.to_string();
    if (mock_db_state.find(name) == mock_db_state.end()) return StatusCode::NOT_FOUND;
    *result = new std::string(name);
    return StatusCode::OK;
}

inline StatusCode content_put(TransactionHandle, StorageHandle storage, Slice key, Slice value, PutOperation) {
    mock_db_state[*storage][key.to_string()] = value.to_string();
    return StatusCode::OK;
}

inline StatusCode content_get(TransactionHandle, StorageHandle storage, Slice key, Slice* result) {
    auto& s = mock_db_state[*storage];
    auto it = s.find(key.to_string());
    if (it == s.end()) return StatusCode::NOT_FOUND;
    static std::string last_val;
    last_val = it->second;
    *result = Slice(last_val.data(), last_val.size());
    return StatusCode::OK;
}

} // namespace sharksfin
