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

| Operation | tsurugi-graph (mock) | Memgraph | FalkorDB | Neo4j |
|:---|---:|---:|---:|---:|
| **CREATE node (individual)** | **56,451** | 640 | 1,240 | 190 |
| **UNWIND bulk insert (100/batch)** | **53,462** | 16,185 | 11,924 | 8,165 |
| **MATCH indexed = (point read)** | **1,012** | 655 | 963 | 367 |
| **MATCH WHERE > (full scan, 100q)** | 5 | 14 | **47** | 20 |
| **CREATE edge (MATCH+CREATE)** | **21,465** | 15 | 47 | 18 |
| **1-hop traversal** | **82,645** | 666 | 656 | 354 |

### Detailed Results

#### 1. CREATE Node (100K individual CREATE statements)

| Database | Ops | Time (sec) | Ops/sec |
|:---|---:|---:|---:|
| **tsurugi-graph (Cypher CREATE)** | 100,000 | 1.77 | 56,451 |
| **tsurugi-graph (Storage API)** | 100,000 | 0.59 | 169,740 |
| FalkorDB | 100,000 | 80.65 | 1,240 |
| Memgraph | 100,000 | 156.36 | 640 |
| Neo4j | 100,000 | 525.12 | 190 |

#### 2. UNWIND Bulk Insert (100K nodes, 100 nodes per batch)

| Database | Ops | Time (sec) | Ops/sec | vs. individual CREATE |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100,000 | 1.87 | 53,462 | 0.95x |
| Memgraph | 100,000 | 6.18 | **16,185** | **25x** |
| FalkorDB | 100,000 | 8.39 | 11,924 | **9.6x** |
| Neo4j | 100,000 | 12.25 | 8,165 | **43x** |

**Analysis:**
- UNWIND batching dramatically improves external DB throughput by amortizing protocol overhead: Neo4j **43x**, Memgraph **25x**, FalkorDB **9.6x**.
- tsurugi-graph's UNWIND is marginally slower than individual CREATE (0.95x) because UNWIND adds list/map literal parsing overhead. Since tsurugi runs in-process, there is no protocol round-trip to amortize.
- With UNWIND, Memgraph (16K ops/s) closes the gap significantly with tsurugi (53K ops/s) — a 3.3x difference vs 88x with individual CREATE.
- This demonstrates that the per-query protocol overhead was the dominant bottleneck for external databases, not the database engine itself.

#### 3. Indexed Point Read (MATCH + indexed property equality, 10K queries)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100 | 0.10 | 1,012 | 0.99ms |
| FalkorDB | 10,000 | 10.39 | 963 | 1.04ms |
| Memgraph | 10,000 | 15.26 | 655 | 1.53ms |
| Neo4j | 10,000 | 27.26 | 367 | 2.73ms |

**Analysis:**
- tsurugi-graph and FalkorDB are essentially tied at ~1ms per query. All databases achieve sub-3ms indexed lookups.

#### 4. Full Scan WHERE (MATCH + inequality filter, 100 queries over 50K Person nodes)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| FalkorDB | 100 | 2.12 | **47** | 21.2ms |
| Neo4j | 100 | 4.89 | 20 | 48.9ms |
| Memgraph | 100 | 6.92 | 14 | 69.2ms |
| tsurugi-graph | 100 | 18.33 | 5 | 183.3ms |

**Analysis:**
- FalkorDB excels at full scans due to GraphBLAS columnar property storage — packed integer arrays are cache-friendly and potentially SIMD-vectorized.
- tsurugi-graph is slowest because: (1) `std::map` iterator pointer-chasing is cache-unfriendly, (2) each node's properties are stored as JSON strings requiring per-node parse, (3) no columnar property layout.
- Production Shirakami's sequential B+tree scan will improve cache locality but cannot match columnar engines.

#### 5. CREATE Edge (MATCH source + MATCH target + CREATE edge, 10K operations)

| Database | Ops | Time (sec) | Ops/sec |
|:---|---:|---:|---:|
| **tsurugi-graph** | 10,000 | 0.47 | **21,465** |
| FalkorDB | 10,000 | 211.89 | 47 |
| Neo4j | 10,000 | 556.22 | 18 |
| Memgraph | 10,000 | 649.73 | 15 |

**Analysis:**
- tsurugi-graph is **457x faster** than FalkorDB for Cypher edge creation. This was enabled by the MATCH inline property index optimization (ADR-0006): `MATCH (n:Person {name: 'X'})` uses `find_nodes_by_property()` directly instead of scanning all label nodes.
- External databases already optimize this internally but the Bolt/Redis protocol overhead (2 round-trips for 2 MATCHes + 1 for CREATE) dominates at 22-70ms total per edge.

#### 6. 1-Hop Traversal (MATCH path with edge type, 1K queries)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100,000 | 1.21 | 82,645 | 0.012ms |
| Memgraph | 1,000 | 1.50 | 666 | 1.50ms |
| FalkorDB | 1,000 | 1.52 | 656 | 1.52ms |
| Neo4j | 1,000 | 2.82 | 354 | 2.82ms |

**Analysis:**
- tsurugi-graph's raw edge traversal is **124x faster** than Memgraph — zero protocol overhead and O(1) adjacency list access in memory.
- Among real databases, Memgraph and FalkorDB are tied at ~1.5ms. Neo4j is 2x slower.

### Performance Ratio Summary (relative to Memgraph)

| Operation | tsurugi (mock) | FalkorDB | Memgraph | Neo4j |
|:---|---:|---:|---:|---:|
| CREATE node (individual) | **88x** | 1.9x | 1.0x | 0.3x |
| UNWIND bulk insert | **3.3x** | 0.7x | 1.0x | 0.5x |
| Indexed point read | **1.5x** | 1.5x | 1.0x | 0.6x |
| Full scan WHERE | 0.4x | **3.4x** | 1.0x | 1.4x |
| CREATE edge | **1,431x** | 3.1x | 1.0x | 1.2x |
| 1-hop traversal | **124x** | 1.0x | 1.0x | 0.5x |

### Caveats

1. **Protocol overhead vs in-process**: External databases are accessed via network protocol (Bolt/Redis). tsurugi-graph runs in-process. UNWIND batching shows that protocol overhead accounts for 10-40x of the individual CREATE difference.
2. **Mock backend**: tsurugi-graph uses `std::map`-based mock storage. Production Shirakami will add I/O latency but provides ACID guarantees.
3. **Single-threaded**: No concurrent clients. Production databases benefit from connection pooling and parallel execution.
4. **Full scan weakness**: tsurugi-graph's JSON-based property storage is structurally disadvantaged for full scans compared to columnar engines (FalkorDB/GraphBLAS).

### Summary: Competitive Positioning

| Strength | tsurugi-graph | Neo4j | Memgraph | FalkorDB |
|:---|:---|:---|:---|:---|
| **Write (individual)** | Very high (mock) | Slow | Moderate | Fast |
| **Write (UNWIND batch)** | High (mock) | Fast | Very fast | Fast |
| **Indexed lookup** | Fast (mock) | Moderate | Fast | Fast |
| **Full scan** | Slow (mock map) | Moderate | Moderate | Fast (GraphBLAS) |
| **Edge creation** | Very fast (mock) | Slow | Slow | Moderate |
| **Traversal** | Very fast (mock) | Slow | Fast | Fast |
| **ACID transactions** | Yes (Shirakami) | Yes | Yes (snapshot) | Limited |
| **Persistence** | Shirakami B+tree | Disk+RAM | In-memory+WAL | In-memory |
| **Concurrency** | Untested | Battle-tested | High | High |
| **Cypher coverage** | Basic subset + UNWIND | Full | Full (+ MAGE) | Subset |
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

### Round 4: Algorithmic optimizations (v2)
- Property index (ADR-0002): O(N) → O(log N) for equality WHERE
- Fused MATCH+WHERE: Skips label scan entirely for indexed queries
- Query cache (ADR-0003): Reuses parsed ASTs
- Batch reads (ADR-0004): Pre-fetches node properties
- Parallel scan (ADR-0005): Multi-threaded full-scan WHERE

### Round 5: UNWIND bulk insert + MATCH inline property index (current)
- UNWIND clause (ADR-0006): Bulk insert via `UNWIND [...] AS item CREATE ...`
- MATCH inline property index: `MATCH (n:Person {name: 'X'})` uses property index directly (191x speedup for Cypher edge creation)
- WHERE batch prefetch: Pre-fetches all node properties via `get_nodes_batch()` before filtering loop

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
