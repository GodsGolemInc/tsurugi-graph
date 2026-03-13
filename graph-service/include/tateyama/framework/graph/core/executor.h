#pragma once

#include <tateyama/framework/graph/core/ast.h>
#include <tateyama/framework/graph/storage.h>
#include <sharksfin/api.h>
#include <map>
#include <string>

namespace tateyama::framework::graph::core {

class executor {
public:
    executor(storage& store, sharksfin::TransactionHandle tx) 
        : store_(store), tx_(tx) {}

    bool execute(const statement& stmt, std::string& result_json);

private:
    storage& store_;
    sharksfin::TransactionHandle tx_;
    
    // Symbol table: variable_name -> node_id or edge_id
    std::map<std::string, uint64_t> context_;
    
    bool execute_create(const std::shared_ptr<create_clause>& create);
    bool execute_match(const std::shared_ptr<match_clause>& match);
    bool execute_return(const std::shared_ptr<return_clause>& ret, std::string& result_json);

    std::string evaluate_properties(const std::map<std::string, std::shared_ptr<expression>>& props);
};

} // namespace tateyama::framework::graph::core
