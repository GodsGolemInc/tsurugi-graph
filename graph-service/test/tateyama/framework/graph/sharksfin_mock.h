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
    WAITING_FOR_OTHER_TRANSACTION = 4,
    ERR_ABORTED_RETRYABLE = -8,
    ERR_CONFLICT_ON_WRITE_PRESERVE = -12,
    ERR_UNKNOWN = 99
};

enum class PutOperation {
    CREATE = 0,
    CREATE_OR_UPDATE = 1,
    UPDATE = 2
};

enum class EndPointKind : std::uint32_t {
    UNBOUND = 0,
    INCLUSIVE = 1,
    EXCLUSIVE = 2,
    PREFIXED_INCLUSIVE = 3,
    PREFIXED_EXCLUSIVE = 4
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
using TransactionControlHandle = void*;
using TransactionHandle = void*;
using StorageHandle = std::string*; // Mock storage handle as its name
using IteratorHandle = void*;
using SequenceId = std::size_t;
using SequenceValue = std::int64_t;
using SequenceVersion = std::size_t;

struct StorageOptions {};
struct TransactionOptions {
    void add_write_preserve(Slice) {} // no-op in mock
};

// Mock Global State
inline std::map<std::string, std::map<std::string, std::string>> mock_db_state;
inline std::map<SequenceId, SequenceValue> mock_sequence_values;
inline std::map<SequenceId, SequenceVersion> mock_sequence_versions;
inline SequenceId mock_next_sequence_id = 0;

// Legacy sequence state for backward compat with tests that use these
inline std::map<std::string, uint64_t> mock_sequences;

inline StatusCode sequence_create(DatabaseHandle, SequenceId* result) {
    *result = mock_next_sequence_id++;
    mock_sequence_values[*result] = 0;
    mock_sequence_versions[*result] = 0;
    return StatusCode::OK;
}

inline StatusCode sequence_put(TransactionHandle, SequenceId id, SequenceVersion version, SequenceValue value) {
    mock_sequence_values[id] = value;
    mock_sequence_versions[id] = version;
    return StatusCode::OK;
}

inline StatusCode sequence_get(DatabaseHandle, SequenceId id, SequenceVersion* version, SequenceValue* value) {
    auto it = mock_sequence_values.find(id);
    if (it == mock_sequence_values.end()) return StatusCode::NOT_FOUND;
    *value = it->second;
    *version = mock_sequence_versions[id];
    return StatusCode::OK;
}

inline std::map<void*, std::map<std::string, std::string>::iterator> mock_iterators;
inline std::map<void*, std::map<std::string, std::string>::iterator> mock_iterators_end;
inline std::map<void*, bool> mock_iterators_started;

inline int mock_commit_failure_count = 0;

inline StatusCode transaction_begin(DatabaseHandle, TransactionOptions const&, TransactionControlHandle* result) {
    *result = (void*)0xbeef;
    return StatusCode::OK;
}

inline StatusCode transaction_borrow_handle(TransactionControlHandle, TransactionHandle* result) {
    *result = (void*)0xdead;
    return StatusCode::OK;
}

inline StatusCode transaction_commit(TransactionControlHandle, bool /*async*/ = false) {
    if (mock_commit_failure_count > 0) {
        mock_commit_failure_count--;
        return StatusCode::ERR_ABORTED_RETRYABLE;
    }
    return StatusCode::OK;
}

inline StatusCode transaction_abort(TransactionControlHandle, bool /*rollback*/ = false) { return StatusCode::OK; }
inline StatusCode transaction_dispose(TransactionControlHandle) { return StatusCode::OK; }

inline StatusCode content_scan(TransactionHandle, StorageHandle storage,
    Slice begin_key, EndPointKind begin_kind,
    Slice end_key, EndPointKind end_kind,
    IteratorHandle* result) {
    auto& s = mock_db_state[*storage];
    std::string bk = begin_key.to_string();

    // Position begin iterator
    auto it = s.lower_bound(bk);
    if (begin_kind == EndPointKind::EXCLUSIVE && it != s.end() && it->first == bk) {
        ++it;
    }

    // Compute end boundary
    auto end_it = s.end();
    if (end_kind == EndPointKind::EXCLUSIVE) {
        std::string ek = end_key.to_string();
        end_it = s.lower_bound(ek);
    } else if (end_kind == EndPointKind::INCLUSIVE) {
        std::string ek = end_key.to_string();
        end_it = s.upper_bound(ek);
    }
    // PREFIXED_INCLUSIVE/PREFIXED_EXCLUSIVE/UNBOUND: end_it stays at s.end()

    static int iter_id = 0;
    void* h = (void*)(uintptr_t)(++iter_id);
    mock_iterators[h] = it;
    mock_iterators_end[h] = end_it;
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
    thread_local std::string k;
    k = it->first;
    *result = Slice(k.data(), k.size());
    return StatusCode::OK;
}

inline StatusCode iterator_get_value(IteratorHandle h, Slice* result) {
    auto it = mock_iterators[h];
    if (it == mock_iterators_end[h]) return StatusCode::NOT_FOUND;
    thread_local std::string v;
    v = it->second;
    *result = Slice(v.data(), v.size());
    return StatusCode::OK;
}

inline StatusCode iterator_dispose(IteratorHandle h) {
    mock_iterators.erase(h);
    mock_iterators_end.erase(h);
    return StatusCode::OK;
}

inline StatusCode storage_create(DatabaseHandle, Slice key, StorageOptions const&, StorageHandle* result) {
    std::string name = key.to_string();
    if (mock_db_state.find(name) != mock_db_state.end()) {
        *result = new std::string(name);
        return StatusCode::ALREADY_EXISTS;
    }
    mock_db_state[name] = {};
    *result = new std::string(name);
    return StatusCode::OK;
}

inline StatusCode storage_get(DatabaseHandle, Slice key, StorageHandle* result) {
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
    thread_local std::string last_val;
    last_val = it->second;
    *result = Slice(last_val.data(), last_val.size());
    return StatusCode::OK;
}

} // namespace sharksfin
