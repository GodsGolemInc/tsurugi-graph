# ADR-0004: Batch Property Reads

- **Status**: Proposed
- **Date**: 2026-03-16
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0001 (Graph Engine Architecture)

## Context

The executor reads node properties one at a time via `storage::get_node()` in multiple places:

1. **WHERE clause** (fallback full scan): Loops over all candidate nodes, calling `get_node()` + `get_json_value()` per node
2. **RETURN clause**: Reads properties for each row individually
3. **SET clause**: Reads current properties before updating each node

Each `get_node()` call involves:
- `to_key()` allocation (8-byte string)
- `content_get()` KV lookup
- `out_properties.assign()` copy

For N nodes, this means N separate function calls with N key allocations.

## Decision

Add a batch read API to the storage layer and use it in the executor.

### Storage API

```cpp
bool get_nodes_batch(TransactionHandle tx,
                     const std::vector<uint64_t>& node_ids,
                     std::vector<std::pair<uint64_t, std::string>>& out_results);
```

### Implementation

```cpp
bool storage::get_nodes_batch(TransactionHandle tx,
                              const std::vector<uint64_t>& node_ids,
                              std::vector<std::pair<uint64_t, std::string>>& out_results) {
    out_results.clear();
    out_results.reserve(node_ids.size());
    char key_buf[8];
    for (uint64_t id : node_ids) {
        write_key(key_buf, id);
        Slice value;
        if (content_get(tx, nodes_handle_, std::string_view(key_buf, 8), &value) == StatusCode::OK) {
            out_results.emplace_back(id, std::string(value.data<char>(), value.size()));
        }
    }
    return true;
}
```

Key optimizations:
- **Single key buffer**: Reuse `key_buf[8]` instead of allocating `std::string` per ID via `to_key()`
- **Pre-reserved output vector**: Single allocation for all results
- **In-order iteration**: Good for cache locality on sorted KV stores

### Executor Integration

**execute_where()** (full scan path):
```cpp
std::vector<std::pair<uint64_t, std::string>> batch;
store_.get_nodes_batch(tx_, it->second, batch);
for (auto& [id, props] : batch) {
    std::string val = get_json_value(props, prop_key);
    // ... filter logic
}
```

**execute_return()**: Pre-fetch all node properties needed for the result set in one batch call.

**execute_set()**: Batch-read all target nodes before applying updates.

## Consequences

### Positive

- Reduces per-node overhead (no key allocation per node)
- Enables future prefetching optimizations (range scan instead of point reads)
- Clearer intent in executor code (batch vs. loop)

### Negative

- All properties are read into memory at once (higher peak memory for large batches)
- For small result sets (<100 nodes), overhead savings are negligible
- Adds a parallel code path to maintain alongside single-read

### Expected Impact

- 2-3x improvement in full-scan WHERE filtering throughput
- Marginal improvement in RETURN for large result sets
