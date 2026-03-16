# ADR-0005: Parallel Query Execution

- **Status**: Proposed
- **Date**: 2026-03-16
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0001, ADR-0002, ADR-0004

## Context

When a property index is unavailable (inequality operators `>`, `<`, `<>`), the WHERE clause must perform a full scan over all label-matched nodes. This is inherently parallelizable since each node's filter check is independent.

Current execution is single-threaded. On a machine with N cores, we leave (N-1) cores idle during scan-heavy queries.

## Decision

Parallelize the WHERE clause full-scan path using `std::thread` when the candidate set exceeds a configurable threshold.

### Design

```
PARALLEL_THRESHOLD = 10,000 nodes
```

When `ids.size() >= PARALLEL_THRESHOLD`:

1. Determine thread count: `min(hardware_concurrency(), ids.size() / 1000)`, minimum 2
2. Partition node IDs into equal chunks
3. Each thread independently:
   - Reads node properties via `store_.get_node()`
   - Evaluates the WHERE condition
   - Appends matching IDs to a thread-local result vector
4. Main thread joins all workers, merges results

### Thread Safety Considerations

**Sharksfin TransactionHandle**: In Shirakami, read operations on a single transaction handle are safe for concurrent access from multiple threads (snapshot reads). The mock implementation uses `static std::string` for return values, which is NOT thread-safe.

**Mock fix**: Change `static` local variables to `thread_local` in:
- `content_get()`: `static std::string last_val` → `thread_local std::string last_val`
- `iterator_get_key()`: `static std::string k` → `thread_local std::string k`

### Executor Changes

```cpp
// executor.h
static constexpr size_t PARALLEL_THRESHOLD = 10000;

// executor.cpp - execute_where() parallel path
if (ids.size() >= PARALLEL_THRESHOLD && op == "=" || op == ">" || ...) {
    size_t num_threads = std::min(
        static_cast<size_t>(std::thread::hardware_concurrency()),
        ids.size() / 1000);
    num_threads = std::max(num_threads, static_cast<size_t>(2));

    std::vector<std::vector<uint64_t>> per_thread_results(num_threads);
    std::vector<std::thread> threads;

    size_t chunk = (ids.size() + num_threads - 1) / num_threads;
    for (size_t t = 0; t < num_threads; ++t) {
        size_t begin = t * chunk;
        size_t end = std::min(begin + chunk, ids.size());
        threads.emplace_back([&, begin, end, t]() {
            auto& local = per_thread_results[t];
            for (size_t i = begin; i < end; ++i) {
                std::string props;
                if (!store_.get_node(tx_, ids[i], props)) continue;
                std::string val = get_json_value(props, prop_key);
                if (val.empty()) continue;
                // ... comparison logic (same as single-threaded)
                if (match_result) local.push_back(ids[i]);
            }
        });
    }

    for (auto& th : threads) th.join();

    filtered.clear();
    for (auto& r : per_thread_results)
        filtered.insert(filtered.end(), r.begin(), r.end());
}
```

### Why Not a Thread Pool?

- `std::thread` creation/destruction is acceptable for scan-heavy queries that take >100ms
- Avoids external dependency (no Boost, no BS::thread_pool)
- Thread pool can be added later if short-query overhead becomes an issue

## Consequences

### Positive

- Near-linear speedup with core count for full-scan WHERE queries
- On 8-core machine: expected 4-6x improvement for large scans
- No change to query semantics or results

### Negative

- Thread creation overhead (~10μs per thread) for small datasets
- Result ordering may differ from single-threaded execution (within each chunk, order is preserved)
- More complex error handling (exceptions in worker threads)
- Increased peak memory usage (thread stacks + per-thread result vectors)

### Risks

- If Shirakami TransactionHandle is NOT safe for concurrent reads, this optimization would cause data corruption. This must be verified against Shirakami documentation before production deployment.
- The `PARALLEL_THRESHOLD` value needs tuning per hardware

### Mitigation

- The parallel path is only activated for large candidate sets (>10K nodes)
- A compile-time or runtime flag can disable parallelism if needed
- Mock thread-safety fix is verified by running existing tests with `PARALLEL_THRESHOLD = 1`
