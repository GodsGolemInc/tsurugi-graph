# ADR-0011: Query Template Cache

- **Status**: Partially Implemented
- **Date**: 2026-03-17
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0003 (query cache)

## Context

ADR-0003 introduced an LRU query cache keyed by exact query strings. Queries that differ
only in literal values (e.g., `WHERE n.name = 'Alice'` vs `WHERE n.name = 'Bob'`) produce
cache misses, requiring a full re-parse (~4μs per query).

For workloads that issue many structurally identical queries with different parameters
(the common case for graph traversals and lookups), the cache hit rate approaches zero.

## Decision

Normalize query strings by replacing literal values with positional placeholders before
cache lookup. On cache hit, deep-copy the cached AST and bind the extracted literal values.

### Normalization

```
"MATCH (n:Person) WHERE n.name = 'Alice' RETURN n"
→ "MATCH (n:Person) WHERE n.name = $0 RETURN n"   literals=["Alice"]

"CREATE (n:Worker {name: 'W1', age: 25})"
→ "CREATE (n:Worker {name: $0, age: $1})"          literals=["W1", "25"]
```

### API

```cpp
// Extract literals and produce normalized query string
std::string normalize_query(const std::string& query, std::vector<std::string>& out_literals);

// Rebind literal values into a cached AST (in-order traversal)
void bind_literals(statement& stmt, const std::vector<std::string>& literals);
```

### Usage Pattern

```cpp
std::vector<std::string> literals;
std::string normalized = normalize_query(raw_query, literals);
auto cached = cache.get(normalized);
if (cached) {
    auto stmt = deep_copy_statement(*cached);
    bind_literals(*stmt, literals);
    // execute stmt
} else {
    auto stmt = parse(raw_query);
    cache.put(normalized, stmt);
    // execute stmt
}
```

## Consequences

### Positive

- **Higher cache hit rate**: All structurally identical queries share one cache entry
- **~3μs saved per cache hit**: Copy + bind (~1μs) vs full parse (~4μs)
- **No parser changes**: Normalization operates on raw query string, not AST
- **Backward compatible**: Cache key changes from raw query to normalized query

### Negative

- **Copy overhead**: Each cache hit requires AST deep copy for thread safety (~1μs)
- **Normalization cost**: String scanning for literal extraction adds ~0.5μs per query
- **Net saving per hit**: ~2.5μs (4μs parse - 1.5μs normalize+copy)

### Risks

- Negative numbers adjacent to identifiers (e.g., `n-1`) could be incorrectly normalized.
  Mitigated by checking that the digit is not preceded by an alphanumeric character.

## Implementation Status

**Partially implemented.** The `normalize_query`, `bind_literals`, and `deep_copy_statement`
functions are implemented in `query_cache.h`. However, the deep copy + bind approach has an
ordering issue: `normalize_query` extracts literals in string-occurrence order (left to right),
while `bind_literals` traverses CREATE properties in `std::map` key-alphabetical order.
This mismatch causes incorrect literal binding for CREATE clauses with multiple properties.

**Current behavior**: Service layer uses exact-match cache (ADR-0003). Template normalization
utilities are available for future use when the ordering issue is resolved (e.g., by changing
`pattern_node::properties` from `std::map` to `std::vector<std::pair<...>>` to preserve
insertion order).
