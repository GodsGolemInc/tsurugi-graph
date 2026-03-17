# ADR-0010: Range Property Index

- **Status**: Accepted
- **Date**: 2026-03-17
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0007 (inverted property index)

## Context

ADR-0007 introduced an inverted property index providing O(1) lookup for equality queries.
However, inequality operators (>, <, >=, <=) still required a full scan: fetching all nodes
via `get_nodes_batch()`, parsing JSON properties per node, and comparing values. This was
O(N) per query — at 100K nodes, `WHERE n.age > 30` took 2.79 seconds (4 ops/s).

## Decision

Leverage the existing inverted index structure for range queries. The index keys
`label\0prop_key\0prop_value` are stored in sorted order in Masstree. A prefix scan
on `label\0prop_key\0` enumerates all unique property values with their packed node ID
lists, enabling value-level filtering without touching node data.

### New API

```cpp
bool find_nodes_by_property_range(
    TransactionHandle tx, std::string_view label,
    std::string_view prop_key, const std::string& op,
    const std::string& compare_value,
    std::vector<uint64_t>& out_node_ids);
```

### Algorithm

1. Build prefix key: `label\0prop_key\0`
2. `prefix_scan()` to get iterator over all values for this (label, key) pair
3. For each index entry, extract `prop_value` from key suffix
4. Compare `prop_value` against `compare_value` using the operator (numeric if parseable)
5. For matching entries, `unpack_ids()` to add node IDs to result

### Executor Integration

In `execute_where()`, when `op` is `>`, `<`, `>=`, or `<=`, attempt
`find_nodes_by_property_range()` before falling through to full scan.
The `<>` operator still uses full scan (range index provides no benefit for inequality).

## Consequences

### Positive

- **4.3x improvement at 100K**: `WHERE > ` from 2.79s to 0.60s (4 → 17 ops/s)
- **5.1x improvement at 1M**: from 29.92s to 5.87s
- **No additional storage**: Reuses existing inverted index — no new KVS tables
- **No write overhead**: Index structure unchanged; only the read path gains a new API

### Negative

- **O(V) scan**: Still scans all unique values for the property (V = distinct values).
  For age with 60 distinct values, V=60 — negligible. For high-cardinality properties
  (names), V approaches N — but such properties are rarely used with inequality operators.
- **Numeric parsing overhead**: Each property value is parsed with `stod()` during scan.
  Mitigated by V << N.

### Risks

- String-encoded numbers may not sort correctly in lexicographic order (e.g., "9" > "10").
  The range scan compares numeric values after parsing, so correctness is maintained.
  A future optimization could use fixed-width numeric encoding for natural sort order.
