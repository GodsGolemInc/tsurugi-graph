# ADR-0013: Sortable Numeric Key Encoding

- **Status**: Accepted
- **Date**: 2026-03-17
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0007 (inverted property index), ADR-0010 (range property index)

## Context

ADR-0010 introduced range property index queries using prefix scan. However, the property
index key format `label\0prop_key\0prop_value_string` stored numeric values as their string
representation (e.g., "42", "100"). This caused two problems:

1. **No numeric sort order**: String "9" > "10" lexicographically, breaking range scan logic
2. **Full scan required**: `find_nodes_by_property_range` had to iterate ALL property values
   and convert each via `std::stod()`, even for operators like `>` where only a subset matches

With 50K nodes and 60 distinct age values, `WHERE n.age > 50` scanned all 60 entries
and performed 60 string-to-double conversions per query.

## Decision

Encode numeric property values as 8-byte sortable binary (IEEE 754 with sign-bit
manipulation) so that byte-level comparison matches numeric order. Use type prefixes
to separate numeric and string values in the index.

### Key Format

```
label\0prop_key\0\x01<8-byte sortable double>   — numeric values
label\0prop_key\0\x02<string>                    — string values
```

### Sortable Double Encoding

```cpp
// IEEE 754 double → sortable binary
uint64_t bits;
memcpy(&bits, &val, 8);
if (bits & (1ULL << 63))
    bits = ~bits;           // negative: flip all bits
else
    bits ^= (1ULL << 63);  // positive/zero: flip MSB
// Store as big-endian
```

This ensures:
- `-2.0 < -1.0 < 0.0 < 1.0 < 2.0` in byte order
- Negative numbers sort correctly (larger absolute value = smaller bytes)

### Range Scan Optimization

For `>`, `>=`, `<`, `<=` with numeric values, use `content_scan()` with computed
begin_key/end_key and appropriate `EndPointKind` (INCLUSIVE/EXCLUSIVE):

| Operator | begin_key | begin_kind | end_key | end_kind |
|:---------|:----------|:-----------|:--------|:---------|
| `>` X    | prefix + encode(X) | EXCLUSIVE | prefix + `\x02` | EXCLUSIVE |
| `>=` X   | prefix + encode(X) | INCLUSIVE | prefix + `\x02` | EXCLUSIVE |
| `<` X    | prefix + `\x01`    | INCLUSIVE | prefix + encode(X) | EXCLUSIVE |
| `<=` X   | prefix + `\x01`    | INCLUSIVE | prefix + encode(X) | INCLUSIVE |

The iterator yields only matching entries — no per-entry comparison needed.

### Mock Update

Updated `sharksfin_mock.h` `content_scan()` to respect `begin_kind` (EXCLUSIVE)
and `end_key`/`end_kind` (INCLUSIVE/EXCLUSIVE) parameters, enabling correct range
scan testing without the real Shirakami backend.

## Consequences

### Positive

- **Correct numeric sorting**: Property index keys now sort in numeric order
- **Reduced scan entries**: `age > 50` with 60 distinct values scans ~29 entries instead of 60
- **No per-entry conversion**: Eliminated `std::stod()` calls during range iteration
- **Foundation for future optimization**: Sortable keys enable Shirakami-side range pruning

### Negative

- **Marginal Shirakami improvement**: Real Shirakami bottleneck is packed ID data transfer
  (192KB/query for 24K matching nodes), not iterator count. Range scan reduces iterator_next
  calls by ~50% but overall query time improves only ~2%
- **Slightly larger keys**: Numeric keys are now fixed 9 bytes (type prefix + 8-byte encoding)
  vs variable-length string representation

### Measurements (50K nodes, 100 queries, `WHERE n.age > X`)

| Backend | Before | After | Change |
|:--------|-------:|------:|-------:|
| Mock (std::map) | 91 ops/s | ~90 ops/s | ~0% (unpack_ids dominates) |
| Shirakami (Masstree) | 8 ops/s | ~8 ops/s | ~0% (B+tree I/O dominates) |

### Future Improvement Directions

1. **Shirakami native range scan**: Shirakami's `content_scan` with EXCLUSIVE/INCLUSIVE
   should internally skip B+tree subtrees outside the range — verify with Shirakami team
2. **Columnar property storage**: Store property values in column-oriented format
   (packed arrays per property key) for cache-friendly range scans
3. **Parallel range scan**: Split range into sub-ranges and scan in parallel threads
4. **Compressed packed IDs**: Use delta encoding or bitmap for packed node ID lists
   to reduce data transfer volume
