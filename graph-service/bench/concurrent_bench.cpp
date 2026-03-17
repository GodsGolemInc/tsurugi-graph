#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <iomanip>
#include <cassert>
#include <functional>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <mutex>
#include <numeric>

#include <sharksfin/api.h>
#include <sharksfin/Environment.h>
#include <sharksfin/DatabaseOptions.h>

#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include <tateyama/framework/graph/storage.h>

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;
using namespace sharksfin;

using hrclock = std::chrono::high_resolution_clock;

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

struct thread_result {
    int thread_id;
    long long ops;
    double elapsed_sec;
    long long retries;
};

// Worker: write workload (CREATE nodes via Cypher)
void write_worker(DatabaseHandle db, storage& store, int tid, long long ops,
                  std::atomic<long long>& total_retries, thread_result& result) {
    auto start = hrclock::now();
    long long retries = 0;
    long long done = 0;
    long long CHUNK = 500;

    while (done < ops) {
        tx_guard txg;
        if (!txg.begin(db)) {
            ++retries;
            continue;
        }
        long long batch_end = std::min(done + CHUNK, ops);
        bool ok = true;
        for (long long i = done; i < batch_end; ++i) {
            std::string q = "CREATE (n:ConcWorker {name: 'T" + std::to_string(tid) +
                            "_W" + std::to_string(i) + "', tid: " + std::to_string(tid) + "})";
            lexer l(q);
            parser p(l.tokenize());
            executor exec(store, txg.handle());
            std::string r;
            if (!exec.execute(p.parse(), r)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            txg.abort();
            ++retries;
            continue;
        }
        auto rc = txg.commit();
        if (rc != StatusCode::OK) {
            ++retries;
            continue;
        }
        done = batch_end;
    }

    auto end = hrclock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    total_retries += retries;
    result = {tid, ops, elapsed, retries};
}

// Worker: read workload (MATCH + WHERE via Cypher)
void read_worker(DatabaseHandle db, storage& store, int tid, long long ops,
                 long long max_node_id, std::atomic<long long>& total_retries,
                 thread_result& result) {
    auto start = hrclock::now();
    long long retries = 0;
    long long done = 0;
    long long CHUNK = 1000;

    while (done < ops) {
        tx_guard txg;
        if (!txg.begin(db)) {
            ++retries;
            continue;
        }
        long long batch_end = std::min(done + CHUNK, ops);
        bool ok = true;
        for (long long i = done; i < batch_end; ++i) {
            // Point read by node ID
            std::string props;
            uint64_t nid = (i % max_node_id) + 1;
            store.get_node(txg.handle(), nid, props);
        }
        auto rc = txg.commit();
        if (rc != StatusCode::OK) {
            ++retries;
            continue;
        }
        done = batch_end;
    }

    auto end = hrclock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    total_retries += retries;
    result = {tid, ops, elapsed, retries};
}

// Worker: mixed read-write (80% read, 20% write)
void mixed_worker(DatabaseHandle db, storage& store, int tid, long long ops,
                  long long max_node_id, std::atomic<long long>& total_retries,
                  thread_result& result) {
    auto start = hrclock::now();
    long long retries = 0;
    long long done = 0;
    long long CHUNK = 200;

    while (done < ops) {
        tx_guard txg;
        if (!txg.begin(db)) {
            ++retries;
            continue;
        }
        long long batch_end = std::min(done + CHUNK, ops);
        bool ok = true;
        for (long long i = done; i < batch_end; ++i) {
            if (i % 5 == 0) {
                // 20% writes
                std::string q = "CREATE (n:MixWorker {name: 'M" + std::to_string(tid) +
                                "_" + std::to_string(i) + "', tid: " + std::to_string(tid) + "})";
                lexer l(q);
                parser p(l.tokenize());
                executor exec(store, txg.handle());
                std::string r;
                if (!exec.execute(p.parse(), r)) {
                    ok = false;
                    break;
                }
            } else {
                // 80% reads
                std::string props;
                uint64_t nid = (i % max_node_id) + 1;
                store.get_node(txg.handle(), nid, props);
            }
        }
        if (!ok) {
            txg.abort();
            ++retries;
            continue;
        }
        auto rc = txg.commit();
        if (rc != StatusCode::OK) {
            ++retries;
            continue;
        }
        done = batch_end;
    }

    auto end = hrclock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    total_retries += retries;
    result = {tid, ops, elapsed, retries};
}

void run_concurrent_bench(const std::string& name, DatabaseHandle db, storage& store,
                          int num_threads, long long ops_per_thread,
                          std::function<void(DatabaseHandle, storage&, int, long long,
                                            std::atomic<long long>&, thread_result&)> worker_fn) {
    std::vector<std::thread> threads;
    std::vector<thread_result> results(num_threads);
    std::atomic<long long> total_retries{0};

    auto wall_start = hrclock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(worker_fn, db, std::ref(store), t, ops_per_thread,
                             std::ref(total_retries), std::ref(results[t]));
    }
    for (auto& th : threads) th.join();
    auto wall_end = hrclock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();

    long long total_ops = 0;
    for (const auto& r : results) total_ops += r.ops;

    double agg_ops_sec = total_ops / wall_sec;
    double avg_thread_sec = 0;
    for (const auto& r : results) avg_thread_sec += r.elapsed_sec;
    avg_thread_sec /= num_threads;

    std::cout << "\n### " << name << " (" << num_threads << " threads)\n";
    std::cout << "| Metric | Value |\n|:---|---:|\n";
    std::cout << "| Threads | " << num_threads << " |\n";
    std::cout << "| Ops/thread | " << ops_per_thread << " |\n";
    std::cout << "| Total ops | " << total_ops << " |\n";
    std::cout << "| Wall time | " << std::fixed << std::setprecision(3) << wall_sec << " sec |\n";
    std::cout << "| Throughput | " << std::fixed << std::setprecision(0) << agg_ops_sec << " ops/sec |\n";
    std::cout << "| Avg thread time | " << std::fixed << std::setprecision(3) << avg_thread_sec << " sec |\n";
    std::cout << "| Total retries | " << total_retries.load() << " |\n";

    std::cerr << "  [done] " << name << ": " << std::fixed << std::setprecision(2) << wall_sec
              << "s, " << std::setprecision(0) << agg_ops_sec << " ops/s, "
              << total_retries.load() << " retries" << std::endl;
}

int main(int argc, char** argv) {
    int max_threads = 8;
    if (argc > 1) max_threads = std::stoi(argv[1]);

    long long seed_nodes = 10000;
    if (argc > 2) seed_nodes = std::stoll(argv[2]);

    long long ops_per_thread = 5000;
    if (argc > 3) ops_per_thread = std::stoll(argv[3]);

    std::string data_dir = "/tmp/tsurugi_conc_bench";
    if (argc > 4) data_dir = argv[4];

    std::string epoch_dur = "40000";  // default 40ms (Shirakami default)
    if (argc > 5) epoch_dur = argv[5];

    std::string rm_cmd = "rm -rf " + data_dir;
    system(rm_cmd.c_str());

    std::cerr << "=== tsurugi-graph Concurrent Benchmark ===" << std::endl;
    std::cerr << "  Max threads: " << max_threads << std::endl;
    std::cerr << "  Seed nodes: " << seed_nodes << std::endl;
    std::cerr << "  Ops/thread: " << ops_per_thread << std::endl;
    std::cerr << "  epoch_duration: " << epoch_dur << " us" << std::endl;

    // Initialize
    Environment env{};
    env.initialize();

    DatabaseOptions db_opts;
    db_opts.attribute("location", data_dir);
    db_opts.attribute("epoch_duration", epoch_dur);

    DatabaseHandle db = nullptr;
    auto rc = database_open(db_opts, nullptr, &db);
    if (rc != StatusCode::OK) {
        std::cerr << "ERROR: Failed to open database" << std::endl;
        return 1;
    }

    storage graph_storage;
    {
        tx_guard txg;
        txg.begin(db);
        graph_storage.init(db, txg.handle());
        txg.commit();
    }

    // Seed data
    std::cerr << "  Seeding " << seed_nodes << " nodes..." << std::endl;
    {
        long long CHUNK = 5000;
        tx_guard txg;
        txg.begin(db);
        for (long long i = 0; i < seed_nodes; ++i) {
            uint64_t id;
            std::string label = (i % 2 == 0) ? "Person" : "Company";
            graph_storage.create_node(txg.handle(), label,
                "{\"name\": \"S" + std::to_string(i) + "\"}", id);
            if ((i + 1) % CHUNK == 0 && i + 1 < seed_nodes) {
                txg.commit();
                txg = tx_guard();
                txg.begin(db);
            }
        }
        txg.commit();
    }
    std::cerr << "  Seed complete." << std::endl;

    std::cout << "# Concurrent Benchmark Results\n\n";
    std::cout << "- Seed nodes: " << seed_nodes << "\n";
    std::cout << "- Ops per thread: " << ops_per_thread << "\n";
    std::cout << "- epoch_duration: " << epoch_dur << "μs\n";

    // Test with different thread counts: 1, 2, 4, 8, ...
    std::vector<int> thread_counts;
    for (int t = 1; t <= max_threads; t *= 2) {
        thread_counts.push_back(t);
    }
    if (thread_counts.back() != max_threads) {
        thread_counts.push_back(max_threads);
    }

    // 1. Read-only benchmark
    std::cout << "\n## Read-Only (point reads)\n";
    for (int nt : thread_counts) {
        run_concurrent_bench("Read-only", db, graph_storage, nt, ops_per_thread,
            [seed_nodes](DatabaseHandle db, storage& store, int tid, long long ops,
                         std::atomic<long long>& retries, thread_result& result) {
                read_worker(db, store, tid, ops, seed_nodes, retries, result);
            });
    }

    // 2. Write-only benchmark
    std::cout << "\n## Write-Only (CREATE via Cypher)\n";
    for (int nt : thread_counts) {
        run_concurrent_bench("Write-only", db, graph_storage, nt, ops_per_thread,
            [](DatabaseHandle db, storage& store, int tid, long long ops,
               std::atomic<long long>& retries, thread_result& result) {
                write_worker(db, store, tid, ops, retries, result);
            });
    }

    // 3. Mixed benchmark (80% read, 20% write)
    std::cout << "\n## Mixed (80% read, 20% write)\n";
    for (int nt : thread_counts) {
        run_concurrent_bench("Mixed 80/20", db, graph_storage, nt, ops_per_thread,
            [seed_nodes](DatabaseHandle db, storage& store, int tid, long long ops,
                         std::atomic<long long>& retries, thread_result& result) {
                mixed_worker(db, store, tid, ops, seed_nodes, retries, result);
            });
    }

    // Cleanup
    database_close(db);
    database_dispose(db);
    system(rm_cmd.c_str());

    std::cerr << "\nConcurrent benchmark complete." << std::endl;
    return 0;
}
