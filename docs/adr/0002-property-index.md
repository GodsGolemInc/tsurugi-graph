# ADR-0002: Secondary Property Index

- **Status**: Proposed
- **Date**: 2026-03-16
- **Deciders**: tsurugi-graph development team
- **Supersedes**: N/A
- **Related**: ADR-0001 (Graph Engine Architecture)

## Context

The current MATCH+WHERE execution performs a full label-index scan followed by per-node property reads and string comparison. At 10M nodes with 50% label selectivity, each WHERE query scans 5M nodes and takes ~7.6 seconds. This is the primary performance bottleneck (see BENCHMARK.md).

### Current Flow

```
MATCH (n:Person) WHERE n.name = 'Alice' RETURN n
```

1. `find_nodes_by_label("Person")` → 5M node IDs (O(N))
2. For each ID: `get_node(id)` → JSON properties (5M KV lookups)
3. For each: `get_json_value(props, "name")` → string comparison
4. Filter to matching IDs

**Total**: O(N) storage reads + O(N) JSON parses per query.

## Decision

Add a secondary property index storage that indexes `(label, property_key, property_value)` → `node_id`.

### Storage Design

**New storage**: `graph_prop_index`

**Key format**: `label + '\0' + property_key + '\0' + property_value + node_id(8-byte BE)`

The NUL byte delimiter is chosen because:
- Labels and property keys are identifiers that cannot contain NUL
- Enables unambiguous prefix scanning at any level:
  - `label\0` → all indexed properties for a label
  - `label\0key\0` → all values for a specific property
  - `label\0key\0value` → exact match (primary use case)

**Value**: empty (existence-only index)

### API

```cpp
// Index all properties of a node (called from create_node)
bool index_node_properties(TransactionHandle tx, uint64_t node_id,
                           std::string_view label, std::string_view properties);

// Find nodes matching exact property value
bool find_nodes_by_property(TransactionHandle tx, std::string_view label,
                            std::string_view prop_key, std::string_view prop_value,
                            std::vector<uint64_t>& out_node_ids);

// Remove property index entries (called from delete_node/update_node)
bool remove_property_index(TransactionHandle tx, uint64_t node_id,
                           std::string_view label, std::string_view properties);
```

### Executor Integration

The executor tracks label metadata per variable in `context_labels_`:
```cpp
std::map<std::string, std::string> context_labels_;  // variable → label
```

`execute_where()` checks for optimizable conditions:
1. If `op == "="` AND the variable has a known label AND the comparison is a literal value:
   - Use `find_nodes_by_property()` for O(log N) lookup
   - Intersect with existing context (if any)
2. Otherwise: fall back to full scan (unchanged)

### Index Maintenance

- `create_node()`: After storing node, call `index_node_properties()` for all JSON key-value pairs
- `update_node()`: Read old properties, call `remove_property_index()`, store new properties, call `index_node_properties()`
- `delete_node()`: Call `remove_property_index()` before deleting node data

### JSON Property Parsing

A lightweight parser extracts all top-level key-value pairs from a JSON object:
```cpp
static std::vector<std::pair<std::string, std::string>> parse_json_properties(std::string_view json);
```

This reuses the existing `get_json_value()` pattern (direct byte scanning, no external JSON library).

## Consequences

### Positive

- **MATCH+WHERE equality queries**: O(N) → O(log N + K) where K is result count
- At 10M nodes: expected improvement from ~7.6s/query to <10ms/query
- No change required to Cypher syntax or client protocol
- Backward compatible: queries still work if index is empty (falls back to scan)

### Negative

- **Write overhead**: Each `create_node` now performs additional index writes (one per property key-value pair). For a node with 2 properties, 2 extra KV writes.
- **Storage cost**: Additional storage proportional to total property count
- **Update complexity**: `update_node` must read old properties to clean up stale index entries
- **Range queries** (>, <) are NOT indexed and still require full scan

### Risks

- NUL delimiter assumes property values do not contain NUL bytes
- Very long property values inflate index key size (consider truncation for values >256 bytes in future)

## Expected Performance Impact

| Operation | Before | After (estimated) |
|---|---|---|
| MATCH+WHERE `=` (10M, per query) | 7,646ms | <10ms |
| create_node throughput | 308K ops/s | ~250K ops/s (10-20% overhead) |
| Storage usage (10M nodes, 2 props each) | ~800MB | ~1.1GB (+40%) |
