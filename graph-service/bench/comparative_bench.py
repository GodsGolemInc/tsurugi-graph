#!/usr/bin/env python3
"""
Unified Graph Database Benchmark
Runs identical workloads against Neo4j, Memgraph, FalkorDB, and tsurugi-graph.
All tests are single-threaded, sequential, same machine.
"""

import time
import sys
import os
import json
import subprocess
import signal
import argparse
import shutil
from contextlib import contextmanager

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools")

# --- Benchmark Infrastructure ---

class BenchResult:
    def __init__(self, name, ops, elapsed):
        self.name = name
        self.ops = ops
        self.elapsed = elapsed
        self.ops_per_sec = ops / elapsed if elapsed > 0 else 0

    def __repr__(self):
        return f"{self.name}: {self.ops} ops in {self.elapsed:.4f}s ({self.ops_per_sec:.0f} ops/s)"


def bench(name, ops, fn):
    """Run a benchmark function, return BenchResult."""
    start = time.perf_counter()
    fn()
    elapsed = time.perf_counter() - start
    r = BenchResult(name, ops, elapsed)
    print(f"  [done] {r}", flush=True)
    return r


# --- Database Launchers ---

@contextmanager
def launch_neo4j(data_dir):
    """Start Neo4j, yield connection info, stop on exit."""
    neo4j_home = os.path.join(TOOLS, "neo4j-community-5.26.2")

    # Write clean config (not append)
    conf = os.path.join(neo4j_home, "conf", "neo4j.conf")
    neo4j_data = os.path.join(data_dir, "neo4j-data")
    # Remove stale data
    if os.path.exists(neo4j_data):
        shutil.rmtree(neo4j_data)
    os.makedirs(neo4j_data, exist_ok=True)

    with open(conf, "w") as f:
        f.write("# Auto-generated for benchmark\n")
        f.write("server.default_listen_address=127.0.0.1\n")
        f.write("dbms.security.auth_enabled=false\n")
        f.write("server.bolt.listen_address=:7687\n")
        f.write(f"server.directories.data={neo4j_data}\n")
        f.write("server.memory.heap.initial_size=2g\n")
        f.write("server.memory.heap.max_size=2g\n")
        f.write("server.jvm.additional=-XX:+UseG1GC\n")

    proc = subprocess.Popen(
        [f"{neo4j_home}/bin/neo4j", "console"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**os.environ, "NEO4J_HOME": neo4j_home}
    )

    # Wait for bolt to be ready
    import neo4j as neo4j_mod
    driver = None
    for i in range(90):
        try:
            driver = neo4j_mod.GraphDatabase.driver("bolt://127.0.0.1:7687")
            driver.verify_connectivity()
            break
        except Exception:
            time.sleep(1)
    else:
        proc.kill()
        raise RuntimeError("Neo4j failed to start")

    try:
        yield {"type": "neo4j", "uri": "bolt://127.0.0.1:7687", "driver": driver}
    finally:
        if driver:
            driver.close()
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


@contextmanager
def launch_memgraph(data_dir):
    """Start Memgraph, yield connection info, stop on exit."""
    memgraph_bin = os.path.join(TOOLS, "memgraph", "usr", "lib", "memgraph", "memgraph")
    lib_dir = os.path.join(TOOLS, "lib")
    mg_data = os.path.join(data_dir, "mg-data")
    if os.path.exists(mg_data):
        shutil.rmtree(mg_data)
    os.makedirs(mg_data, exist_ok=True)

    proc = subprocess.Popen(
        [memgraph_bin,
         "--data-directory", mg_data,
         "--bolt-port", "7688",
         "--log-level", "WARNING",
         "--storage-mode", "IN_MEMORY_TRANSACTIONAL",
         "--also-log-to-stderr", "false"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**os.environ, "LD_LIBRARY_PATH": lib_dir}
    )

    import neo4j as neo4j_mod
    for i in range(30):
        try:
            driver = neo4j_mod.GraphDatabase.driver("bolt://127.0.0.1:7688")
            driver.verify_connectivity()
            break
        except Exception:
            time.sleep(1)
    else:
        proc.kill()
        raise RuntimeError("Memgraph failed to start")

    try:
        yield {"type": "memgraph", "uri": "bolt://127.0.0.1:7688", "driver": driver}
    finally:
        driver.close()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


@contextmanager
def launch_falkordb(data_dir):
    """Start Redis with FalkorDB module, yield connection info, stop on exit."""
    redis_bin = os.path.join(TOOLS, "redis-stable", "src", "redis-server")
    module_path = os.path.join(TOOLS, "falkordb.so")

    proc = subprocess.Popen(
        [redis_bin,
         "--port", "6380",
         "--loadmodule", module_path,
         "--bind", "127.0.0.1",
         "--save", "",
         "--dir", data_dir],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    from falkordb import FalkorDB as FDB
    for i in range(15):
        try:
            fdb = FDB(host="127.0.0.1", port=6380)
            fdb.list_graphs()
            break
        except Exception:
            time.sleep(1)
    else:
        proc.kill()
        raise RuntimeError("FalkorDB failed to start")

    try:
        yield {"type": "falkordb", "host": "127.0.0.1", "port": 6380, "fdb": fdb}
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


# --- Benchmark Workloads ---

def run_neo4j_bench(conn, N):
    """Run benchmarks against Neo4j or Memgraph (both use Bolt protocol)."""
    driver = conn["driver"]
    db_name = conn["type"]
    results = []

    print(f"\n=== {db_name} Benchmark ({N} nodes) ===", flush=True)

    # Cleanup
    with driver.session() as s:
        s.run("MATCH (n) DETACH DELETE n").consume()

    # 1. CREATE nodes
    def create_nodes():
        with driver.session() as s:
            for i in range(N):
                label = "Person" if i % 2 == 0 else "Company"
                s.run(
                    f"CREATE (n:{label} {{name: $name, age: $age}})",
                    name=f"U{i}", age=20 + i % 60
                ).consume()
    results.append(bench(f"{db_name}: CREATE node", N, create_nodes))

    # 1b. UNWIND bulk insert (batch of 100 nodes per query)
    # First cleanup the individual creates
    with driver.session() as s:
        s.run("MATCH (n) DETACH DELETE n").consume()

    batch_size = 100
    unwind_batches = N // batch_size
    def unwind_bulk():
        with driver.session() as s:
            for b in range(unwind_batches):
                rows = [{"name": f"U{b*batch_size+j}", "age": 20 + (b*batch_size+j) % 60}
                        for j in range(batch_size)]
                label = "Person"
                s.run(
                    f"UNWIND $rows AS row CREATE (n:{label} {{name: row.name, age: row.age}})",
                    rows=rows
                ).consume()
    results.append(bench(f"{db_name}: UNWIND bulk insert ({batch_size}/batch)", N, unwind_bulk))

    # Re-create with proper Person/Company labels for subsequent tests
    with driver.session() as s:
        s.run("MATCH (n) DETACH DELETE n").consume()
        for i in range(N):
            label = "Person" if i % 2 == 0 else "Company"
            s.run(f"CREATE (n:{label} {{name: $name, age: $age}})",
                  name=f"U{i}", age=20 + i % 60).consume()

    # 2. Create index for WHERE benchmarks
    with driver.session() as s:
        try:
            if db_name == "memgraph":
                s.run("CREATE INDEX ON :Person(name)").consume()
            else:
                s.run("CREATE INDEX FOR (n:Person) ON (n.name)").consume()
            time.sleep(2)  # Wait for index to be built
        except Exception as e:
            print(f"  Index creation: {e}")

    # 3. Point read (get node by indexed property)
    read_n = min(N, 10000)
    def point_reads():
        with driver.session() as s:
            for i in range(read_n):
                s.run(
                    "MATCH (n:Person {name: $name}) RETURN n",
                    name=f"U{(i * 2) % N}"
                ).consume()
    results.append(bench(f"{db_name}: MATCH indexed = (point)", read_n, point_reads))

    # 4. MATCH+WHERE inequality (full scan)
    scan_q = min(100, N // 100)
    def full_scan():
        with driver.session() as s:
            for i in range(scan_q):
                s.run(
                    "MATCH (n:Person) WHERE n.age > $age RETURN count(n)",
                    age=50 + (i % 20)
                ).consume()
    results.append(bench(f"{db_name}: MATCH WHERE > (scan, {scan_q}q)", scan_q, full_scan))

    # 5. Create edges (capped at 10K — MATCH+CREATE per query is inherently slow over Bolt)
    edge_n = min(N, 10000)
    def create_edges():
        with driver.session() as s:
            for i in range(edge_n):
                s.run(
                    "MATCH (a:Person {name: $from}), (b:Company {name: $to}) "
                    "CREATE (a)-[:WORKS_AT]->(b)",
                    **{"from": f"U{(i * 2) % N}", "to": f"U{(i * 2 + 1) % N}"}
                ).consume()
    results.append(bench(f"{db_name}: CREATE edge (MATCH+CREATE)", edge_n, create_edges))

    # 6. Traversal
    trav_n = min(1000, N // 10)
    def traversal():
        with driver.session() as s:
            for i in range(trav_n):
                s.run(
                    "MATCH (a:Person {name: $name})-[r:WORKS_AT]->(b) RETURN b",
                    name=f"U{(i * 2) % N}"
                ).consume()
    results.append(bench(f"{db_name}: 1-hop traversal", trav_n, traversal))

    return results


def run_falkordb_bench(conn, N):
    """Run benchmarks against FalkorDB."""
    from falkordb import FalkorDB as FDB
    fdb = conn["fdb"]
    results = []
    db_name = "FalkorDB"

    print(f"\n=== {db_name} Benchmark ({N} nodes) ===", flush=True)

    # Create/get graph
    g = fdb.select_graph("bench")
    try:
        g.query("MATCH (n) DETACH DELETE n")
    except Exception:
        pass

    # 1. CREATE nodes
    def create_nodes():
        for i in range(N):
            label = "Person" if i % 2 == 0 else "Company"
            g.query(f"CREATE (:{label} {{name: 'U{i}', age: {20 + i % 60}}})")
    results.append(bench(f"{db_name}: CREATE node", N, create_nodes))

    # 1b. UNWIND bulk insert
    try:
        g.query("MATCH (n) DETACH DELETE n")
    except Exception:
        pass

    batch_size = 100
    unwind_batches = N // batch_size
    def unwind_bulk():
        for b in range(unwind_batches):
            items = ", ".join(
                f"{{name: 'U{b*batch_size+j}', age: {20 + (b*batch_size+j) % 60}}}"
                for j in range(batch_size)
            )
            g.query(f"UNWIND [{items}] AS row CREATE (n:Person {{name: row.name, age: row.age}})")
    results.append(bench(f"{db_name}: UNWIND bulk insert ({batch_size}/batch)", N, unwind_bulk))

    # Re-create with proper Person/Company labels
    try:
        g.query("MATCH (n) DETACH DELETE n")
    except Exception:
        pass
    for i in range(N):
        label = "Person" if i % 2 == 0 else "Company"
        g.query(f"CREATE (:{label} {{name: 'U{i}', age: {20 + i % 60}}})")

    # 2. Create index
    try:
        g.query("CREATE INDEX FOR (n:Person) ON (n.name)")
        time.sleep(1)
    except Exception as e:
        print(f"  Index: {e}")

    # 3. Point read
    read_n = min(N, 10000)
    def point_reads():
        for i in range(read_n):
            g.query(f"MATCH (n:Person {{name: 'U{(i * 2) % N}'}}) RETURN n")
    results.append(bench(f"{db_name}: MATCH indexed = (point)", read_n, point_reads))

    # 4. Full scan
    scan_q = min(100, N // 100)
    def full_scan():
        for i in range(scan_q):
            g.query(f"MATCH (n:Person) WHERE n.age > {50 + (i % 20)} RETURN count(n)")
    results.append(bench(f"{db_name}: MATCH WHERE > (scan, {scan_q}q)", scan_q, full_scan))

    # 5. Create edges (capped at 10K)
    edge_n = min(N, 10000)
    def create_edges():
        for i in range(edge_n):
            g.query(
                f"MATCH (a:Person {{name: 'U{(i * 2) % N}'}}), "
                f"(b:Company {{name: 'U{(i * 2 + 1) % N}'}}) "
                f"CREATE (a)-[:WORKS_AT]->(b)"
            )
    results.append(bench(f"{db_name}: CREATE edge (MATCH+CREATE)", edge_n, create_edges))

    # 6. Traversal
    trav_n = min(1000, N // 10)
    def traversal():
        for i in range(trav_n):
            g.query(f"MATCH (a:Person {{name: 'U{(i * 2) % N}'}})-[r:WORKS_AT]->(b) RETURN b")
    results.append(bench(f"{db_name}: 1-hop traversal", trav_n, traversal))

    return results


def run_tsurugi_bench(N):
    """Run tsurugi-graph benchmark (compiled C++ binary)."""
    bench_dir = os.path.dirname(os.path.abspath(__file__))
    graph_dir = os.path.dirname(bench_dir)
    binary = os.path.join(bench_dir, "load_test_bench")

    # Always recompile to pick up latest changes
    print("  Compiling tsurugi-graph benchmark...", flush=True)
    rc = subprocess.run(
        ["g++", "-std=c++17", "-O2",
         "-I", os.path.join(graph_dir, "include"),
         "-I", os.path.join(graph_dir, "test"),
         "-o", binary,
         os.path.join(bench_dir, "load_test.cpp"),
         os.path.join(graph_dir, "src/tateyama/framework/graph/core/parser.cpp"),
         os.path.join(graph_dir, "src/tateyama/framework/graph/core/executor.cpp"),
         os.path.join(graph_dir, "src/tateyama/framework/graph/storage.cpp"),
         "-lpthread"],
        capture_output=True, text=True
    )
    if rc.returncode != 0:
        print(f"  Compile failed: {rc.stderr}")
        return []

    print(f"\n=== tsurugi-graph Benchmark ({N} nodes) ===", flush=True)
    result = subprocess.run(
        [binary, str(N)],
        capture_output=True, text=True, timeout=1800
    )
    print(result.stderr, end="", flush=True)

    # Parse results from stderr
    results = []
    for line in result.stderr.strip().split("\n"):
        if "[done]" in line:
            # Parse: "  [done] Name: 1.23s (456 ops/s)"
            parts = line.split("[done] ")[1]
            name_part, rest = parts.rsplit(": ", 1)
            time_str = rest.split("s (")[0]
            ops_str = rest.split("(")[1].rstrip(" ops/s)")
            elapsed = float(time_str)
            ops_per_sec = float(ops_str)
            ops = int(ops_per_sec * elapsed)
            results.append(BenchResult(f"tsurugi: {name_part}", ops, elapsed))

    return results


# --- Main ---

def print_comparison(all_results):
    """Print comparison table."""
    print("\n" + "=" * 80)
    print("COMPARATIVE RESULTS")
    print("=" * 80)

    print(f"\n{'Operation':<45} {'Ops':>8} {'Time(s)':>10} {'Ops/sec':>12}")
    print("-" * 80)
    for r in all_results:
        print(f"{r.name:<45} {r.ops:>8} {r.elapsed:>10.4f} {r.ops_per_sec:>12.0f}")


def main():
    parser = argparse.ArgumentParser(description="Comparative Graph DB Benchmark")
    parser.add_argument("nodes", type=int, nargs="?", default=100000, help="Number of nodes")
    parser.add_argument("--skip-neo4j", action="store_true")
    parser.add_argument("--skip-memgraph", action="store_true")
    parser.add_argument("--skip-falkordb", action="store_true")
    parser.add_argument("--skip-tsurugi", action="store_true")
    args = parser.parse_args()

    N = args.nodes
    data_dir = os.path.join(TOOLS, "bench_data")
    os.makedirs(data_dir, exist_ok=True)

    all_results = []

    # tsurugi-graph (always first, fastest)
    if not args.skip_tsurugi:
        try:
            all_results.extend(run_tsurugi_bench(N))
        except Exception as e:
            print(f"tsurugi-graph failed: {e}")

    # Neo4j
    if not args.skip_neo4j:
        try:
            with launch_neo4j(data_dir) as conn:
                all_results.extend(run_neo4j_bench(conn, N))
        except Exception as e:
            print(f"Neo4j failed: {e}")

    # Memgraph
    if not args.skip_memgraph:
        try:
            with launch_memgraph(data_dir) as conn:
                all_results.extend(run_neo4j_bench(conn, N))
        except Exception as e:
            print(f"Memgraph failed: {e}")

    # FalkorDB
    if not args.skip_falkordb:
        try:
            with launch_falkordb(data_dir) as conn:
                all_results.extend(run_falkordb_bench(conn, N))
        except Exception as e:
            print(f"FalkorDB failed: {e}")

    print_comparison(all_results)

    # Output JSON for later processing
    json_out = os.path.join(data_dir, f"results_{N}.json")
    with open(json_out, "w") as f:
        json.dump([{"name": r.name, "ops": r.ops, "elapsed": r.elapsed, "ops_per_sec": r.ops_per_sec} for r in all_results], f, indent=2)
    print(f"\nResults saved to {json_out}")


if __name__ == "__main__":
    main()
