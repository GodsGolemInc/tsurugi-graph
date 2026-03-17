# tsurugi-graph

English | [日本語](README.md)

A graph database engine for TsurugiDB. Executes a Cypher query subset with ACID transactions on top of the Shirakami KVS.

## Features

- **Cypher subset** — CREATE / MATCH / WHERE / SET / DELETE / DETACH DELETE / RETURN / UNWIND
- **Directed edges** — Create and traverse `(a)-[r:KNOWS]->(b)` patterns
- **Property index** — Inverted index with O(1) equality and O(V) range lookups
- **Label index** — Efficient label-based node retrieval
- **Query template cache** — LRU cache with AST deep-copy and literal binding
- **ACID transactions** — Retry logic on Shirakami OCC (up to 5 retries, exponential backoff)
- **Batch operations** — UNWIND bulk insert, batched property reads
- **Streaming execution** — Chunked processing (BATCH_SIZE=1024) supporting 10M+ nodes

## Architecture

```
Client (Tateyama protocol)
    │
    ▼
service.cpp ── Request routing, retry logic, query cache
    │
    ▼
core/parser.cpp ── Cypher lexer + recursive-descent parser → AST
    │
    ▼
core/executor.cpp ── AST execution engine (symbol table management)
    │
    ▼
storage.cpp ── Node/edge CRUD, 6 KVS indices
    │
    ▼
Shirakami KVS ── Masstree B+tree, MVCC, WAL
```

### Storage Schema (6 KVS Indices)

| Storage | Key | Value | Purpose |
|:--|:--|:--|:--|
| `graph_nodes` | node_id (8B BE) | JSON properties | Node data |
| `graph_edges` | edge_id (8B BE) | from+to+label+props | Edge data |
| `graph_label_index` | label + node_id | (empty) | Label → node mapping |
| `graph_out_index` | from_id + edge_id | to_id | Outgoing edges |
| `graph_in_index` | to_id + edge_id | from_id | Incoming edges |
| `graph_prop_index` | label\0key\0value | packed node IDs | Inverted property index |

## Supported Cypher

| Clause | Example |
|:--|:--|
| CREATE (node) | `CREATE (n:Person {name: 'Alice', age: 30})` |
| CREATE (edge) | `CREATE (a:Person)-[r:KNOWS]->(b:Person)` |
| MATCH | `MATCH (n:Person) RETURN n` |
| MATCH (edge) | `MATCH (a)-[r:KNOWS]->(b) RETURN a, b` |
| WHERE (=, >, <, <>) | `MATCH (n) WHERE n.age > 30 RETURN n` |
| SET | `MATCH (n) SET n.age = 31 RETURN n` |
| DELETE | `MATCH (n:Person) DELETE n` |
| DETACH DELETE | `MATCH (n:Person) DETACH DELETE n` |
| RETURN AS | `MATCH (n) RETURN n.name AS name` |
| UNWIND | `UNWIND ['a','b'] AS x CREATE (n:Item {name: x})` |

## Build

### Prerequisites

- CMake 3.20+
- C++17 compiler (GCC 11+ / Clang 14+)
- Protobuf, glog
- TsurugiDB (Shirakami, tateyama) — for full server build

### Full Build (Server Integration)

```bash
# From tsurugidb root
./install.sh --prefix=$HOME/tsurugi
```

Build order: takatori → yugawara → shirakami → sharksfin → jogasaki → **tsurugi-graph** → tateyama-bootstrap

### Standalone Build (Testing)

```bash
cd graph-service
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/tsurugi/install -DBUILD_TESTING=ON
cmake --build .
ctest
```

### Direct Test Compilation (No Server Dependencies)

```bash
cd graph-service
g++ -std=c++17 -I include -I test -I test/tateyama/framework/graph \
  test/tateyama/framework/graph/executor_test.cpp \
  src/tateyama/framework/graph/storage.cpp \
  src/tateyama/framework/graph/core/parser.cpp \
  src/tateyama/framework/graph/core/executor.cpp \
  -o executor_test -lpthread && ./executor_test
```

## Server Startup

```bash
export PATH=$HOME/tsurugi/bin:$PATH
tgctl start     # graph-service auto-loads as service ID 13
tgctl status    # Check status
tgctl shutdown  # Stop
```

Clients connect via the Tateyama protocol (IPC/TCP), sending Cypher queries to service ID **13**.

## Tests

14 test files with 119 test cases, all using the mock Shirakami backend.

| Component | Test Files | Cases |
|:--|:--|---:|
| Parser | parser_standalone, parser_full, parser_edge, parser_property | 30 |
| Storage | storage_standalone, storage_label, storage_navigation, storage_property, storage_iterator | 26 |
| Executor | executor, executor_advanced, executor_batch | 27 |
| Query Cache | query_cache | 35 |
| Match Label | match_label | 1 |

## Performance

### Real Shirakami Backend (100K Nodes)

| Operation | Throughput |
|:--|---:|
| create_node | 47,761 ops/s |
| Property index lookup | 265,460 ops/s |
| MATCH+WHERE = (indexed) | 46,060 ops/s |
| Edge traversal | 120,171 ops/s |
| Full pipeline | 10,719 ops/s |
| **8-thread read** | **953K ops/s** |

### Comparison with Other Graph Databases (100K Nodes)

| Operation | tsurugi-graph | Neo4j | Memgraph | FalkorDB |
|:--|---:|---:|---:|---:|
| CREATE | 47.8K | 1.9K | 7.9K | 768 |
| Equality lookup | 265K | 6.3K | 4.3K | 6.0K |
| Edge traversal | 120K | 2.8K | 889 | 3.3K |

See [BENCHMARK.md](BENCHMARK.md) for full results.

## ADRs (Architecture Decision Records)

| # | Title | Key Impact |
|:--|:--|:--|
| 0001 | Graph Engine Architecture | 5-layer design, 6 KVS indices |
| 0002 | Secondary Property Index | O(N)→O(log N) (superseded by 0007) |
| 0003 | Query Cache | Eliminates 4us parse overhead |
| 0004 | Batch Property Reads | 2-3x improvement for large result sets |
| 0005 | Parallel Query Execution | Multi-threaded WHERE |
| 0006 | UNWIND Bulk Insert | 25% faster than individual CREATEs |
| 0007 | Inverted Property Index | O(1) equality lookup, 57x speedup |
| 0008 | Epoch Duration Tuning | Default 40ms optimal |
| 0009 | Optimistic Index Write | Single KVS call for unique values |
| 0010 | Range Property Index | Inequality WHERE in O(V) |
| 0011 | Query Template Cache | Literal normalization for higher hit rate |
| 0012 | Streaming Executor | Peak memory 5GB → 100MB |

## Directory Structure

```
tsurugi-graph/
├── README.md              # Japanese
├── README_en.md           # This file (English)
├── BENCHMARK.md           # Performance report
├── graph-service/
│   ├── CMakeLists.txt
│   ├── README.md          # graph-service details
│   ├── proto/             # Protobuf definitions (request/response)
│   ├── include/           # Public headers
│   │   └── tateyama/framework/graph/
│   │       ├── service.h, resource.h, storage.h
│   │       └── core/ (ast.h, parser.h, executor.h, query_cache.h)
│   ├── src/               # Implementation (~2,400 lines)
│   │   └── tateyama/framework/graph/
│   │       ├── service.cpp, resource.cpp, storage.cpp
│   │       └── core/ (parser.cpp, executor.cpp)
│   ├── test/              # Tests (14 files, 119 cases)
│   ├── bench/             # Benchmarks
│   └── docs/adr/          # ADRs (0001-0012)
└── build/
```

## License

Apache License 2.0 (following the TsurugiDB project)
