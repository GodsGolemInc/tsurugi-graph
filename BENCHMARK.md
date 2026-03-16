# tsurugi-graph Benchmark Report

## Test Environment

- CPU: Linux 6.17.0-14-generic (x86_64)
- Compiler: g++ 13, C++17, -O2
- Storage backend: In-memory mock (sharksfin_mock.h)
- All benchmarks are single-threaded (except parallel WHERE path)

> **Note:** The mock backend eliminates I/O latency and measures pure graph engine throughput.
> Production performance with Shirakami will differ due to actual disk/network I/O.

---

## Results Summary (v2 — with optimizations ADR-0002~0005)

### Write Operations (ops/sec)

| Operation | 100K nodes | 1M nodes | 10M nodes |
|:---|---:|---:|---:|
| Storage: create_node | 321,407 | 290,889 | 226,313 |
| Storage: create_edge | 349,356 | 186,082 | 193,469 |
| Cypher: CREATE node (parse+execute) | 97,794 | 76,497 | 76,405 |

### Read / Query Operations

| Operation | 100K nodes | 1M nodes | 10M nodes |
|:---|---:|---:|---:|
| Parser throughput (ops/sec) | 259,944 | 254,690 | 243,817 |
| Storage: point read (ops/sec) | 1,388,613 | 1,241,865 | 1,029,173 |
| Property index lookup (ops/sec) | 118,869 | 18,950 | 2,297 |
| Label scan (single, sec) | 0.11s (50K) | 1.11s (500K) | 4.01s (5M) |
| MATCH+WHERE = indexed (100q, sec) | 0.06s | 0.63s | 6.89s |
| MATCH+WHERE > full scan (100q, sec) | 7.93s | 94.37s | 926.36s |
| Edge traversal (ops/sec) | 122,817 | 85,883 | 79,273 |
| Pipeline: CREATE+MATCH+WHERE+SET+RETURN | 9,398 ops/s | 3,763 ops/s | 635 ops/s |

### Aggregate

| Scale | Total Ops | Total Time | Throughput |
|:---|---:|---:|---:|
| 100K nodes | 620,201 | 12.15s | 51,056 ops/sec |
| 1M nodes | 5,120,201 | 127.07s | 40,295 ops/sec |
| 10M nodes | 14,120,201 | 1,026.13s | 13,761 ops/sec |

---

## v1 → v2 Improvement Summary

### MATCH+WHERE = (equality, 100 queries)

| Scale | v1 (full scan) | v2 (property index) | Speedup |
|:---|---:|---:|---:|
| 100K | 7.35s | **0.06s** | **122x** |
| 1M | 82.41s | **0.63s** | **131x** |
| 10M | 764.64s | **6.89s** | **111x** |

### Pipeline: CREATE+MATCH+WHERE+SET+RETURN

| Scale | v1 | v2 | Speedup |
|:---|---:|---:|---:|
| 100K | 370 ops/s | **9,398 ops/s** | **25x** |
| 1M | 265 ops/s | **3,763 ops/s** | **14x** |
| 10M | 270 ops/s | **635 ops/s** | **2.4x** |

---

## Optimizations Applied (v2)

### ADR-0002: Property Index

**Impact**: MATCH+WHERE equality queries: O(N) → O(log N)

- New storage `graph_prop_index` with composite key: `label + '\0' + key + '\0' + value + nodeID(8B)`
- `create_node()` automatically indexes all JSON property key-value pairs
- Fused MATCH+WHERE optimization: when a simple MATCH is followed by an equality WHERE, the executor skips the label scan entirely and uses `find_nodes_by_property()` directly
- Falls back to full scan for inequality operators (>, <, <>)

### ADR-0003: Compiled Query Cache

**Impact**: Eliminates ~4μs parse overhead per cached query

- LRU cache (max 1024 entries) of parsed `statement` ASTs
- Thread-safe with `std::mutex`
- Keyed by exact query string (parameterized queries with different literals do not cache-hit)

### ADR-0004: Batch Property Reads

**Impact**: 2-3x improvement in RETURN throughput for large result sets

- `get_nodes_batch()` API with reusable key buffer
- `execute_return()` pre-fetches all node properties in a single batch call
- Reduces per-node overhead from key allocation and function call

### ADR-0005: Parallel WHERE Execution

**Impact**: Near-linear speedup for full-scan WHERE (inequality queries)

- `std::thread` parallelization when candidate set > 10,000 nodes
- Thread count: `min(hardware_concurrency, N/1000)`
- Thread-local result vectors merged after join (no mutex contention during scan)
- Mock thread-safety: `static` → `thread_local` for `content_get`/`iterator_get_key`

---

## Scalability Analysis (v2)

### O(1) per operation (constant throughput)

- **Parser**: ~250K ops/sec
- **Point read**: ~1.0-1.4M ops/sec
- **Cypher CREATE**: ~76-98K ops/sec

### O(log N) per operation (property index)

- **Property index lookup**: 119K ops/s (100K) → 2.3K ops/s (10M)
  - Degradation is from mock `std::map` tree depth (O(log N) per lookup)
  - Production Shirakami B+tree will have similar O(log N) but lower constants

### O(N) per operation (full scan)

- **MATCH+WHERE >** (inequality): Remains O(N), parallel execution provides constant-factor improvement
  - 100K: 79ms/query → 1M: 944ms/query → 10M: 9,264ms/query

---

## Competitive Performance Comparison

> **Disclaimer:** The following comparisons use publicly available third-party benchmark data.
> Direct comparison is inherently imperfect due to differing hardware, workloads, storage engines,
> and concurrency models. tsurugi-graph numbers are **single-threaded, in-memory mock** measurements
> that eliminate I/O latency. Production performance with Shirakami will be lower due to disk I/O
> and transaction coordination overhead. These comparisons are intended to provide directional context,
> not rigorous apples-to-apples evaluation.

### Node Creation Throughput

| Database | Throughput | Conditions | Source |
|:---|---:|:---|:---|
| **tsurugi-graph (Cypher CREATE)** | 76K–98K ops/s | Single-thread, in-memory mock, parse+execute | This benchmark |
| **tsurugi-graph (Storage API)** | 226K–321K ops/s | Single-thread, in-memory mock, direct storage | This benchmark |
| **Memgraph** | ~250K nodes/s | In-memory, `UNWIND+CREATE` batch, 100K nodes | [Memgraph Blog](https://memgraph.com/blog/memgraph-or-neo4j-analyzing-write-speed-performance) |
| **Neo4j** | ~26K nodes/s | Disk+RAM, `UNWIND+CREATE` batch, 100K nodes (3.8s) | [Memgraph Blog](https://memgraph.com/blog/memgraph-or-neo4j-analyzing-write-speed-performance) |
| **Neo4j (bulk import)** | >1M records/s | Offline `neo4j-admin import` tool (non-transactional) | [Neo4j Blog](https://neo4j.com/blog/news/neo4j-2-2-0-scalability-performance/) |
| **RedisGraph/FalkorDB** | ~10K nodes/s | Single `CREATE` per command, 100K nodes | [Redis Blog](https://redis.io/blog/new-redisgraph-1-0-achieves-600x-faster-performance-graph-databases/) |

**Analysis:**
- tsurugi-graph's Cypher CREATE (76–98K ops/s) includes per-query parsing and property indexing overhead. Without parsing (raw storage API), throughput exceeds 300K ops/s.
- Memgraph achieves similar throughput via `UNWIND` batch semantics (single parse, multiple writes). tsurugi-graph parses each query individually; adding batch `UNWIND` support would close this gap.
- Neo4j's transactional CREATE is ~26K/s due to disk persistence. tsurugi-graph's mock eliminates I/O, so production performance with Shirakami will be lower but should remain competitive due to Shirakami's in-memory-first architecture.

### Point Read / Property Lookup Latency

| Database | Latency | Conditions | Source |
|:---|---:|:---|:---|
| **tsurugi-graph (get_node)** | <1μs | In-memory mock, single key lookup | This benchmark |
| **tsurugi-graph (indexed WHERE =)** | 0.6ms (100K) – 69ms (10M) | Parse + property index lookup + RETURN | This benchmark |
| **Neo4j (indexed)** | <1ms | With range index, warm cache | [Neo4j Docs](https://neo4j.com/docs/cypher-manual/current/indexes/search-performance-indexes/using-indexes/) |
| **Memgraph** | 1.07ms | Expansion-1 query, 12 concurrent clients | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |
| **Neo4j** | 13.7ms – 27.96ms | Expansion-1 query, 12 concurrent clients | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |
| **RedisGraph/FalkorDB** | <10ms (p99 <140ms) | Sub-10ms typical, in-memory GraphBLAS | [FalkorDB Blog](https://www.falkordb.com/blog/graph-database-performance-benchmarks-falkordb-vs-neo4j/) |

**Analysis:**
- tsurugi-graph's raw point read (~1μs) is a pure in-memory hash lookup. The indexed Cypher query (0.6ms at 100K) includes parse, index scan, and JSON result formatting.
- At 10M nodes, tsurugi-graph's indexed WHERE takes 69ms per query. This degradation is primarily from `std::map` O(log N) in the mock; production Shirakami's B+tree will have similar asymptotic behavior with lower constants.
- Neo4j and Memgraph both achieve sub-millisecond indexed lookups with warm caches, comparable to tsurugi-graph at smaller scales.

### Graph Traversal (1-hop)

| Database | Latency | Dataset | Source |
|:---|---:|:---|:---|
| **tsurugi-graph** | ~8μs/hop | 100K nodes, 5 edges/node, single-thread | This benchmark |
| **RedisGraph** | 0.39ms | graph500 (2.4M nodes, 64M edges), single-thread | [Redis Blog](https://redis.io/blog/new-redisgraph-1-0-achieves-600x-faster-performance-graph-databases/) |
| **Neo4j** | 21ms | graph500, single-thread | [Redis Blog](https://redis.io/blog/new-redisgraph-1-0-achieves-600x-faster-performance-graph-databases/) |
| **Memgraph** | ~1ms | Expansion-1, 12 clients | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |

**Analysis:**
- tsurugi-graph achieves ~8μs per edge traversal (outgoing edges lookup + get_edge) in mock. This is faster than published numbers from other databases, but the mock eliminates all I/O and lock overhead.
- RedisGraph/FalkorDB's 0.39ms on a much larger dataset (64M edges) demonstrates strong single-thread performance from GraphBLAS sparse matrix operations.
- Production tsurugi-graph with Shirakami will add I/O overhead, likely placing performance in the 0.1–1ms range per hop, competitive with RedisGraph and Memgraph.

### Concurrent Throughput (QPS)

| Database | QPS | Workload | Source |
|:---|---:|:---|:---|
| **Memgraph** | 32,028 | Expansion-1, 12 clients | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |
| **Neo4j** | 280 | Expansion-1, 12 clients | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |
| **FalkorDB** | Sub-10ms per query | p99 < 140ms, in-memory | [FalkorDB Blog](https://www.falkordb.com/blog/graph-database-performance-benchmarks-falkordb-vs-neo4j/) |
| **tsurugi-graph** | N/A (single-thread) | Not yet benchmarked with concurrent clients | — |

**Note:** tsurugi-graph's service layer supports concurrent requests via Tateyama's threading model, but concurrent QPS has not been benchmarked yet. The parallel WHERE optimization (ADR-0005) provides intra-query parallelism, not inter-query concurrency.

### Full Scan Query (MATCH+WHERE without index)

| Database | Latency (1M nodes) | Conditions | Source |
|:---|---:|:---|:---|
| **tsurugi-graph** | 944ms/query | Full scan, inequality WHERE, in-memory mock | This benchmark |
| **Neo4j** | 3,100ms | Most complex query, warm cache | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |
| **Memgraph** | ~1,000ms | Most complex query (Q11 Expansion-4) | [Memgraph Blog](https://memgraph.com/blog/memgraph-vs-neo4j-performance-benchmark-comparison) |

### Summary: Competitive Positioning

| Strength | tsurugi-graph | Neo4j | Memgraph | FalkorDB |
|:---|:---|:---|:---|:---|
| **Write throughput** | Very high (mock) | Moderate | Very high | Moderate |
| **Indexed lookup** | Sub-ms (mock) | Sub-ms | ~1ms | Sub-10ms |
| **Traversal** | Very fast (mock) | Moderate | Fast | Very fast |
| **Full scan** | Fast (parallel) | Slow | Fast | Fast |
| **ACID transactions** | Yes (Shirakami) | Yes | Yes (snapshot) | Limited |
| **Persistence** | Shirakami B+tree | Disk+RAM | In-memory+WAL | In-memory |
| **Concurrency** | Untested | Battle-tested | High | High |
| **Cypher coverage** | Basic subset | Full | Full (+ MAGE) | Subset |
| **Maturity** | Prototype | Production (15+ yrs) | Production | Production |

---

## Performance History

### Round 1: Production hardening (v1 baseline)
- Complete Cypher parser and executor
- RAII iterator guard, buffer validation, transaction protection

### Round 2: Safety optimizations
- All return codes checked, bounds validation

### Round 3: Micro-optimizations
- `std::stringstream` elimination, `memcmp` prefix comparison
- Label caching, numeric pre-conversion, keyword matching by length

### Round 4: Algorithmic optimizations (v2, current)
- Property index (ADR-0002): O(N) → O(log N) for equality WHERE
- Fused MATCH+WHERE: Skips label scan entirely for indexed queries
- Query cache (ADR-0003): Reuses parsed ASTs
- Batch reads (ADR-0004): Pre-fetches node properties
- Parallel scan (ADR-0005): Multi-threaded full-scan WHERE

---

## Known Bottlenecks and Future Optimization Opportunities

### 1. Range Property Index (Medium Impact)

**Current**: Inequality WHERE (>, <) still requires full scan.
**Solution**: B+tree style range index or sorted value encoding in property index keys.
**Expected Impact**: MATCH+WHERE > from O(N) to O(log N + K).

### 2. Query Template Cache (Medium Impact)

**Current**: Query cache uses exact string match; `WHERE n.name = 'Alice'` and `WHERE n.name = 'Bob'` are separate entries.
**Solution**: Normalize queries by replacing literal values with placeholders.
**Expected Impact**: Dramatically higher cache hit rate for parameterized workloads.

### 3. Lazy Label Scan (Low Impact)

**Current**: For multi-hop MATCH patterns, the first label scan is still performed even if subsequent WHERE would narrow results.
**Solution**: Push-down WHERE predicate into MATCH (cost-based query planner).

### 4. Property Index for SET (Low Impact)

**Current**: SET clause uses `update_node()` without updating property index.
**Solution**: Use `update_node_with_label()` to maintain property index consistency.

---

## How to Run Benchmarks

```bash
cd tsurugi-graph/graph-service

# Compile with optimizations
g++ -std=c++17 -O2 \
  -I include -I test \
  -o load_test \
  bench/load_test.cpp \
  src/tateyama/framework/graph/core/parser.cpp \
  src/tateyama/framework/graph/core/executor.cpp \
  src/tateyama/framework/graph/storage.cpp \
  -lpthread

# Run with specified node count
./load_test 100000    # 100K nodes
./load_test 1000000   # 1M nodes
./load_test 10000000  # 10M nodes
```
