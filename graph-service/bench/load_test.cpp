#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <iomanip>
#include <cassert>
#include <functional>

#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include <tateyama/framework/graph/storage.h>
#include "../test/tateyama/framework/graph/sharksfin_mock.h"

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;

struct bench_result {
    std::string name;
    long long ops;
    double elapsed_sec;
    double ops_per_sec;
};

static void reset_mock() {
    sharksfin::mock_db_state.clear();
    sharksfin::mock_sequences.clear();
    sharksfin::mock_iterators.clear();
    sharksfin::mock_iterators_end.clear();
    sharksfin::mock_iterators_started.clear();
    sharksfin::mock_commit_failure_count = 0;
}

using hrclock = std::chrono::high_resolution_clock;

template<typename Fn>
bench_result run_bench(const std::string& name, long long ops, Fn fn) {
    auto start = hrclock::now();
    fn();
    auto end = hrclock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    std::cerr << "  [done] " << name << ": " << std::fixed << std::setprecision(2) << elapsed << "s ("
              << std::setprecision(0) << ops / elapsed << " ops/s)" << std::endl;
    return {name, ops, elapsed, ops / elapsed};
}

void print_results(const std::vector<bench_result>& results) {
    std::cout << "\n| Benchmark | Ops | Time (sec) | Ops/sec |\n";
    std::cout << "|:---|---:|---:|---:|\n";
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << r.ops
                  << " | " << std::fixed << std::setprecision(4) << r.elapsed_sec
                  << " | " << std::fixed << std::setprecision(0) << r.ops_per_sec
                  << " |\n";
    }
}

int main(int argc, char** argv) {
    long long N = 100000;
    if (argc > 1) N = std::stoll(argv[1]);

    std::cerr << "=== tsurugi-graph Load Test (" << N << " nodes) ===" << std::endl;
    std::vector<bench_result> results;

    // ------- 1. Parser throughput -------
    {
        int pn = std::min(N, (long long)1000000);
        results.push_back(run_bench("Parser throughput", pn, [&]() {
            for (int i = 0; i < pn; ++i) {
                lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n.name AS name");
                parser p(l.tokenize());
                p.parse();
            }
        }));
    }

    // ------- 2. Raw storage: create_node -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        results.push_back(run_bench("Storage: create_node", N, [&]() {
            for (long long i = 0; i < N; ++i) {
                uint64_t id;
                std::string label = (i % 2 == 0) ? "Person" : "Company";
                s.create_node(tx, label, "{\"name\": \"U" + std::to_string(i) + "\"}", id);
            }
        }));
    }

    // ------- 3. Raw storage: create_edge -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        int node_count = std::min(N, (long long)100000);
        for (int i = 0; i < node_count; ++i) {
            uint64_t id;
            s.create_node(tx, "", "{}", id);
        }
        long long edge_count = std::min(N, (long long)1000000);
        results.push_back(run_bench("Storage: create_edge", edge_count, [&]() {
            for (long long i = 0; i < edge_count; ++i) {
                uint64_t eid;
                s.create_edge(tx, (i % node_count) + 1, ((i + 1) % node_count) + 1, "LINK", "{}", eid);
            }
        }));
    }

    // ------- 4. Cypher CREATE (full pipeline) -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        long long cn = std::min(N, (long long)1000000);
        results.push_back(run_bench("Cypher: CREATE node", cn, [&]() {
            for (long long i = 0; i < cn; ++i) {
                lexer l("CREATE (n:Person {name: 'U" + std::to_string(i) + "', age: " + std::to_string(20 + i % 60) + "})");
                parser p(l.tokenize());
                executor exec(s, tx);
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
    }

    // ------- 4b. Cypher UNWIND bulk insert -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        int batch_size = 100;
        long long batches = std::min(N / batch_size, (long long)10000);
        long long total_nodes = batches * batch_size;
        results.push_back(run_bench("Cypher: UNWIND bulk insert (100/batch)", total_nodes, [&]() {
            for (long long b = 0; b < batches; ++b) {
                std::string list = "[";
                for (int j = 0; j < batch_size; ++j) {
                    if (j > 0) list += ", ";
                    long long idx = b * batch_size + j;
                    list += "{name: 'U" + std::to_string(idx) + "', age: " + std::to_string(20 + idx % 60) + "}";
                }
                list += "]";
                std::string q = "UNWIND " + list + " AS item CREATE (n:Person {name: item.name, age: item.age})";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(s, tx);
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
    }

    // ------- 4c. Cypher CREATE edge (MATCH+CREATE) -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        int node_count = std::min(N, (long long)100000);
        // Pre-populate nodes with Person (even) and Company (odd)
        for (int i = 0; i < node_count; ++i) {
            uint64_t id;
            std::string label = (i % 2 == 0) ? "Person" : "Company";
            s.create_node(tx, label, "{\"name\": \"U" + std::to_string(i) + "\"}", id);
        }
        long long edge_n = std::min(N, (long long)10000);
        results.push_back(run_bench("Cypher: CREATE edge (MATCH+CREATE)", edge_n, [&]() {
            for (long long i = 0; i < edge_n; ++i) {
                std::string q = "MATCH (a:Person {name: 'U" + std::to_string((i * 2) % node_count) +
                                "'}), (b:Company {name: 'U" + std::to_string((i * 2 + 1) % node_count) +
                                "'}) CREATE (a)-[:WORKS_AT]->(b)";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(s, tx);
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
    }

    // ------- 5. Label index scan -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        for (long long i = 0; i < N; ++i) {
            uint64_t id;
            std::string label = (i % 2 == 0) ? "Person" : "Company";
            s.create_node(tx, label, "{}", id);
        }
        long long expected = N / 2;
        results.push_back(run_bench("Label scan (" + std::to_string(expected) + " results)", 1, [&]() {
            std::vector<uint64_t> ids;
            s.find_nodes_by_label(tx, "Person", ids);
            assert(static_cast<long long>(ids.size()) == expected);
        }));
    }

    // ------- 6. Point read: get_node -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        for (long long i = 0; i < N; ++i) {
            uint64_t id;
            s.create_node(tx, "X", "{\"v\": " + std::to_string(i) + "}", id);
        }
        long long rn = std::min(N, (long long)1000000);
        results.push_back(run_bench("Storage: point read (get_node)", rn, [&]() {
            for (long long i = 0; i < rn; ++i) {
                std::string props;
                s.get_node(tx, (i % N) + 1, props);
            }
        }));
    }

    // ------- 7. MATCH+WHERE with property index -------
    // Fixed query count regardless of N to keep timing reasonable
    // Shared storage for tests 7, 7b, 7c
    reset_mock();
    storage s_where;
    void* db_w = (void*)0x1; void* tx_w = (void*)0x2;
    s_where.init(db_w, tx_w);
    // Create N nodes, 50% Person
    for (long long i = 0; i < N; ++i) {
        uint64_t id;
        std::string label = (i % 2 == 0) ? "Person" : "Company";
        s_where.create_node(tx_w, label, "{\"name\": \"U" + std::to_string(i) + "\", \"age\": " + std::to_string(20 + i % 60) + "}", id);
    }

    {
        int Q = 100;
        long long person_count = N / 2;
        results.push_back(run_bench("Cypher: MATCH+WHERE = (indexed, " + std::to_string(Q) + " queries)", Q, [&]() {
            for (int i = 0; i < Q; ++i) {
                std::string q = "MATCH (n:Person) WHERE n.name = 'U" + std::to_string((i * 2) % N) + "' RETURN n";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(s_where, tx_w);
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
    }

    // ------- 7b. Property index: find_nodes_by_property (direct) -------
    {
        int pq = std::min((long long)10000, N);
        results.push_back(run_bench("Property index lookup (direct)", pq, [&]() {
            for (int i = 0; i < pq; ++i) {
                std::vector<uint64_t> ids;
                s_where.find_nodes_by_property(tx_w, "Person", "name", "U" + std::to_string((i * 2) % N), ids);
            }
        }));
    }

    // ------- 7c. MATCH+WHERE inequality (full scan, no index) -------
    {
        int Q = 100;
        long long person_count = N / 2;
        results.push_back(run_bench("Cypher: MATCH+WHERE > (full scan, " + std::to_string(Q) + " queries)", Q, [&]() {
            for (int i = 0; i < Q; ++i) {
                std::string q = "MATCH (n:Person) WHERE n.age > " + std::to_string(50 + (i % 20)) + " RETURN n";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(s_where, tx_w);
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
    }

    // ------- 8. Edge traversal -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        int trav_nodes = std::min(N, (long long)100000);
        int edges_per = 5;
        for (int i = 0; i < trav_nodes; ++i) {
            uint64_t id;
            s.create_node(tx, "N", "{}", id);
        }
        for (int i = 0; i < trav_nodes; ++i) {
            for (int j = 0; j < edges_per; ++j) {
                uint64_t eid;
                s.create_edge(tx, i + 1, ((i + j + 1) % trav_nodes) + 1, "L", "{}", eid);
            }
        }
        long long tn = std::min(N, (long long)100000);
        results.push_back(run_bench("Edge traversal (outgoing+get_edge)", tn, [&]() {
            for (long long i = 0; i < tn; ++i) {
                uint64_t nid = (i % trav_nodes) + 1;
                std::vector<uint64_t> edges;
                s.get_outgoing_edges(tx, nid, edges);
                for (uint64_t eid : edges) {
                    edge_data ed;
                    s.get_edge(tx, eid, ed);
                }
            }
        }));
    }

    // ------- 9. Complex pipeline: CREATE+MATCH+WHERE+SET+RETURN -------
    {
        reset_mock();
        storage s;
        void* db = (void*)0x1; void* tx = (void*)0x2;
        s.init(db, tx);
        int pn = std::min(N / 10, (long long)10000);
        results.push_back(run_bench("Pipeline: CREATE+MATCH+WHERE+SET+RETURN", pn, [&]() {
            for (int i = 0; i < pn; ++i) {
                {
                    lexer l("CREATE (n:Worker {name: 'W" + std::to_string(i) + "', salary: 50000})");
                    parser p(l.tokenize()); executor exec(s, tx); std::string r; exec.execute(p.parse(), r);
                }
                {
                    lexer l("MATCH (n:Worker) WHERE n.name = 'W" + std::to_string(i) +
                            "' SET n.salary = " + std::to_string(50000 + i * 100) + " RETURN n.name AS name");
                    parser p(l.tokenize()); executor exec(s, tx); std::string r; exec.execute(p.parse(), r);
                }
            }
        }));
    }

    // ------- Print results -------
    print_results(results);

    double total_ops = 0, total_time = 0;
    for (const auto& r : results) { total_ops += r.ops; total_time += r.elapsed_sec; }
    std::cout << "\n**Total: " << static_cast<long long>(total_ops) << " ops in "
              << std::fixed << std::setprecision(2) << total_time << " sec ("
              << std::fixed << std::setprecision(0) << total_ops / total_time << " ops/sec)**\n";

    return 0;
}
