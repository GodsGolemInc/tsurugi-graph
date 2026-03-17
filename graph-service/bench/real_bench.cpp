#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <iomanip>
#include <cassert>
#include <functional>
#include <cstdlib>

#include <sharksfin/api.h>
#include <sharksfin/Environment.h>
#include <sharksfin/DatabaseOptions.h>

#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include <tateyama/framework/graph/storage.h>

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;
using namespace sharksfin;

struct bench_result {
    std::string name;
    long long ops;
    double elapsed_sec;
    double ops_per_sec;
};

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

// RAII guard for transaction lifecycle
class tx_guard {
public:
    tx_guard() = default;

    bool begin(DatabaseHandle db, const TransactionOptions& opt = {}) {
        auto rc = transaction_begin(db, opt, &ctl_);
        if (rc != StatusCode::OK) return false;
        rc = transaction_borrow_handle(ctl_, &tx_);
        if (rc != StatusCode::OK) {
            transaction_dispose(ctl_);
            ctl_ = nullptr;
            return false;
        }
        active_ = true;
        return true;
    }

    StatusCode commit() {
        if (!active_) return StatusCode::ERR_UNKNOWN;
        auto rc = transaction_commit(ctl_);
        active_ = false;
        return rc;
    }

    void abort() {
        if (active_) {
            transaction_abort(ctl_);
            active_ = false;
        }
    }

    TransactionHandle handle() const { return tx_; }

    ~tx_guard() {
        if (ctl_) {
            if (active_) transaction_abort(ctl_);
            transaction_dispose(ctl_);
        }
    }

    tx_guard(const tx_guard&) = delete;
    tx_guard& operator=(const tx_guard&) = delete;
    tx_guard(tx_guard&& o) noexcept : ctl_(o.ctl_), tx_(o.tx_), active_(o.active_) {
        o.ctl_ = nullptr; o.tx_ = nullptr; o.active_ = false;
    }
    tx_guard& operator=(tx_guard&& o) noexcept {
        if (this != &o) {
            if (ctl_) { if (active_) transaction_abort(ctl_); transaction_dispose(ctl_); }
            ctl_ = o.ctl_; tx_ = o.tx_; active_ = o.active_;
            o.ctl_ = nullptr; o.tx_ = nullptr; o.active_ = false;
        }
        return *this;
    }

private:
    TransactionControlHandle ctl_{};
    TransactionHandle tx_{};
    bool active_{false};
};

int main(int argc, char** argv) {
    long long N = 10000;
    if (argc > 1) N = std::stoll(argv[1]);

    std::string data_dir = "/tmp/tsurugi_bench_data";
    if (argc > 2) data_dir = argv[2];

    std::string epoch_dur = "40000";  // default 40ms (Shirakami default)
    if (argc > 3) epoch_dur = argv[3];

    // Clean up previous data
    std::string rm_cmd = "rm -rf " + data_dir;
    system(rm_cmd.c_str());

    std::cerr << "=== tsurugi-graph Real Shirakami Benchmark (" << N << " nodes) ===" << std::endl;
    std::cerr << "  Data directory: " << data_dir << std::endl;
    std::cerr << "  epoch_duration: " << epoch_dur << " us" << std::endl;

    // Initialize sharksfin environment
    Environment env{};
    env.initialize();

    // Open database
    DatabaseOptions db_opts;
    db_opts.attribute("location", data_dir);
    db_opts.attribute("epoch_duration", epoch_dur);  // configurable (ADR-0008), default 3ms

    DatabaseHandle db = nullptr;
    auto rc = database_open(db_opts, nullptr, &db);
    if (rc != StatusCode::OK) {
        std::cerr << "ERROR: Failed to open database: rc=" << static_cast<int>(rc) << std::endl;
        return 1;
    }
    std::cerr << "  Database opened successfully" << std::endl;

    // Initialize graph storage
    storage graph_storage;
    {
        tx_guard txg;
        if (!txg.begin(db)) {
            std::cerr << "ERROR: Failed to begin init transaction" << std::endl;
            database_close(db);
            database_dispose(db);
            return 1;
        }
        if (!graph_storage.init(db, txg.handle())) {
            std::cerr << "ERROR: Failed to initialize graph storage" << std::endl;
            database_close(db);
            database_dispose(db);
            return 1;
        }
        rc = txg.commit();
        if (rc != StatusCode::OK) {
            std::cerr << "ERROR: Failed to commit init transaction: rc=" << static_cast<int>(rc) << std::endl;
            database_close(db);
            database_dispose(db);
            return 1;
        }
    }
    std::cerr << "  Graph storage initialized" << std::endl;

    std::vector<bench_result> results;

    // ------- 1. Parser throughput (no DB needed) -------
    {
        int pn = std::min(N, (long long)100000);
        results.push_back(run_bench("Parser throughput", pn, [&]() {
            for (int i = 0; i < pn; ++i) {
                lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n.name AS name");
                parser p(l.tokenize());
                p.parse();
            }
        }));
    }

    // ------- 2. Storage: create_node -------
    {
        long long CHUNK = (N > 100000) ? 10000 : N;  // split large datasets to limit per-tx memory
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Storage: create_node", N, [&]() {
            for (long long i = 0; i < N; ++i) {
                uint64_t id;
                std::string label = (i % 2 == 0) ? "Person" : "Company";
                graph_storage.create_node(txg.handle(), label, "{\"name\": \"U" + std::to_string(i) + "\"}", id);
                if ((i + 1) % CHUNK == 0 && i + 1 < N) {
                    txg.commit();
                    txg = tx_guard();
                    txg.begin(db);
                }
            }
        }));
        txg.commit();
    }

    // ------- 3. Storage: point read (get_node) -------
    {
        long long rn = std::min(N, (long long)100000);
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Storage: point read (get_node)", rn, [&]() {
            for (long long i = 0; i < rn; ++i) {
                std::string props;
                graph_storage.get_node(txg.handle(), (i % N) + 1, props);
            }
        }));
        txg.commit();
    }

    // ------- 4. Storage: create_edge -------
    {
        long long edge_count = std::min(N, (long long)100000);
        long long CHUNK = (edge_count > 100000) ? 50000 : edge_count;
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Storage: create_edge", edge_count, [&]() {
            for (long long i = 0; i < edge_count; ++i) {
                uint64_t eid;
                graph_storage.create_edge(txg.handle(), (i % N) + 1, ((i + 1) % N) + 1, "LINK", "{}", eid);
                if ((i + 1) % CHUNK == 0 && i + 1 < edge_count) {
                    txg.commit();
                    txg = tx_guard();
                    txg.begin(db);
                }
            }
        }));
        txg.commit();
    }

    // ------- 5. Label scan -------
    {
        long long expected = N / 2;
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Label scan (" + std::to_string(expected) + " results)", 1, [&]() {
            std::vector<uint64_t> ids;
            graph_storage.find_nodes_by_label(txg.handle(), "Person", ids);
        }));
        txg.commit();
    }

    // ------- 6. Property index lookup -------
    {
        int pq = std::min((long long)10000, N);
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Property index lookup", pq, [&]() {
            for (int i = 0; i < pq; ++i) {
                std::vector<uint64_t> ids;
                graph_storage.find_nodes_by_property(txg.handle(), "Person", "name", "U" + std::to_string((i * 2) % N), ids);
            }
        }));
        txg.commit();
    }

    // ------- 7. Cypher: CREATE node (full pipeline) -------
    {
        long long cn = std::min(N, (long long)10000);
        long long CHUNK = (cn > 5000) ? 5000 : cn;
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Cypher: CREATE node", cn, [&]() {
            for (long long i = 0; i < cn; ++i) {
                lexer l("CREATE (n:Worker {name: 'W" + std::to_string(i) + "', age: " + std::to_string(20 + i % 60) + "})");
                parser p(l.tokenize());
                executor exec(graph_storage, txg.handle());
                std::string r;
                exec.execute(p.parse(), r);
                if ((i + 1) % CHUNK == 0 && i + 1 < cn) {
                    txg.commit();
                    txg = tx_guard();
                    txg.begin(db);
                }
            }
        }));
        txg.commit();
    }

    // ------- 8. Cypher: MATCH+WHERE = (indexed) -------
    {
        int Q = std::min(1000, (int)N);
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Cypher: MATCH+WHERE = (indexed, " + std::to_string(Q) + "q)", Q, [&]() {
            for (int i = 0; i < Q; ++i) {
                std::string q = "MATCH (n:Person) WHERE n.name = 'U" + std::to_string((i * 2) % N) + "' RETURN n";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(graph_storage, txg.handle());
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
        txg.commit();
    }

    // ------- 9. Cypher: MATCH+WHERE > (full scan) -------
    {
        int Q = std::min(10, (int)N);  // fewer queries for real backend
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Cypher: MATCH+WHERE > (full scan, " + std::to_string(Q) + "q)", Q, [&]() {
            for (int i = 0; i < Q; ++i) {
                std::string q = "MATCH (n:Person) WHERE n.age > " + std::to_string(50 + (i % 20)) + " RETURN n";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(graph_storage, txg.handle());
                std::string r;
                exec.execute(p.parse(), r);
            }
        }));
        txg.commit();
    }

    // ------- 10. Edge traversal -------
    {
        long long tn = std::min(N, (long long)10000);
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Edge traversal (outgoing+get_edge)", tn, [&]() {
            for (long long i = 0; i < tn; ++i) {
                uint64_t nid = (i % N) + 1;
                std::vector<uint64_t> edges;
                graph_storage.get_outgoing_edges(txg.handle(), nid, edges);
                for (uint64_t eid : edges) {
                    edge_data ed;
                    graph_storage.get_edge(txg.handle(), eid, ed);
                }
            }
        }));
        txg.commit();
    }

    // ------- 11. Pipeline: CREATE+MATCH+WHERE+SET+RETURN -------
    {
        int pn = std::min((long long)1000, N / 10);
        tx_guard txg;
        txg.begin(db);
        results.push_back(run_bench("Pipeline: CREATE+MATCH+WHERE+SET+RETURN", pn, [&]() {
            for (int i = 0; i < pn; ++i) {
                {
                    lexer l("CREATE (n:Engineer {name: 'E" + std::to_string(i) + "', salary: 50000})");
                    parser p(l.tokenize()); executor exec(graph_storage, txg.handle()); std::string r; exec.execute(p.parse(), r);
                }
                {
                    lexer l("MATCH (n:Engineer) WHERE n.name = 'E" + std::to_string(i) +
                            "' SET n.salary = " + std::to_string(50000 + i * 100) + " RETURN n.name AS name");
                    parser p(l.tokenize()); executor exec(graph_storage, txg.handle()); std::string r; exec.execute(p.parse(), r);
                }
            }
        }));
        txg.commit();
    }

    // ------- Print results -------
    print_results(results);

    double total_ops = 0, total_time = 0;
    for (const auto& r : results) { total_ops += r.ops; total_time += r.elapsed_sec; }
    std::cout << "\n**Total: " << static_cast<long long>(total_ops) << " ops in "
              << std::fixed << std::setprecision(2) << total_time << " sec ("
              << std::fixed << std::setprecision(0) << total_ops / total_time << " ops/sec)**\n";

    // Cleanup
    database_close(db);
    database_dispose(db);

    std::cerr << "\nDatabase closed. Cleaning up data directory..." << std::endl;
    system(rm_cmd.c_str());

    return 0;
}
