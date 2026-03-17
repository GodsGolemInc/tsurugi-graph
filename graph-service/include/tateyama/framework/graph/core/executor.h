#pragma once

#include <tateyama/framework/graph/core/ast.h>
#include <tateyama/framework/graph/storage.h>
#include <sharksfin/api.h>
#include <map>
#include <string>
#include <vector>

namespace tateyama::framework::graph::core {

class executor {
public:
    executor(storage& store, sharksfin::TransactionHandle tx)
        : store_(store), tx_(tx) {}

    bool execute(const statement& stmt, std::string& result_json);

private:
    storage& store_;
    sharksfin::TransactionHandle tx_;

    // Symbol table: variable_name -> list of node/edge IDs
    std::map<std::string, std::vector<uint64_t>> context_;
    // Label metadata: variable_name -> label (for property index optimization)
    std::map<std::string, std::string> context_labels_;

    // UNWIND context: alias -> current binding
    struct unwind_binding {
        std::map<std::string, std::string> properties;
        std::map<std::string, bool> is_string;
    };
    std::map<std::string, unwind_binding> unwind_context_;

    static constexpr size_t PARALLEL_THRESHOLD = 10000;
    static constexpr size_t BATCH_SIZE = 1024;

    bool execute_create(const std::shared_ptr<create_clause>& create);
    bool execute_match(const std::shared_ptr<match_clause>& match);
    bool execute_where(const std::shared_ptr<where_clause>& where);
    bool execute_return(const std::shared_ptr<return_clause>& ret, std::string& result_json);
    bool execute_delete(const std::shared_ptr<delete_clause>& del);
    bool execute_set(const std::shared_ptr<set_clause>& set);
    bool execute_unwind(const std::shared_ptr<unwind_clause>& unwind,
                        const statement& stmt, size_t& ci, std::string& result_json);

    // Fused MATCH+WHERE using property index (skips label scan)
    bool try_indexed_match_where(const std::shared_ptr<match_clause>& match, const std::shared_ptr<where_clause>& where);

    std::string evaluate_properties(const std::map<std::string, std::shared_ptr<expression>>& props);

    // JSON property helpers
    static std::string get_json_value(const std::string& json, const std::string& key);
    static std::string update_json_property(const std::string& json, const std::string& key, const std::string& value, bool is_string);
};

} // namespace tateyama::framework::graph::core
