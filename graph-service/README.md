# graph-service

Tsurugi Graph Service - Cypher query engine on Shirakami KVS.

## Overview

graph-service provides a graph database layer on top of Tsurugi's Shirakami storage engine. It implements a subset of the Cypher query language with ACID transaction support, property indexing, and label-based navigation.

## Architecture

```
Client -> tateyama framework -> service.cpp (request routing, retry, query cache)
                                    |
                              core/parser.cpp (lexer + Cypher parser -> AST)
                                    |
                              core/executor.cpp (AST execution engine)
                                    |
                              storage.cpp (node/edge CRUD, indexes)
                                    |
                              Shirakami KVS (ACID transactions)
```

## Supported Cypher

| Clause | Example |
|:--|:--|
| CREATE | `CREATE (n:Person {name: 'Alice', age: 30})` |
| CREATE edge | `CREATE (a:Person {name: 'A'})-[r:KNOWS]->(b:Person {name: 'B'})` |
| MATCH | `MATCH (n:Person) RETURN n` |
| MATCH edge | `MATCH (a)-[r:KNOWS]->(b) RETURN a, b` |
| WHERE = | `MATCH (n:Person) WHERE n.name = 'Alice' RETURN n` |
| WHERE > < | `MATCH (n:Person) WHERE n.age > 30 RETURN n` |
| SET | `MATCH (n:Person) SET n.age = 31 RETURN n` |
| DELETE | `MATCH (n:Person) DELETE n` |
| DETACH DELETE | `MATCH (n:Person) DETACH DELETE n` |
| RETURN AS | `MATCH (n) RETURN n.name AS name` |
| UNWIND | `UNWIND ['a','b'] AS x CREATE (n:Item {name: x})` |

## Build

graph-service is built as part of the tsurugi-graph project:

```bash
cd tsurugi-graph
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/tsurugi/install -DBUILD_TESTING=ON
cmake --build .
```

Standalone test build (no server dependencies):

```bash
cd graph-service
g++ -std=c++17 -I include -I test \
  test/tateyama/framework/graph/executor_test.cpp \
  src/tateyama/framework/graph/storage.cpp \
  src/tateyama/framework/graph/core/parser.cpp \
  src/tateyama/framework/graph/core/executor.cpp \
  -o executor_test
```

## Tests

12 standalone test files with 110 test cases, all using the mock Shirakami backend (`test/sharksfin/api.h`).

### Run all tests

```bash
# Via CMake
cmake -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build

# Directly (example)
g++ -std=c++17 -I include -I test -I test/tateyama/framework/graph \
  test/tateyama/framework/graph/query_cache_test.cpp \
  src/tateyama/framework/graph/core/parser.cpp \
  -o query_cache_test && ./query_cache_test
```

### Test coverage

| Component | Test files | Cases | Coverage |
|:--|:--|---:|:--|
| Parser (lexer + AST) | parser_standalone, parser_full, parser_edge, parser_property | 30 | All clause types, edge patterns, literals, tokens |
| Storage | storage_standalone, storage_label, storage_navigation, storage_property | 22 | All 18 public APIs |
| Executor | executor, executor_advanced | 22 | All clause handlers, range WHERE, indexed MATCH, pipeline |
| Query Cache | query_cache | 35 | normalize, clone, deep_copy, bind, LRU, cache ops |
| Match Label | match_label | 1 | Label-based MATCH execution |

### Benchmarks

See `bench/` for performance benchmarks:
- `graph_bench.cpp` - Mock backend throughput
- `real_bench.cpp` - Shirakami backend (single-thread)
- `concurrent_bench.cpp` - Multi-threaded scaling

## ADRs

| ADR | Title |
|:--|:--|
| 0001 | Graph Engine Architecture |
| 0002 | Property Index |
| 0003 | Query Cache |
| 0004 | Batch Property Reads |
| 0005 | Parallel Query Execution |
| 0006 | UNWIND Bulk Insert |
| 0007 | Inverted Property Index |
| 0008 | Epoch Duration Tuning |
| 0009 | Optimistic Index Write |
| 0010 | Range Property Index |
| 0011 | Query Template Cache |

## Service ID

graph-service registers as tateyama service ID **13** (`service_id_graph`).
