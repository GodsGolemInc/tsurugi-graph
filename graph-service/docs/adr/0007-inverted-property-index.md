# ADR-0007: Inverted Property Index

- **Status**: Proposed
- **Date**: 2026-03-17
- **Deciders**: tsurugi-graph development team
- **Supersedes**: ADR-0002 key format (API unchanged)
- **Related**: ADR-0002 (property index concept)

## Context

ADR-0002 introduced a property index using prefix scan with composite keys:
- Key: `label + '\0' + prop_key + '\0' + prop_value + node_id(8byte BE)`
- Value: empty
- Lookup: `prefix_scan()` + iterator traversal

This design degrades severely on real Shirakami (Masstree B+tree) at scale:

| Scale | Property Index Lookup (ops/s) | Degradation |
|:---|---:|---:|
| 100K nodes | 4,669 | baseline |
| 1M nodes | 550 | 8.5x slower |
| 5M nodes | 108 | **43x slower** |

The root cause is Masstree's prefix scan cost: O(log N) tree traversal plus sequential
iteration through matching entries. Each property value for each node is a separate B+tree
entry, creating millions of entries that must be scanned.

## Decision

Replace the prefix-scan index with an **inverted index** using exact-match keys and packed
node ID values.

### New Key-Value Format

| Component | Old (ADR-0002) | New (ADR-0007) |
|:--|:--|:--|
| **Key** | `label\0key\0value + nodeID(8B)` | `label\0key\0value` |
| **Value** | empty | packed nodeID list (8B × N) |
| **Lookup** | `prefix_scan` + iterator | `content_get` (single call) |
| **Complexity** | O(log N + K) per query | **O(1)** per query |

### Operations

**Index (create_node):**
1. `content_get` existing packed IDs for `(label, prop_key, prop_value)`
2. Append new `node_id` (8 bytes big-endian)
3. `content_put` updated packed list

**Lookup (find_nodes_by_property):**
1. `content_get` with key `label\0prop_key\0prop_value`
2. Deserialize packed IDs into `vector<uint64_t>`

**Remove (update_node_with_label / delete_node):**
1. `content_get` existing packed IDs
2. Remove target `node_id` from packed list
3. `content_put` updated list

### Packed ID Format

```
[node_id_1: 8 bytes BE] [node_id_2: 8 bytes BE] ... [node_id_N: 8 bytes BE]
```

No length prefix needed — total size / 8 = number of IDs.

## Consequences

### Positive

- **Lookup O(1)**: Single `content_get` replaces `prefix_scan` + iterator loop
- **Expected 10-50x improvement** at 100K+ scale for `find_nodes_by_property`
- **No API changes**: `find_nodes_by_property()` signature remains identical
- **Simple serialization**: Fixed 8-byte big-endian encoding, no complex format

### Negative

- **Write amplification**: Each node create requires read-modify-write per property
  (one extra `content_get` per property). For unique values this is 8 bytes → minimal.
- **Large values for common properties**: If 1M nodes share the same property value,
  the packed list is 8MB. This is acceptable for graph workloads where indexed properties
  (names, IDs) typically have high cardinality.
- **Not backward compatible**: Old prefix-scan format data is not readable. Requires
  fresh database or re-index. Acceptable for current prototype stage.

### Risks

- Transaction conflict probability increases for common property values (multiple creates
  writing to the same key). Mitigated by high cardinality of typical indexed properties.
