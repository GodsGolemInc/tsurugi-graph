# ADR-0009: Optimistic Index Write and Delta SET

- **Status**: Accepted
- **Date**: 2026-03-17
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0007 (inverted property index)

## Context

ADR-0007 introduced an inverted property index that provides O(1) lookup via packed nodeID
lists. However, the write path requires read-modify-write for every indexed property:

```
content_get → append nodeID → content_put   (3 KVS calls per property)
```

This caused `create_node` to regress from 44,207 to 5,220 ops/s (-88%) at 100K nodes on
real Shirakami, because every property insertion required a `content_get` even when the
property value had never been indexed before.

Additionally, `update_node_with_label` (used by SET) removed all property index entries
and re-added all, even when only one property changed. For `SET n.salary = 50100` on a
node with 2 properties, this performed 4 index operations instead of the minimum 2.

## Decision

### 1. Optimistic CREATE for Index Writes

For `index_node_properties`, try `content_put(CREATE)` first:

```cpp
auto rc = content_put(tx, property_index_handle_, idx_key,
                      Slice(id_buf, 8), PutOperation::CREATE);
if (rc == StatusCode::OK) {
    continue;  // Unique value — single KVS call, no content_get
}
// Key exists — fall back to read-modify-write
```

For high-cardinality properties (names, IDs, emails), `CREATE` succeeds nearly 100% of
the time, reducing the write path from 3 KVS calls to 1.

### 2. Delta SET for Property Index Updates

For `update_node_with_label`, compare old and new properties and only update entries for
properties that actually changed:

```cpp
auto old_map = parse_json_properties(old_props);
auto new_map = parse_json_properties(new_properties);

// Remove index entries only for changed/deleted properties
for (auto& [key, old_val] : old_map) {
    auto it = new_map.find(key);
    if (it == new_map.end() || it->second != old_val) {
        remove_index_entry(label, key, old_val, node_id);
    }
}

// Add index entries only for changed/added properties
for (auto& [key, new_val] : new_map) {
    auto it = old_map.find(key);
    if (it == old_map.end() || it->second != new_val) {
        add_index_entry(label, key, new_val, node_id);  // uses optimistic CREATE
    }
}
```

The add path also uses optimistic CREATE for newly unique values.

## Consequences

### Positive

- **create_node recovered**: 5,220 → 38,025 ops/s at 100K (+7.3x), nearly matching v2 (44,207)
- **No read regression**: Property index lookup remains 243K ops/s (52x vs v2)
- **Pipeline improved**: 9,749 → 12,943 ops/s (+33%) from delta SET reducing unnecessary index operations
- **Zero cost for unique values**: Optimistic CREATE adds no overhead when property values are unique — the common case for names, IDs, and other indexed fields

### Negative

- **Failed CREATE overhead**: When `content_put(CREATE)` fails (duplicate value), the total cost is higher than direct read-modify-write (failed CREATE + content_get + content_put = 3 calls vs 2). This only occurs for low-cardinality properties.
- **Delta SET memory**: Requires parsing both old and new JSON properties and building hash maps for comparison. Adds ~O(P) memory where P = number of properties.

### Risks

- If a workload has predominantly low-cardinality indexed properties (e.g., boolean flags, status enums shared across many nodes), the optimistic CREATE path will fail frequently and perform worse than direct read-modify-write. For such workloads, a hint or adaptive strategy could be considered.
