# ADR-0006: UNWIND Clause for Bulk Insert

- **Status**: Accepted
- **Date**: 2026-03-16
- **Deciders**: tsurugi-graph development team
- **Related**: ADR-0001 (Architecture), ADR-0002 (Property Index)

## Context

Comparative benchmarks (100K nodes) showed that Neo4j, Memgraph, and FalkorDB support `UNWIND` for bulk inserts, enabling 100+ nodes per round-trip. tsurugi-graph only supported individual `CREATE` statements, requiring one parse+execute cycle per node.

Additionally, `MATCH` with inline properties (e.g., `MATCH (n:Person {name: 'Alice'})`) performed full label scans without using the property index, making Cypher-level edge creation extremely slow (385 ops/s vs storage API 326K ops/s).

## Decision

### 1. Implement UNWIND clause

Syntax:
```cypher
UNWIND [{name: 'Alice', age: 30}, {name: 'Bob', age: 25}] AS item
CREATE (n:Person {name: item.name, age: item.age})
```

### 2. Optimize MATCH with inline properties

When a MATCH pattern includes inline properties and the label has a property index, use `find_nodes_by_property()` directly instead of scanning all label nodes.

## Implementation

### AST changes (`ast.h`)

New expression types:
- `list_literal_expr`: `[expr, expr, ...]` — vector of expressions
- `map_literal_expr`: `{key: expr, ...}` — key-value map expression

New clause type:
- `unwind_clause`: `list_expr` (the list) + `alias` (AS binding variable)

### Parser changes (`parser.h`, `parser.cpp`)

- Token `keyword_unwind` (6-char, case-insensitive)
- `parse_unwind()`: parses `UNWIND <list_expr> AS <alias>`
- `parse_expression()` extended for `[...]` (list) and `{k:v}` (map) literals

### Executor changes (`executor.h`, `executor.cpp`)

**UNWIND execution model:**
- `execute_unwind()` iterates list elements
- Per element: binds alias in `unwind_context_` → executes all subsequent clauses → clears node context
- `evaluate_properties()` resolves `property_access` expressions against `unwind_context_`

**MATCH inline property index:**
- `execute_match()` checks for inline properties with literal values
- If property index hit: uses `find_nodes_by_property()` (O(log N))
- Fallback: label scan + property filter (O(N))

## Performance Impact

### UNWIND bulk insert (100 nodes/batch, 10K scale)
| Method | Ops/sec |
|:---|---:|
| Individual CREATE | 151K |
| UNWIND (100/batch) | 173K |
| Speedup | 1.14x |

Single parse per batch eliminates 99% of parse overhead. The improvement is modest because tsurugi-graph's parse+execute is already fast in-process.

### MATCH inline property index
| Operation | Before | After | Speedup |
|:---|---:|---:|---:|
| Cypher CREATE edge (10K) | 385 ops/s | 73,704 ops/s | **191x** |

The bottleneck was 2 full label scans per edge creation. Property index reduces each to O(log N).

## Constraints

- List elements must be literal lists (no variable references)
- Map values limited to string and number literals
- No nested UNWIND
- No UNWIND from MATCH result variables
- UNWIND must precede subsequent clauses (consumes all remaining clauses)

## Future Work

- `UNWIND $parameter AS item` — parameter-driven batch operations
- `UNWIND range(0, N) AS i` — range function support
- Nested UNWIND for cross-product operations
