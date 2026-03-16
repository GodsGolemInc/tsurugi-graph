# ADR-0003: Compiled Query Cache

- **Status**: Proposed
- **Date**: 2026-03-16
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0001 (Graph Engine Architecture)

## Context

Every Cypher query is tokenized and parsed from scratch, even if the exact same query string has been executed before. The parser achieves ~250K ops/sec, meaning each parse takes ~4μs. For high-throughput OLTP workloads where the same query patterns repeat frequently, this overhead is avoidable.

### Current Flow

```
CypherRequest → lexer.tokenize() → parser.parse() → executor.execute()
```

Each request creates new token vectors and AST objects on the heap via `std::shared_ptr`.

## Decision

Add an LRU cache of parsed `statement` ASTs in the service layer, keyed by the exact query string.

### Design

**New header**: `include/tateyama/framework/graph/core/query_cache.h`

```cpp
class query_cache {
public:
    explicit query_cache(size_t max_size = 1024);

    std::shared_ptr<statement> get(const std::string& query) const;
    void put(const std::string& query, std::shared_ptr<statement> stmt);
    void clear();
    size_t size() const;

private:
    size_t max_size_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<statement>> cache_;
    mutable std::list<std::string> lru_order_;
    mutable std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;
};
```

### Thread Safety

The cache is protected by `std::mutex` since the service may be called from multiple request-handling threads. The lock is held only for map lookup/insert (O(1) average), not during query execution.

### Service Integration

```cpp
// In service::operator()
auto cached = query_cache_.get(cypher.query());
std::shared_ptr<statement> stmt_ptr;
if (cached) {
    stmt_ptr = cached;
} else {
    core::lexer lexer(cypher.query());
    core::parser parser(lexer.tokenize());
    stmt_ptr = std::make_shared<statement>(parser.parse());
    query_cache_.put(cypher.query(), stmt_ptr);
}
// Execute with *stmt_ptr
```

### AST Immutability

The cached AST is shared across threads/requests. The current AST nodes are read-only after construction (no executor modifies the AST), so sharing is safe without deep copies.

### LRU Eviction

When the cache reaches `max_size`, the least recently used entry is evicted. This bounds memory usage while keeping hot queries cached.

## Consequences

### Positive

- Eliminates ~4μs parse overhead for repeated queries
- Reduces heap allocations (no new token/AST objects per cached query)
- Transparent to the rest of the system (executor sees the same `statement`)

### Negative

- Parameterized queries with different literal values (e.g., `WHERE n.name = 'Alice'` vs `WHERE n.name = 'Bob'`) do NOT cache-hit since the string differs
- Adds mutex contention (but lock duration is O(1))
- Memory usage for cached ASTs (~1KB per statement * 1024 = ~1MB max)

### Future Work

- Query template normalization: Replace literal values with placeholders to improve cache hit rates
- Prepared statement protocol: Client sends parameterized query + bind values separately
