# ADR-0012: Streaming Executor and Memory Efficiency

## Status

Accepted

## Date

2026-03-17

## Context

At 10M nodes on 16GB RAM, the graph-service aborts with OOM. Three bottlenecks were identified:

1. **`execute_return` materializes all properties at once** — A `MATCH (n:Person) RETURN n` on 5M nodes calls `get_nodes_batch` for all 5M IDs, creating a `var_props_cache` of ~2.5GB. Combined with the batch results vector, peak memory reaches ~5GB per query.

2. **`execute_where` full-scan loads all properties** — The same pattern: `get_nodes_batch` on all candidate IDs before filtering, consuming ~2.5GB.

3. **`append_packed_id` has O(N^2) copy behavior** — The property index stores packed nodeID lists. Each append copies the entire existing list into a new string, resulting in quadratic total copies during bulk insert.

4. **`find_nodes_by_label` materializes all IDs** — Returns a `vector<uint64_t>` containing all matching node IDs (40MB for 5M nodes).

## Decision

### Batched `execute_return` and `execute_where`

Process results in chunks of `BATCH_SIZE = 1024` nodes instead of all at once. Each chunk:
1. Fetches properties only for that chunk's node IDs
2. Processes (filters or serializes) the chunk
3. Releases the chunk's memory before loading the next

This reduces peak memory from O(N * avg_props_size) to O(BATCH_SIZE * avg_props_size), a reduction from ~2.5GB to ~50KB.

The parallel WHERE path (ADR-0005) is preserved within each chunk when the chunk exceeds `PARALLEL_THRESHOLD`.

### In-place `append_packed_id`

Replace the copy-and-append pattern:
```cpp
// Before: O(N) copy per append
std::string new_value = append_packed_id(existing_str, node_id);

// After: O(1) append
existing_str.append(id_buf, 8);
```

This eliminates the O(N^2) behavior during bulk insert, recovering write throughput at scale.

### `label_iterator` streaming scan

Add a pull-based iterator that wraps Shirakami's `content_scan` / `iterator_next` API:
```cpp
class label_iterator {
    bool next();                   // advance to next matching node
    uint64_t node_id() const;     // current node ID
    bool get_properties(...);     // lazy property read
};
```

This avoids materializing all label-matched IDs into a single vector. The executor's `execute_match` uses this iterator to collect IDs incrementally.

## Consequences

### Positive

- **10M+ nodes on 16GB** — Peak query memory drops from ~5GB to ~100MB
- **Bulk insert recovery** — O(N^2) packed ID append eliminated
- **No API changes** — Cypher query interface unchanged
- **Backward compatible** — All 110 existing tests pass without modification

### Negative

- **Slightly more complex executor** — Batched loop adds nesting
- **JSON output constructed incrementally** — Cannot pre-allocate exact output size (uses conservative estimate)
- **Iterator lifetime** — `label_iterator` holds an open Shirakami iterator handle; must be consumed promptly to avoid blocking GC

### Performance

| Metric | Before | After |
|:---|---:|---:|
| MATCH+RETURN peak memory (5M nodes) | ~5.2 GB | ~100 MB |
| 10M node bulk insert | OOM | Completes |
| Small query (<1K nodes) throughput | Baseline | No regression |
