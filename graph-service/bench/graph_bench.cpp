#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/executor.h>
#include <tateyama/framework/graph/storage.h>
#include "sharksfin_mock.h" // We can use mock for logic bench or real if linked

using namespace tateyama::framework::graph;
using namespace tateyama::framework::graph::core;

void run_benchmark(int num_nodes, int num_edges_per_node) {
    storage s;
    void* db = (void*)0x1;
    void* tx = (void*)0x2;
    s.init(db, tx);
    
    executor exec(s, tx);
    std::string result;

    std::cout << "Starting benchmark: " << num_nodes << " nodes, " << num_edges_per_node << " edges/node" << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    // 1. Create Nodes
    for (int i = 0; i < num_nodes; ++i) {
        std::string query = "CREATE (n:Person {id: " + std::to_string(i) + ", name: 'User" + std::to_string(i) + "'})";
        lexer l(query);
        parser p(l.tokenize());
        exec.execute(p.parse(), result);
    }

    // 2. Create Edges (Randomly connecting)
    for (int i = 0; i < num_nodes; ++i) {
        for (int j = 0; j < num_edges_per_node; ++j) {
            int target = (i + j + 1) % num_nodes;
            // Note: In real Cypher, we'd use MATCH then CREATE edge. 
            // For simple bench, we assume we have IDs or use direct storage calls.
            // Using executor with simple CREATE edge syntax (requires vars to be bound, or IDs)
            // Let's use direct storage call for raw performance measurement of the backend.
            uint64_t edge_id;
            s.create_edge(tx, i+1, target+1, "KNOWS", "{}", edge_id);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Benchmark finished in " << diff.count() << " seconds" << std::endl;
    std::cout << "Throughput: " << (num_nodes + num_nodes * num_edges_per_node) / diff.count() << " ops/sec" << std::endl;
}

int main(int argc, char** argv) {
    int nodes = 1000;
    if (argc > 1) nodes = std::stoi(argv[1]);
    run_benchmark(nodes, 10);
    return 0;
}
