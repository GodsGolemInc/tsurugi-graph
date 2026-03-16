# ADR-0001: Graph Engine Architecture

- **Status**: Accepted
- **Date**: 2026-03-16
- **Deciders**: tsurugi-graph development team

## Context

Tsurugi is a transactional database built on Shirakami (via the Sharksfin API). To support graph workloads (Cypher queries), a graph service layer was needed that integrates into the Tateyama framework lifecycle (setup/start/shutdown) and maps graph operations onto the existing key-value storage engine.

## Decision

We designed a layered graph engine consisting of five components:

### 1. Storage Layer (`storage.h/cpp`)

Maps graph primitives (nodes, edges, labels) onto Sharksfin key-value storages:

| Storage Name | Key Format | Value Format |
|---|---|---|
| `graph_nodes` | `node_id` (8-byte BE) | JSON properties string |
| `graph_edges` | `edge_id` (8-byte BE) | `from_id(8) + to_id(8) + label_size(4) + label + properties` |
| `graph_out_index` | `from_node_id(8) + edge_id(8)` | `to_node_id(8)` |
| `graph_in_index` | `to_node_id(8) + edge_id(8)` | `from_node_id(8)` |
| `graph_label_index` | `label_string + node_id(8)` | empty |

All IDs are generated from a single monotonic sequence (`graph_id_sequence`), encoded in big-endian for ordered prefix scans.

**API**: `create_node`, `get_node`, `update_node`, `delete_node`, `create_edge`, `get_edge`, `delete_edge`, `get_outgoing_edges`, `get_incoming_edges`, `find_nodes_by_label`.

### 2. Parser Layer (`core/parser.h/cpp`)

Two-phase Cypher parser:

- **Lexer**: Character-by-character tokenizer producing `token` objects. Supports identifiers, string/number literals, symbols (`(){}[]:,.-><`), and keywords (`CREATE`, `MATCH`, `RETURN`, `WHERE`, `DELETE`, `SET`, `AS`, `DETACH`). Case-insensitive keyword matching via `ci_eq()` with length-based dispatch.
- **Parser**: Recursive-descent parser consuming tokens into an AST (`statement` containing `clause` objects).

**AST types** (`core/ast.h`):
- Expressions: `variable`, `literal`, `property_access`, `binary_expression`
- Patterns: `pattern_node`, `pattern_relationship`, `pattern_element`, `pattern_path`
- Clauses: `create_clause`, `match_clause`, `return_clause`, `where_clause`, `delete_clause`, `set_clause`

### 3. Executor Layer (`core/executor.h/cpp`)

Interprets the AST against the storage layer:

- `execute_create()`: Creates nodes/edges, records IDs in context
- `execute_match()`: Label index scan + multi-hop edge traversal with label cache
- `execute_where()`: Filters context nodes by property comparison (=, >, <, <>)
- `execute_return()`: Builds JSON result array from context
- `execute_delete()`: Removes nodes/edges (with DETACH support)
- `execute_set()`: Updates node properties

**Context model**: `std::map<std::string, std::vector<uint64_t>>` maps variable names to lists of node/edge IDs.

### 4. Resource Layer (`resource.h/cpp`)

Tateyama `framework::resource` implementation:
- `setup()`: Allocates storage instance
- `start()`: Opens a bootstrap transaction, calls `storage::init()` to create/open all storages and the ID sequence, then commits
- `shutdown()`: Cleanup

### 5. Service Layer (`service.h/cpp`)

Tateyama `framework::service` implementation:
- Handles `CypherRequest`: lexer -> parser -> executor pipeline within a retryable transaction (exponential backoff for serialization failures)
- Handles `NodeGetRequest` / `EdgeGetRequest`: Direct storage lookups
- Transaction lifecycle: begin -> execute -> commit/abort -> dispose

## Consequences

- **Positive**: Clean separation of concerns; storage layer is independent of query language; parser/executor are independent of Tateyama framework.
- **Positive**: Big-endian key encoding enables efficient prefix scans for adjacency lists and label lookups.
- **Negative**: Properties stored as opaque JSON strings require per-access parsing; no secondary property indexes.
- **Negative**: WHERE clause requires O(N) full scan of label-matched nodes.
- **Negative**: Single-threaded execution limits throughput for scan-heavy queries.

## Performance Characteristics (measured)

| Operation | 100K nodes | 1M nodes | 10M nodes |
|---|---:|---:|---:|
| create_node | 408K ops/s | 402K ops/s | 308K ops/s |
| point read | 1.31M ops/s | 1.25M ops/s | 1.14M ops/s |
| MATCH+WHERE (per query) | 73ms | 824ms | 7,646ms |

The O(N) MATCH+WHERE bottleneck motivates ADR-0002 (Property Index).
