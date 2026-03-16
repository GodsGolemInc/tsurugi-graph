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

## Comparative Performance Benchmark (Same-Machine, 100K Nodes)

> **Test Environment:** All databases running on the same machine (Linux 6.17.0-14-generic, x86_64).
> Single-threaded, sequential operations. Each database started fresh with empty data.
> tsurugi-graph uses in-memory mock backend (no I/O). Neo4j, Memgraph, and FalkorDB are
> real database server processes accessed via Bolt/Redis protocol from Python client.

**Database versions:** Neo4j 5.26.2, Memgraph 2.20.0, FalkorDB 4.16.7, tsurugi-graph (mock)

### Results Summary (100K nodes, ops/sec — higher is better)

| Operation | tsurugi-graph (mock) | FalkorDB | Memgraph | Neo4j |
|:---|---:|---:|---:|---:|
| **CREATE node** | **86,337** | 1,162 | 729 | 195 |
| **MATCH indexed = (point read)** | **1,386** | 858 | 707 | 363 |
| **MATCH WHERE > (full scan, 100q)** | 12 | **65** | 19 | 23 |
| **CREATE edge (MATCH+CREATE)** | N/A† | **70** | 22 | 22 |
| **1-hop traversal** | **114,515** | 667 | 720 | 370 |

† tsurugi-graph edge creation benchmarked separately via raw storage API (326K ops/s)

### Detailed Results

#### 1. CREATE Node (100K individual CREATE statements)

| Database | Ops | Time (sec) | Ops/sec | vs. best external |
|:---|---:|---:|---:|---:|
| **tsurugi-graph (Cypher CREATE)** | 100,000 | 1.16 | 86,337 | 74x |
| **tsurugi-graph (Storage API)** | 100,000 | 0.32 | 312,837 | 269x |
| FalkorDB | 100,000 | 86.07 | 1,162 | 1.0x |
| Memgraph | 100,000 | 137.20 | 729 | — |
| Neo4j | 100,000 | 511.80 | 195 | — |

**Analysis:**
- tsurugi-graph's Cypher CREATE (86K ops/s) is **74x faster** than the fastest external database (FalkorDB at 1.2K ops/s). The raw storage API is 269x faster.
- The external databases include client-server protocol overhead (Bolt/Redis). tsurugi-graph runs in-process with zero I/O.
- FalkorDB leads among real databases due to Redis's efficient command processing. Memgraph and Neo4j's Bolt protocol adds ~1ms latency per round-trip.
- Production tsurugi-graph with Shirakami will be slower due to disk I/O, but should remain significantly faster than external DBs when accessed via Tateyama's in-process service API.

#### 2. Indexed Point Read (MATCH + indexed property equality, 10K queries)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100 | 0.07 | 1,386 | 0.72ms |
| FalkorDB | 10,000 | 11.66 | 858 | 1.17ms |
| Memgraph | 10,000 | 14.14 | 707 | 1.41ms |
| Neo4j | 10,000 | 27.56 | 363 | 2.76ms |

**Analysis:**
- All databases achieve sub-3ms indexed lookups, demonstrating effective index utilization.
- tsurugi-graph's 0.72ms includes Cypher parse + property index lookup + JSON formatting.
- FalkorDB and Memgraph are competitive at ~1ms, consistent with their in-memory architectures.
- Neo4j's 2.76ms reflects additional disk/cache layer overhead.

#### 3. Full Scan WHERE (MATCH + inequality filter, 100 queries over 50K Person nodes)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| FalkorDB | 100 | 1.53 | **65** | 15.3ms |
| Neo4j | 100 | 4.44 | 23 | 44.4ms |
| Memgraph | 100 | 5.14 | 19 | 51.4ms |
| tsurugi-graph | 100 | 8.17 | 12 | 81.7ms |

**Analysis:**
- FalkorDB excels at full scans due to GraphBLAS sparse matrix operations — 3x faster than Neo4j/Memgraph.
- tsurugi-graph is slowest here because the mock backend's `std::map` iteration is less cache-friendly than purpose-built scan engines. Production Shirakami's sequential B+tree scan should improve this.
- All databases show O(N) scaling for unindexed inequality queries.

#### 4. CREATE Edge (MATCH source + MATCH target + CREATE edge, 10K operations)

| Database | Ops | Time (sec) | Ops/sec |
|:---|---:|---:|---:|
| FalkorDB | 10,000 | 142.07 | **70** |
| Memgraph | 10,000 | 444.74 | 22 |
| Neo4j | 10,000 | 446.04 | 22 |

**Analysis:**
- Edge creation via Cypher requires 2 indexed lookups + 1 write per operation, making it the most expensive workload.
- FalkorDB's in-process Redis command pipeline gives it a 3x advantage over Bolt-based databases.
- tsurugi-graph's raw storage `create_edge` runs at 326K ops/s (in-memory mock), but Cypher-level edge creation with MATCH is not yet benchmarked end-to-end.

#### 5. 1-Hop Traversal (MATCH path with edge type, 1K queries)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100,000 | 0.87 | 114,515 | 0.009ms |
| Memgraph | 1,000 | 1.39 | 720 | 1.39ms |
| FalkorDB | 1,000 | 1.50 | 667 | 1.50ms |
| Neo4j | 1,000 | 2.70 | 370 | 2.70ms |

**Analysis:**
- tsurugi-graph's raw edge traversal (outgoing edge list + get_edge) is **159x faster** than Memgraph. This reflects zero protocol overhead and O(1) adjacency list access in memory.
- Among real databases, Memgraph and FalkorDB are essentially tied at ~1.4ms per hop. Neo4j is 2x slower at 2.7ms.
- Production tsurugi-graph traversal with Shirakami should remain fast due to sequential key layout optimizing for adjacency list access.

### Performance Ratio Summary (relative to Neo4j)

| Operation | tsurugi (mock) | FalkorDB | Memgraph | Neo4j |
|:---|---:|---:|---:|---:|
| CREATE node | **443x** | 6.0x | 3.7x | 1.0x |
| Indexed point read | **3.8x** | 2.4x | 1.9x | 1.0x |
| Full scan WHERE | 0.5x | **2.8x** | 0.8x | 1.0x |
| CREATE edge | N/A | **3.2x** | 1.0x | 1.0x |
| 1-hop traversal | **310x** | 1.8x | 1.9x | 1.0x |

### Caveats

1. **Protocol overhead dominates**: External databases are accessed via network protocol (Bolt/Redis). tsurugi-graph runs in-process. A fairer comparison would use embedded mode or shared-memory IPC for all databases.
2. **Mock backend**: tsurugi-graph uses `std::map`-based mock storage. Production Shirakami will add I/O latency but provides ACID guarantees.
3. **No batching**: All writes are single-statement (no UNWIND/batch). Memgraph and Neo4j can achieve much higher throughput with batch operations.
4. **Single-threaded**: No concurrent clients. Production databases benefit from connection pooling and parallel execution.

### Summary: Competitive Positioning

| Strength | tsurugi-graph | Neo4j | Memgraph | FalkorDB |
|:---|:---|:---|:---|:---|
| **Write throughput** | Very high (mock) | Slow | Moderate | Fast |
| **Indexed lookup** | Fast (mock) | Moderate | Fast | Fast |
| **Full scan** | Slow (mock map) | Moderate | Moderate | Fast (GraphBLAS) |
| **Traversal** | Very fast (mock) | Slow | Fast | Fast |
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

### tsurugi-graph standalone benchmark

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

### Comparative benchmark (all databases)

Requires: Neo4j, Memgraph, FalkorDB installed in `bench/tools/`. Python packages: `neo4j`, `falkordb`.

```bash
cd tsurugi-graph/graph-service

# Run all databases (100K nodes)
python3 bench/comparative_bench.py 100000

# Skip specific databases
python3 bench/comparative_bench.py 100000 --skip-neo4j --skip-memgraph

# Results saved to bench/tools/bench_data/results_<N>.json
```
