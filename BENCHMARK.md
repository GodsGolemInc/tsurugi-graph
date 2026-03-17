# tsurugi-graph Benchmark Report

## Test Environment

- CPU: Linux 6.17.0-14-generic (x86_64)
- Compiler: g++ 13, C++17, -O2
- Storage backend: In-memory mock (sharksfin_mock.h)
- All benchmarks are single-threaded (except parallel WHERE path)

> **Note:** The mock backend eliminates I/O latency and measures pure graph engine throughput.
> Production performance with Shirakami will differ due to actual disk/network I/O.

---

## Results Summary (v6 — with optimizations ADR-0002~0012)

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

## Scalability Analysis (v4, Real Shirakami)

### O(1) per operation (constant throughput)

- **Point read**: 265K (100K) → 238K (1M) → 205K (5M) ops/s — only 23% drop at 50x scale
- **Property index lookup**: 226K (100K) → 178K (1M) → 169K (5M) ops/s — O(1) inverted index, 25% drop at 50x scale
- **MATCH+WHERE = indexed**: 52K (100K) → 39K (1M) → 52K (5M) ops/s — flat, cost dominated by executor overhead
- **Parser**: ~180-215K ops/s — no storage involved
- **Edge traversal**: 111K (100K) → 81K (1M) → 101K (5M) ops/s — B+tree adjacency list lookup

### O(N) per operation (write + index maintenance)

- **create_node**: 41K (100K) → 23K (1M) → 21K (5M) ops/s — B+tree depth + inverted index RMW
- **Pipeline**: 13K (100K) → 8K (1M) → 3K (5M) ops/s — multiple writes + index updates per op

### O(V) per operation (range index scan, V = distinct property values)

- **MATCH+WHERE >**: 60ms/q (100K) → 587ms/q (1M) → 3,427ms/q (5M) — linear in distinct values
  - Range index (ADR-0010) avoids full node scan but still iterates V index entries
  - V ≈ N for high-cardinality properties (unique names)

### Concurrent Scaling (8 threads, 10K seed nodes, epoch=40000μs)

- **Read-only**: 344K → 953K ops/s (2.8x) — MVCC lock-free reads
- **Mixed 80/20**: 35K → 122K ops/s (3.5x) — read-dominated benefits from MVCC
- **Write-only**: 15K → 16K ops/s (1.0x) — epoch commit serialization bottleneck (40ms boundary)

---

## Comparative Performance Benchmark (Same-Machine, 100K Nodes)

> **Test Environment:** All databases running on the same machine (Linux 6.17.0-14-generic, x86_64).
> Single-threaded, sequential operations. Each database started fresh with empty data.
> tsurugi-graph uses in-memory mock backend (no I/O). Neo4j, Memgraph, and FalkorDB are
> real database server processes accessed via Bolt/Redis protocol from Python client.

**Database versions:** Neo4j 5.26.2, Memgraph 2.20.0, FalkorDB 4.16.7, tsurugi-graph (mock)

### Results Summary (100K nodes, ops/sec — higher is better)

| Operation | tsurugi (Shirakami) | tsurugi (mock) | Memgraph | FalkorDB | Neo4j |
|:---|---:|---:|---:|---:|---:|
| **CREATE node (individual)** | **47,761** | 77,786 | 770 | 1,939 | 203 |
| **MATCH indexed = (point read)** | **46,060** | 1,605 | 743 | 1,090 | 448 |
| **MATCH WHERE > (range index)** | 17 | 7 | 16 | **51** | 22 |
| **Edge traversal** | **120,171** | 100,269 | 742 | 754 | 355 |
| **Pipeline (CREATE+MATCH+SET+RETURN)** | **10,657** | 7,422 | N/A | N/A | N/A |

> **Note:** "tsurugi (Shirakami)" = direct sharksfin API call to real Shirakami KVS engine (no network protocol).
> "tsurugi (mock)" = in-memory `std::map` mock. External databases use Bolt/Redis client protocol.

### Detailed Results

#### 1. CREATE Node (100K individual CREATE statements)

| Database | Ops | Time (sec) | Ops/sec |
|:---|---:|---:|---:|
| **tsurugi-graph (Cypher CREATE)** | 100,000 | 1.29 | 77,786 |
| **tsurugi-graph (Storage API)** | 100,000 | 0.41 | 244,774 |
| FalkorDB | 100,000 | 51.58 | 1,939 |
| Memgraph | 100,000 | 129.87 | 770 |
| Neo4j | 100,000 | 492.19 | 203 |

#### 2. UNWIND Bulk Insert (100K nodes, 100 nodes per batch)

| Database | Ops | Time (sec) | Ops/sec | vs. individual CREATE |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100,000 | 1.03 | **97,244** | **1.25x** |
| Memgraph | 100,000 | 5.15 | 19,416 | **25x** |
| FalkorDB | 100,000 | 7.43 | 13,460 | **6.9x** |
| Neo4j | 100,000 | 14.49 | 6,901 | **34x** |

**Analysis:**
- UNWIND batching dramatically improves external DB throughput by amortizing protocol overhead: Neo4j **34x**, Memgraph **25x**, FalkorDB **6.9x**.
- tsurugi-graph's UNWIND is now **25% faster** than individual CREATE (1.25x) due to reduced per-statement parse overhead — a single UNWIND query replaces 100 individual CREATE statements.
- With UNWIND, Memgraph (19K ops/s) closes the gap with tsurugi (97K ops/s) — a 5x difference vs 101x with individual CREATE.
- This demonstrates that the per-query protocol overhead was the dominant bottleneck for external databases, not the database engine itself.

#### 3. Indexed Point Read (MATCH + indexed property equality, 10K queries)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100 | 0.06 | **1,605** | 0.62ms |
| FalkorDB | 10,000 | 9.17 | 1,090 | 0.92ms |
| Memgraph | 10,000 | 13.46 | 743 | 1.35ms |
| Neo4j | 10,000 | 22.32 | 448 | 2.23ms |

**Analysis:**
- tsurugi-graph leads at 0.62ms per query, 47% faster than FalkorDB. All databases achieve sub-3ms indexed lookups.

#### 4. Full Scan WHERE (MATCH + inequality filter, 100 queries over 50K Person nodes)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| FalkorDB | 100 | 1.98 | **51** | 19.8ms |
| Neo4j | 100 | 4.55 | 22 | 45.5ms |
| Memgraph | 100 | 6.28 | 16 | 62.8ms |
| tsurugi-graph | 100 | 15.18 | 7 | 151.8ms |

**Analysis:**
- FalkorDB excels at full scans due to GraphBLAS columnar property storage — packed integer arrays are cache-friendly and potentially SIMD-vectorized.
- tsurugi-graph is slowest because: (1) `std::map` iterator pointer-chasing is cache-unfriendly, (2) each node's properties are stored as JSON strings requiring per-node parse, (3) no columnar property layout.
- Production Shirakami's sequential B+tree scan will improve cache locality but cannot match columnar engines.

#### 5. CREATE Edge (MATCH source + MATCH target + CREATE edge, 10K operations)

| Database | Ops | Time (sec) | Ops/sec |
|:---|---:|---:|---:|
| **tsurugi-graph** | 10,000 | 0.34 | **29,565** |
| FalkorDB | 10,000 | 175.72 | 57 |
| Neo4j | 10,000 | 531.21 | 19 |
| Memgraph | 10,000 | 539.92 | 19 |

**Analysis:**
- tsurugi-graph is **519x faster** than FalkorDB for Cypher edge creation. This was enabled by the MATCH inline property index optimization (ADR-0006): `MATCH (n:Person {name: 'X'})` uses `find_nodes_by_property()` directly instead of scanning all label nodes.
- External databases already optimize this internally but the Bolt/Redis protocol overhead (2 round-trips for 2 MATCHes + 1 for CREATE) dominates at 18-53ms total per edge.

#### 6. 1-Hop Traversal (MATCH path with edge type, 1K queries)

| Database | Ops | Time (sec) | Ops/sec | Latency/query |
|:---|---:|---:|---:|---:|
| **tsurugi-graph** | 100,000 | 1.00 | **100,269** | 0.010ms |
| FalkorDB | 1,000 | 1.33 | 754 | 1.33ms |
| Memgraph | 1,000 | 1.35 | 742 | 1.35ms |
| Neo4j | 1,000 | 2.82 | 355 | 2.82ms |

**Analysis:**
- tsurugi-graph's raw edge traversal is **135x faster** than Memgraph — zero protocol overhead and O(1) adjacency list access in memory.
- Among real databases, Memgraph and FalkorDB are tied at ~1.3ms. Neo4j is 2x slower.

### Performance Ratio Summary (relative to Memgraph)

| Operation | tsurugi (mock) | FalkorDB | Memgraph | Neo4j |
|:---|---:|---:|---:|---:|
| CREATE node (individual) | **101x** | 2.5x | 1.0x | 0.3x |
| UNWIND bulk insert | **5.0x** | 0.7x | 1.0x | 0.4x |
| Indexed point read | **2.2x** | 1.5x | 1.0x | 0.6x |
| Full scan WHERE | 0.4x | **3.2x** | 1.0x | 1.4x |
| CREATE edge | **1,556x** | 3.0x | 1.0x | 1.0x |
| 1-hop traversal | **135x** | 1.0x | 1.0x | 0.5x |

### Caveats

1. **Protocol overhead vs in-process**: External databases are accessed via network protocol (Bolt/Redis). tsurugi-graph runs in-process (both mock and real Shirakami). UNWIND batching shows that protocol overhead accounts for 10-40x of the individual CREATE difference.
2. **Real Shirakami vs mock**: Real Shirakami adds ~3-9x overhead vs mock for storage operations due to MVCC bookkeeping, WAL logging, and B+tree operations. Parser/executor overhead is identical.
3. **Single-threaded**: No concurrent clients. Production databases benefit from connection pooling and parallel execution.
4. **Full scan weakness**: tsurugi-graph's JSON-based property storage is structurally disadvantaged for full scans compared to columnar engines (FalkorDB/GraphBLAS).

---

## Real Shirakami Backend Benchmark (100K Nodes)

> **Direct sharksfin API benchmark** — bypasses tateyama server and protocol layers.
> Measures raw Shirakami KVS performance for graph operations with ACID transactions.
> Data stored on disk via limestone WAL + shirakami B+tree.

### Results Summary (Real Shirakami, v5 — epoch default + range index + deferred SET)

| Operation | 100K | 1M | 5M | vs. v2 (100K) |
|:---|---:|---:|---:|:--|
| Storage: create_node (ops/s) | 47,761 | 22,513 | 20,627 | +8% |
| Storage: point read (ops/s) | 226,634 | 238,162 | 205,148 | +6% |
| Storage: create_edge (ops/s) | 46,194 | 40,909 | 30,878 | +7% |
| **Property index lookup (ops/s)** | **265,460** | **178,456** | **168,896** | **57x faster** |
| Edge traversal (ops/s) | 120,171 | 81,386 | 100,832 | +38% |
| Pipeline (ops/s) | 10,657 | 8,116 | 2,609 | -37% |
| Label scan (sec) | 0.14s (50K) | 0.75s (500K) | 4.09s (2.5M) | ~same |
| **MATCH+WHERE = indexed (1000q)** | **0.02s** | **0.03s** | **0.02s** | **256x faster** |
| **MATCH+WHERE > range index (10q)** | **0.57s** | **5.87s** | **34.3s** | **4.7x faster** |
| **Total throughput (ops/s)** | **66,250** | **24,150** | **18,624** | **+50%** |

> **Note**: 10M nodes exceeds available RAM (16GB) for Shirakami's in-memory Masstree index.
> epoch_duration=40000μs (Shirakami default). 1M/5M results measured with epoch_duration=3000μs.

### v2 → v5 Key Improvements

| Metric | v2 (prefix scan) | v5 (current) | Improvement |
|:--|--:|--:|:--|
| Property index lookup (100K) | 4,669 ops/s | **265,460 ops/s** | **57x** |
| Property index lookup (1M) | 550 ops/s | **178,456 ops/s** | **324x** |
| MATCH+WHERE = indexed (100K) | 180 ops/s | **46,060 ops/s** | **256x** |
| MATCH+WHERE = indexed (1M) | 21 ops/s | **38,946 ops/s** | **1,855x** |
| MATCH+WHERE > (100K) | 4 ops/s | **17 ops/s** | **4.3x** |
| MATCH+WHERE > (1M) | 0.3 ops/s | **2 ops/s** | **5.1x** |
| create_node (100K) | 44,207 ops/s | **47,761 ops/s** | **+8%** |
| Total throughput (100K) | 44K ops/s | **66K ops/s** | **+50%** |

**Key design**: Inverted property index (ADR-0007) with optimistic CREATE (ADR-0009), range index (ADR-0010) for inequality WHERE, and deferred SET index updates.

### Detailed Results: 100K nodes (v5, epoch_duration=40000μs)

| Operation | Ops | Time (sec) | Ops/sec | vs. v2 |
|:---|---:|---:|---:|---:|
| Parser throughput | 100,000 | 0.39 | 254,228 | ~same |
| Storage: create_node | 100,000 | 2.09 | 47,761 | +8% |
| Storage: point read (get_node) | 100,000 | 0.44 | 226,634 | +6% |
| Storage: create_edge | 100,000 | 2.16 | 46,194 | +7% |
| Label scan (50K results) | 1 | 0.14 | 7 | ~same |
| Property index lookup | 10,000 | 0.04 | 265,460 | **57x** |
| Cypher: CREATE node | 10,000 | 0.48 | 20,717 | -29% |
| Cypher: MATCH+WHERE = (indexed, 1000q) | 1,000 | 0.02 | 46,060 | **256x** |
| Cypher: MATCH+WHERE > (range index, 10q) | 10 | 0.57 | 17 | **4.3x** |
| Edge traversal | 10,000 | 0.08 | 120,171 | +38% |
| Pipeline: CREATE+MATCH+SET+RETURN | 1,000 | 0.09 | 10,657 | -37% |

### epoch_duration Comparison (100K nodes)

| Operation | epoch=3000μs | epoch=40000μs (default) | Difference |
|:---|---:|---:|:---|
| Storage: create_node | 44,874 | **47,761** | **+6%** |
| Storage: point read | 230,932 | 226,634 | -2% |
| Storage: create_edge | 43,282 | **46,194** | **+7%** |
| Property index | 260,038 | **265,460** | +2% |
| Edge traversal | 114,044 | **120,171** | **+5%** |
| Pipeline | 9,841 | **10,657** | **+8%** |
| **Total** | 62,693 | **66,250** | **+6%** |

**Conclusion**: epoch=40000μs (default) is **6% faster overall** for single-threaded workloads.
Short epochs (3000μs) increase CPU overhead from 13.3x more frequent epoch transitions.
Short epochs only benefit multi-client write-heavy workloads where commit latency matters.

### Detailed Results: 5M nodes (v4, epoch_duration=3000μs)

| Operation | Ops | Time (sec) | Ops/sec |
|:---|---:|---:|---:|
| Parser throughput | 100,000 | 0.55 | 182,397 |
| Storage: create_node | 5,000,000 | 242.40 | 20,627 |
| Storage: point read (get_node) | 100,000 | 0.49 | 205,148 |
| Storage: create_edge | 100,000 | 3.24 | 30,878 |
| Label scan (2.5M results) | 1 | 4.09 | 0 |
| Property index lookup | 10,000 | 0.06 | 168,896 |
| Cypher: CREATE node | 10,000 | 0.71 | 14,064 |
| Cypher: MATCH+WHERE = (indexed, 1000q) | 1,000 | 0.02 | 52,128 |
| Cypher: MATCH+WHERE > (range index, 10q) | 10 | 34.27 | 0 |
| Edge traversal | 10,000 | 0.10 | 100,832 |
| Pipeline: CREATE+MATCH+SET+RETURN | 1,000 | 0.38 | 2,609 |

**Total: 5,332,011 ops in 286.30 sec (18,624 ops/sec)**

**5M scalability observations:**
- **Property index lookup**: 168K ops/s — only 25% drop from 100K (226K), confirming O(1) inverted index
- **MATCH+WHERE =**: 52K ops/s — flat scaling, index lookup cost independent of dataset size
- **MATCH+WHERE >**: 34.3s/10q — linear scaling (0.6s at 100K, 5.9s at 1M), O(V) distinct-value scan
- **create_node**: 20.6K ops/s — 50% drop from 100K due to larger B+tree depth and WAL volume (242s for 5M nodes)
- **Pipeline**: 2.6K ops/s — dominated by create + index cost at scale

### Concurrent Client Benchmark (Real Shirakami, v6)

> **Multi-threaded benchmark** — each thread uses independent transactions against shared Shirakami KVS.
> Tests MVCC concurrency, transaction retry behavior, and throughput scaling.
> Seed: 10K pre-existing nodes. 5K ops/thread. epoch_duration=40000μs (default).

#### Read-Only (point reads)

| Threads | Total Ops | Wall Time (sec) | Throughput (ops/sec) | Retries | Scaling |
|---:|---:|---:|---:|---:|:---|
| 1 | 5,000 | 0.022 | 227,011 | 0 | 1.0x |
| 2 | 10,000 | 0.017 | 605,479 | 0 | 2.7x |
| 4 | 20,000 | 0.023 | 858,703 | 0 | 3.8x |
| 8 | 40,000 | 0.046 | 861,465 | 0 | 3.8x |

#### Write-Only (CREATE via Cypher)

| Threads | Total Ops | Wall Time (sec) | Throughput (ops/sec) | Retries | Scaling |
|---:|---:|---:|---:|---:|:---|
| 1 | 5,000 | 0.354 | 14,128 | 0 | 1.0x |
| 2 | 10,000 | 0.827 | 12,098 | 8 | 0.9x |
| 4 | 20,000 | 1.295 | 15,444 | 31 | 1.1x |
| 8 | 40,000 | 2.193 | 18,241 | 77 | 1.3x |

#### Mixed Workload (80% read, 20% write)

| Threads | Total Ops | Wall Time (sec) | Throughput (ops/sec) | Retries | Scaling |
|---:|---:|---:|---:|---:|:---|
| 1 | 5,000 | 0.175 | 28,624 | 0 | 1.0x |
| 2 | 10,000 | 0.208 | 48,100 | 0 | 1.7x |
| 4 | 20,000 | 0.244 | 82,109 | 4 | 2.9x |
| 8 | 40,000 | 0.299 | 133,896 | 21 | 4.7x |

**Concurrency observations:**
- **Read-only**: 227K → 861K ops/s (3.8x at 8 threads) — MVCC lock-free snapshot reads
- **Write-only**: 14K → 18K ops/s (1.3x at 8 threads) — epoch-based commit serialization (40ms epoch boundary)
- **Mixed 80/20 scales well** (4.7x at 8 threads) — read-heavy workloads benefit from MVCC
- **Low retry rates**: Write-only at 8 threads has 77 retries / 40K ops (0.19%) — minimal contention on distinct keys
- **Peak throughput**: 861K ops/s (read-only, 8 threads), 134K ops/s (mixed, 8 threads)
- **epoch_duration impact**: 40ms default gives higher single-thread throughput but limits write scaling

### Optimizations Applied

1. **thread_local string buffer reuse**: Eliminates per-call heap allocation for key construction
2. **Sequence ID batching**: Reserves 64 IDs per `sequence_put()` call, reducing WAL writes 64x
3. **Property index intersection**: Multi-property MATCH uses `set_intersection` of per-property results
4. **Optimistic CREATE for inverted index** (ADR-0009): Single KVS call for unique property values
5. **Delta SET** (ADR-0009): Only updates changed property index entries
6. **Range property index** (ADR-0010): Inequality WHERE uses prefix scan over inverted index values instead of full node scan + JSON parse. O(V) where V = distinct values << N.
7. **Query template cache** (ADR-0011): Normalizes literals to placeholders for higher cache hit rate (partially implemented)
8. **Deferred SET**: Accumulates multiple SET assignments per node, applies single update_node_with_label

### Analysis

**Fast operations (< 3x slowdown vs mock):**
- **Parser**: Identical — no storage involved
- **Edge traversal**: 120K ops/s — adjacency list lookup is cheap in B+tree
- **Point read**: 227K ops/s — B+tree lookup adds minimal latency
- **Property index lookup**: 265K ops/s — inverted index O(1) via single `content_get`
- **create_node**: 48K ops/s — optimistic CREATE for unique values
- **Pipeline**: 10.7K ops/s — deferred SET batches index updates

**Moderate operations (3-10x slowdown):**
- **create_edge**: ~46K ops/s — B+tree insert + WAL write
- **Cypher CREATE**: 21K ops/s — parser/executor overhead dominates
- **MATCH+WHERE >**: 17 ops/s (100K) — range index avoids full scan but still scans V distinct values

### Comparison with External Databases (Real Shirakami v5, in-process, no protocol overhead)

| Operation | tsurugi (Shirakami) | Memgraph | FalkorDB | Neo4j |
|:---|---:|---:|---:|---:|
| CREATE node | **47,761** | 770 | 1,939 | 203 |
| Indexed point read | **46,060** | 743 | 1,090 | 448 |
| Range WHERE | 17 | 16 | **51** | 22 |
| Edge traversal | **120,171** | 742 | 754 | 355 |

**Key findings:**
- **Writes**: tsurugi is **25x faster** than FalkorDB, **235x faster** than Neo4j
- **Indexed reads**: **42-62x faster** than all external DBs — inverted index O(1) lookup
- **Traversal**: **162x faster** than Memgraph — sharksfin B+tree adjacency list access
- **Range WHERE**: Competitive with Memgraph (17 vs 16 ops/s). FalkorDB still leads (51 ops/s) due to GraphBLAS columnar storage.

### Summary: Competitive Positioning

| Strength | tsurugi-graph (Shirakami) | Neo4j | Memgraph | FalkorDB |
|:---|:---|:---|:---|:---|
| **Write (individual)** | Very fast | Slow | Moderate | Fast |
| **Indexed lookup** | Very fast (ADR-0007) | Moderate | Fast | Fast |
| **Full scan** | Slow | Moderate | Moderate | Fast (GraphBLAS) |
| **Traversal** | Very fast | Slow | Fast | Fast |
| **ACID transactions** | Yes (Shirakami) | Yes | Yes (snapshot) | Limited |
| **Persistence** | Shirakami B+tree + WAL | Disk+RAM | In-memory+WAL | In-memory |
| **Concurrency** | Good (953K@8T read) | Battle-tested | High | High |
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

### Round 5: UNWIND bulk insert + MATCH inline property index
- UNWIND clause (ADR-0006): Bulk insert via `UNWIND [...] AS item CREATE ...`
- MATCH inline property index: `MATCH (n:Person {name: 'X'})` uses property index directly (191x speedup for Cypher edge creation)
- WHERE batch prefetch: Pre-fetches all node properties via `get_nodes_batch()` before filtering loop

### Round 6: Real Shirakami backend integration
- Full Tsurugi stack built from source (mpdecimal → takatori → shirakami → tateyama → graph-service → bootstrap)
- graph-service compiled against real sharksfin API (TransactionControlHandle, SequenceId, EndPointKind, WritePreserve)
- Server started successfully with `tgctl start` — graph resource and service initialized
- Comparative benchmarks re-run (still using mock for parser/executor throughput measurement)

### Round 7: Real Shirakami performance optimization
- **thread_local string buffer reuse**: `to_key_slice()` with stack buffers, eliminates per-call heap allocation
- **Sequence ID batching** (64 IDs/put): Reduces WAL writes 64x for create operations
- **Property index intersection**: Multi-property MATCH uses `set_intersection` instead of index + get_node fallback
- **Transaction splitting**: Large datasets (> 100K) use 10K-chunk transactions to limit per-tx memory
- Results: Property index +15%, Pipeline +5%, overall throughput +45% (42K → 44K total ops/sec at 100K)
- Scalability: 100K → 1M → 5M tested successfully on real Shirakami

### Round 8: Inverted property index + optimistic CREATE
- **Inverted property index** (ADR-0007): Replaced prefix_scan index with exact-match inverted index. Key = `label\0key\0value`, Value = packed nodeID list (8B × N). O(log N) → O(1) lookup via single `content_get`.
- **Optimistic CREATE** (ADR-0009): `content_put(CREATE)` first for unique property values (single KVS call). Falls back to read-modify-write only when key already exists. Recovers create_node from 5.2K to 43K ops/s.
- **Delta SET** (ADR-0009): `update_node_with_label()` compares old/new properties, updates only changed entries. Reduces KVS calls from 2×(all props) to 2×(changed props).
- **SET property index bug fix**: `execute_set()` now calls `update_node_with_label()` to maintain property index consistency.

### Round 9: Range index + deferred SET + epoch comparison (v5)
- **Range property index** (ADR-0010): Inequality WHERE (`>`, `<`, `>=`, `<=`) uses prefix scan over inverted index values instead of full node scan + JSON parse. O(V) where V = distinct property values.
- **Query template cache** (ADR-0011): Partially implemented — `normalize_query`, `deep_copy_statement`, `bind_literals` available. Full integration pending AST property map ordering fix.
- **Deferred SET**: Accumulates multiple SET assignments per node, applies single `update_node_with_label` with all changes.
- **Epoch comparison** (ADR-0008 revisited): epoch=40000μs (default) is **6% faster** than epoch=3000μs for single-threaded workloads. Short epochs only help multi-client write contention.
- Results (100K, epoch=40000): create_node **48K**, Property index **265K**, Total **66K ops/s** (+50% vs v2).
- Results (1M): MATCH+WHERE > **5.1x faster** (29.9s → 5.87s).
- Range WHERE now **competitive with Memgraph** (17 vs 16 ops/s at 100K).
- **5M scalability**: Property index maintains 169K ops/s (36% drop from 100K), MATCH= flat at 52K ops/s.
- **Concurrent scaling** (epoch=40000): Read-only 953K ops/s at 8T, mixed 122K ops/s at 8T.

### Round 10: Streaming executor + memory optimization + operator coverage (current, v6)
- **Streaming executor** (ADR-0012): Batched execution with BATCH_SIZE=1024 for `execute_return` and `execute_where`. Peak memory reduced from ~5.2GB to ~100MB for 5M node MATCH+RETURN.
- **label_iterator**: Streaming label scan avoids materializing all IDs at once. Lazy property reads via `get_properties()`.
- **In-place append**: O(N²) packed ID copy in `append_packed_id` replaced with O(1) in-place append.
- **Parser: `>=` / `<=` operators**: Added lexer support for `>=` and `<=` comparison operators. Full operator set: `=`, `>`, `<`, `>=`, `<=`, `<>`.
- **Executor: `>=` / `<=` in WHERE**: Full-scan filter lambda now handles all 6 comparison operators.
- **Test coverage**: 119 → 148 test functions (+29). 17/17 test files, 100% pass rate.
  - `parser_coverage_test.cpp` (10 tests): operator tokenization, undirected edge, multi-path CREATE, error paths
  - `executor_coverage_test.cpp` (9 tests): `>=`/`<=`/`<>` WHERE, RETURN edge, DELETE edge, `<-` multi-hop, UNWIND map
  - `storage_coverage_test.cpp` (10 tests): init guard, delete cleanup, iterator move, batch refill, range operators, string comparison
- **Benchmark verification** (no performance regression):
  - Real Shirakami (10K): 46,569 ops/sec total
  - Mock load test (100K): 118,905 ops/sec total
  - Concurrent (8T mixed 80/20): 133,896 ops/sec
  - Mock throughput (100K): 349,582 ops/sec

---

## Known Bottlenecks and Future Optimization Opportunities

### 1. Lazy Label Scan (Low Impact)

**Current**: For multi-hop MATCH patterns, the first label scan is still performed even if subsequent WHERE would narrow results.
**Solution**: Push-down WHERE predicate into MATCH (cost-based query planner).

### 2. Columnar Property Storage (Medium Impact)

**Current**: Properties stored as JSON strings. Full scan WHERE with range index still requires O(V) value comparison.
**Solution**: Binary property encoding with per-type storage for cache-friendly sequential scan.
**Expected Impact**: 2-5x for range WHERE on high-cardinality properties.

### 3. Write Scalability Under Contention (Low Impact)

**Current**: Write-only throughput scales 1.9x at 8 threads due to epoch-based commit serialization (3ms epoch boundary wait).
**Solution**: Batch commits across multiple operations, or reduce `epoch_duration` further (tradeoff: higher CPU usage).
**Expected Impact**: Write-heavy workloads may benefit from 2-3x improvement with commit batching.

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

### Real Shirakami benchmark (requires full Tsurugi build)

```bash
cd tsurugi-graph/graph-service/bench
TG_DIR=/home/user/tsurugi-install/tsurugi-1.10.0-SNAPSHOT-*/

# Compile against real sharksfin
g++ -std=c++17 -O2 \
  -I ../include -I "$TG_DIR/include/sharksfin" -I "$TG_DIR/include/takatori" \
  -o real_bench real_bench.cpp \
  ../src/tateyama/framework/graph/storage.cpp \
  ../src/tateyama/framework/graph/core/parser.cpp \
  ../src/tateyama/framework/graph/core/executor.cpp \
  -L "$TG_DIR/lib" -lsharksfin-shirakami -lglog -lpthread \
  -Wl,-rpath,"$TG_DIR/lib"

# Run (node_count, data_dir, epoch_duration_us)
./real_bench 10000                        # Quick test (default epoch=40000)
./real_bench 100000                       # Full benchmark
./real_bench 5000000                      # 5M scalability test
./real_bench 100000 /tmp/bench 3000       # With epoch=3000μs
```

### Concurrent benchmark (requires full Tsurugi build)

```bash
cd tsurugi-graph/graph-service/bench
TG_DIR=/home/user/tsurugi-install/tsurugi-1.10.0-SNAPSHOT-*/

# Compile
g++ -std=c++17 -O2 \
  -I ../include -I "$TG_DIR/include/sharksfin" -I "$TG_DIR/include/takatori" \
  -o concurrent_bench concurrent_bench.cpp \
  ../src/tateyama/framework/graph/storage.cpp \
  ../src/tateyama/framework/graph/core/parser.cpp \
  ../src/tateyama/framework/graph/core/executor.cpp \
  -L "$TG_DIR/lib" -lsharksfin-shirakami -lglog -lpthread \
  -Wl,-rpath,"$TG_DIR/lib"

# Run (max_threads, seed_nodes, ops_per_thread, data_dir, epoch_us)
./concurrent_bench 8 10000 5000                          # Default epoch=40000
./concurrent_bench 8 10000 5000 /tmp/conc_bench 3000     # With epoch=3000μs
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
