#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace sharksfin {

enum class StatusCode : std::int64_t {
    OK = 0,
    NOT_FOUND = 1,
    ALREADY_EXISTS = 2,
    ERR_SERIALIZATION_FAILURE = 10,
    ERR_CONFLICT_ON_WRITE_PRESERVE = 11,
    ERR_WAITING_FOR_OTHER_TRANSACTION = 12,
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
using IteratorHandle = void*;

struct StorageOptions {};
struct TransactionOptions {};

// Mock Global State
inline std::map<std::string, std::map<std::string, std::string>> mock_db_state;
inline std::map<void*, std::map<std::string, std::string>::iterator> mock_iterators;
inline std::map<void*, std::map<std::string, std::string>::iterator> mock_iterators_end;
inline std::map<void*, bool> mock_iterators_started;

inline int mock_commit_failure_count = 0;

inline StatusCode transaction_begin(DatabaseHandle, TransactionOptions const&, TransactionHandle* result) {
    *result = (void*)0xbeef;
    return StatusCode::OK;
}

inline StatusCode transaction_commit(TransactionHandle) {
    if (mock_commit_failure_count > 0) {
        mock_commit_failure_count--;
        return StatusCode::ERR_SERIALIZATION_FAILURE;
    }
    return StatusCode::OK;
}

inline StatusCode transaction_abort(TransactionHandle) { return StatusCode::OK; }
inline StatusCode transaction_dispose(TransactionHandle) { return StatusCode::OK; }

inline StatusCode content_scan(TransactionHandle, StorageHandle storage, Slice begin_key, int, Slice end_key, int, IteratorHandle* result) {
    auto& s = mock_db_state[*storage];
    auto it = s.lower_bound(begin_key.to_string());
    static int iter_id = 0;
    void* h = (void*)(uintptr_t)(++iter_id);
    mock_iterators[h] = it;
    mock_iterators_end[h] = s.end();
    mock_iterators_started[h] = false;
    *result = h;
    return StatusCode::OK;
}

inline StatusCode iterator_next(IteratorHandle h) {
    if (!mock_iterators_started[h]) {
        mock_iterators_started[h] = true;
        if (mock_iterators[h] == mock_iterators_end[h]) return StatusCode::NOT_FOUND;
        return StatusCode::OK;
    }
    if (mock_iterators[h] == mock_iterators_end[h]) return StatusCode::NOT_FOUND;
    ++mock_iterators[h];
    if (mock_iterators[h] == mock_iterators_end[h]) return StatusCode::NOT_FOUND;
    return StatusCode::OK;
}

inline StatusCode iterator_get_key(IteratorHandle h, Slice* result) {
    auto it = mock_iterators[h];
    if (it == mock_iterators_end[h]) return StatusCode::NOT_FOUND;
    static std::string k;
    k = it->first;
    *result = Slice(k.data(), k.size());
    return StatusCode::OK;
}

inline StatusCode iterator_dispose(IteratorHandle h) {
    mock_iterators.erase(h);
    mock_iterators_end.erase(h);
    return StatusCode::OK;
}

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
