#include <tateyama/framework/graph/core/executor.h>
#include <algorithm>
#include <cstring>
#include <set>
#include <thread>
#include <unordered_map>

namespace tateyama::framework::graph::core {

bool executor::execute(const statement& stmt, std::string& result_json) {
    for (size_t ci = 0; ci < stmt.clauses.size(); ++ci) {
        const auto& clause = stmt.clauses[ci];
        switch (clause->type()) {
            case clause_type::create:
                if (!execute_create(std::dynamic_pointer_cast<create_clause>(clause))) return false;
                break;
            case clause_type::match: {
                auto match = std::dynamic_pointer_cast<match_clause>(clause);
                // Lookahead: fuse MATCH+WHERE when property index can be used
                if (ci + 1 < stmt.clauses.size() && stmt.clauses[ci + 1]->type() == clause_type::where) {
                    auto where = std::dynamic_pointer_cast<where_clause>(stmt.clauses[ci + 1]);
                    if (try_indexed_match_where(match, where)) {
                        ++ci; // Skip WHERE clause (already handled)
                        break;
                    }
                }
                if (!execute_match(match)) return false;
                break;
            }
            case clause_type::where:
                if (!execute_where(std::dynamic_pointer_cast<where_clause>(clause))) return false;
                break;
            case clause_type::return_clause:
                if (!execute_return(std::dynamic_pointer_cast<return_clause>(clause), result_json)) return false;
                break;
            case clause_type::delete_clause:
                if (!execute_delete(std::dynamic_pointer_cast<delete_clause>(clause))) return false;
                break;
            case clause_type::set_clause:
                if (!execute_set(std::dynamic_pointer_cast<set_clause>(clause))) return false;
                break;
            case clause_type::unwind: {
                auto unwind = std::dynamic_pointer_cast<unwind_clause>(clause);
                if (!execute_unwind(unwind, stmt, ci, result_json)) return false;
                break;
            }
        }
    }
    return true;
}

bool executor::try_indexed_match_where(const std::shared_ptr<match_clause>& match, const std::shared_ptr<where_clause>& where) {
    // Only optimize simple single-node MATCH patterns
    if (match->paths.size() != 1 || match->paths[0].elements.size() != 1) return false;

    auto& node = match->paths[0].elements[0].node;
    if (node->label.empty() || node->variable.empty()) return false;

    // Only optimize equality WHERE on this variable
    if (where->variable != node->variable || where->op != "=") return false;
    if (where->property.empty()) return false;

    // Extract comparison value
    std::string compare_value;
    if (where->value) {
        if (auto lit = std::dynamic_pointer_cast<literal>(where->value)) {
            compare_value = lit->value;
        } else {
            return false;
        }
    } else {
        return false;
    }

    // Use property index directly (skips label scan entirely)
    std::vector<uint64_t> results;
    if (!store_.find_nodes_by_property(tx_, node->label, where->property, compare_value, results)) {
        return false; // Index lookup failed, fall back to normal path
    }

    context_[node->variable] = std::move(results);
    context_labels_[node->variable] = node->label;
    return true; // Successfully handled both MATCH and WHERE
}

std::string executor::evaluate_properties(const std::map<std::string, std::shared_ptr<expression>>& props) {
    std::string result;
    result.reserve(props.size() * 32); // rough estimate
    result += '{';
    bool first = true;
    for (const auto& [key, expr] : props) {
        if (!first) result += ", ";
        first = false;
        result += '"';
        result += key;
        result += "\": ";
        if (expr->type() == node_type::literal_string) {
            auto lit = std::dynamic_pointer_cast<literal>(expr);
            result += '"';
            result += lit->value;
            result += '"';
        } else if (expr->type() == node_type::literal_number) {
            auto lit = std::dynamic_pointer_cast<literal>(expr);
            result += lit->value;
        } else if (expr->type() == node_type::property_access) {
            auto pa = std::dynamic_pointer_cast<property_access>(expr);
            auto uit = unwind_context_.find(pa->variable_name);
            if (uit != unwind_context_.end()) {
                auto pit = uit->second.properties.find(pa->property_key);
                if (pit != uit->second.properties.end()) {
                    auto sit = uit->second.is_string.find(pa->property_key);
                    bool is_str = (sit != uit->second.is_string.end() && sit->second);
                    if (is_str) {
                        result += '"';
                        result += pit->second;
                        result += '"';
                    } else {
                        result += pit->second;
                    }
                }
            }
        }
    }
    result += '}';
    return result;
}

bool executor::execute_create(const std::shared_ptr<create_clause>& create) {
    for (const auto& path : create->paths) {
        for (size_t i = 0; i < path.elements.size(); ++i) {
            auto& elem = path.elements[i];

            uint64_t node_id = 0;
            if (!elem.node->variable.empty() && context_.count(elem.node->variable) && !context_[elem.node->variable].empty()) {
                node_id = context_[elem.node->variable][0];
            } else {
                std::string props = evaluate_properties(elem.node->properties);
                if (!store_.create_node(tx_, elem.node->label, props, node_id)) return false;
                if (!elem.node->variable.empty()) {
                    context_[elem.node->variable] = {node_id};
                }
            }

            if (elem.relationship && i + 1 < path.elements.size()) {
                auto& next_elem = path.elements[i + 1];
                uint64_t next_node_id = 0;

                if (!next_elem.node->variable.empty() && context_.count(next_elem.node->variable) && !context_[next_elem.node->variable].empty()) {
                    next_node_id = context_[next_elem.node->variable][0];
                } else {
                    std::string props = evaluate_properties(next_elem.node->properties);
                    if (!store_.create_node(tx_, next_elem.node->label, props, next_node_id)) return false;
                    if (!next_elem.node->variable.empty()) {
                        context_[next_elem.node->variable] = {next_node_id};
                    }
                }

                uint64_t edge_id;
                std::string rel_props = evaluate_properties(elem.relationship->properties);
                uint64_t from = node_id;
                uint64_t to = next_node_id;
                if (elem.relationship->direction == "<-") std::swap(from, to);

                if (!store_.create_edge(tx_, from, to, elem.relationship->type, rel_props, edge_id)) return false;
                if (!elem.relationship->variable.empty()) {
                    context_[elem.relationship->variable] = {edge_id};
                }
            }
        }
    }
    return true;
}

bool executor::execute_match(const std::shared_ptr<match_clause>& match) {
    for (const auto& path : match->paths) {
        if (path.elements.empty()) continue;

        auto& first_node = path.elements[0].node;

        if (!first_node->label.empty()) {
            bool used_index = false;

            // Optimization: use property index intersection for inline property match
            // e.g., MATCH (n:Person {name: 'Alice', age: 30}) → intersect index results
            if (!first_node->properties.empty()) {
                bool first_prop = true;
                std::vector<uint64_t> intersection;
                bool all_indexed = true;

                for (auto& [key, expr] : first_node->properties) {
                    auto lit = std::dynamic_pointer_cast<literal>(expr);
                    if (!lit) { all_indexed = false; break; }

                    // For numeric literals, convert to string (index stores string representation)
                    std::string lookup_value = lit->value;

                    std::vector<uint64_t> index_ids;
                    if (!store_.find_nodes_by_property(tx_, first_node->label, key, lookup_value, index_ids)) {
                        all_indexed = false;
                        break;
                    }

                    if (first_prop) {
                        intersection = std::move(index_ids);
                        std::sort(intersection.begin(), intersection.end());
                        first_prop = false;
                    } else {
                        std::sort(index_ids.begin(), index_ids.end());
                        std::vector<uint64_t> tmp;
                        tmp.reserve(std::min(intersection.size(), index_ids.size()));
                        std::set_intersection(intersection.begin(), intersection.end(),
                                              index_ids.begin(), index_ids.end(),
                                              std::back_inserter(tmp));
                        intersection = std::move(tmp);
                    }
                }

                if (all_indexed && !first_prop) {
                    if (!first_node->variable.empty()) {
                        context_[first_node->variable] = std::move(intersection);
                        context_labels_[first_node->variable] = first_node->label;
                    }
                    used_index = true;
                }
            }

            if (!used_index) {
                // ADR-0012: Use streaming iterator to avoid materializing all IDs
                // in the KVS layer simultaneously
                auto iter = store_.find_nodes_by_label_iter(tx_, first_node->label);
                std::vector<uint64_t> ids;
                if (!first_node->properties.empty()) {
                    // Filter by inline properties during iteration
                    while (iter.next()) {
                        std::string props;
                        if (!iter.get_properties(props)) continue;
                        bool all_match = true;
                        for (auto& [key, expr] : first_node->properties) {
                            if (auto lit = std::dynamic_pointer_cast<literal>(expr)) {
                                std::string val = get_json_value(props, key);
                                if (val != lit->value) { all_match = false; break; }
                            }
                        }
                        if (all_match) ids.push_back(iter.node_id());
                    }
                } else {
                    while (iter.next()) {
                        ids.push_back(iter.node_id());
                    }
                }
                if (!first_node->variable.empty() && !ids.empty()) {
                    context_[first_node->variable] = std::move(ids);
                    context_labels_[first_node->variable] = first_node->label;
                }
            }
        }

        // Multi-hop match
        if (path.elements.size() > 1) {
            // Cache label lookups to avoid repeated index scans
            std::map<std::string, std::set<uint64_t>> label_cache;

            for (size_t i = 0; i < path.elements.size() - 1; ++i) {
                auto& elem = path.elements[i];
                if (!elem.relationship) continue;

                auto& prev_var = elem.node->variable;
                if (prev_var.empty() || !context_.count(prev_var)) continue;

                auto& next_node = path.elements[i + 1].node;

                // Pre-cache label set if needed
                if (!next_node->label.empty() && label_cache.find(next_node->label) == label_cache.end()) {
                    std::vector<uint64_t> label_ids;
                    store_.find_nodes_by_label(tx_, next_node->label, label_ids);
                    label_cache[next_node->label] = std::set<uint64_t>(label_ids.begin(), label_ids.end());
                }

                std::vector<uint64_t> next_ids;
                for (uint64_t src_id : context_[prev_var]) {
                    std::vector<uint64_t> edge_ids;
                    bool is_outgoing = (elem.relationship->direction != "<-");

                    if (is_outgoing) {
                        store_.get_outgoing_edges(tx_, src_id, edge_ids);
                    } else {
                        store_.get_incoming_edges(tx_, src_id, edge_ids);
                    }

                    for (uint64_t eid : edge_ids) {
                        edge_data ed;
                        if (!store_.get_edge(tx_, eid, ed)) continue;

                        if (!elem.relationship->type.empty() && ed.label != elem.relationship->type) continue;

                        uint64_t target_id = is_outgoing ? ed.to_id : ed.from_id;

                        if (!next_node->label.empty()) {
                            auto& label_set = label_cache[next_node->label];
                            if (label_set.find(target_id) == label_set.end()) continue;
                        }

                        next_ids.push_back(target_id);

                        if (!elem.relationship->variable.empty()) {
                            context_[elem.relationship->variable].push_back(eid);
                        }
                    }
                }

                if (!next_node->variable.empty()) {
                    context_[next_node->variable] = std::move(next_ids);
                    if (!next_node->label.empty()) {
                        context_labels_[next_node->variable] = next_node->label;
                    }
                }
            }
        }
    }
    return true;
}

bool executor::execute_where(const std::shared_ptr<where_clause>& where) {
    if (where->variable.empty() || where->property.empty()) return true;

    auto it = context_.find(where->variable);
    if (it == context_.end()) return true;

    std::string compare_value;
    bool is_numeric = false;
    double compare_numeric = 0;
    if (where->value) {
        if (auto lit = std::dynamic_pointer_cast<literal>(where->value)) {
            compare_value = lit->value;
            if (!lit->is_string) {
                is_numeric = true;
                try { compare_numeric = std::stod(compare_value); } catch (...) { is_numeric = false; }
            }
        }
    }

    const std::string& prop_key = where->property;
    const std::string& op = where->op;

    // ADR-0002: Use property index for equality lookups
    if (op == "=") {
        auto label_it = context_labels_.find(where->variable);
        if (label_it != context_labels_.end() && !label_it->second.empty()) {
            std::vector<uint64_t> index_results;
            if (store_.find_nodes_by_property(tx_, label_it->second, prop_key, compare_value, index_results)) {
                // Intersect index results with current context
                std::set<uint64_t> index_set(index_results.begin(), index_results.end());
                std::vector<uint64_t> filtered;
                filtered.reserve(std::min(it->second.size(), index_results.size()));
                for (uint64_t id : it->second) {
                    if (index_set.count(id)) {
                        filtered.push_back(id);
                    }
                }
                it->second = std::move(filtered);
                return true;
            }
            // Fall through to full scan if index lookup fails
        }
    }

    // ADR-0010: Use range property index for inequality operators
    if (op != "=" && op != "<>") {
        auto label_it = context_labels_.find(where->variable);
        if (label_it != context_labels_.end() && !label_it->second.empty()) {
            std::vector<uint64_t> range_results;
            if (store_.find_nodes_by_property_range(tx_, label_it->second, prop_key, op, compare_value, range_results)) {
                std::set<uint64_t> range_set(range_results.begin(), range_results.end());
                std::vector<uint64_t> filtered;
                filtered.reserve(std::min(it->second.size(), range_results.size()));
                for (uint64_t id : it->second) {
                    if (range_set.count(id)) filtered.push_back(id);
                }
                it->second = std::move(filtered);
                return true;
            }
        }
    }

    // Full scan path (for <> operator or when no label/index available)
    // ADR-0012: Process in BATCH_SIZE chunks to cap peak memory
    auto& ids = it->second;
    std::vector<uint64_t> filtered;

    // Filter lambda — operates on a batch-local props_map
    auto filter_node = [&](uint64_t id, const std::unordered_map<uint64_t, std::string*>& props_map) -> bool {
        auto pit = props_map.find(id);
        if (pit == props_map.end()) return false;
        std::string val = get_json_value(*pit->second, prop_key);
        if (val.empty()) return false;

        if (op == "=") return val == compare_value;
        if (is_numeric) {
            double node_val;
            try { node_val = std::stod(val); } catch (...) { return false; }
            if (op == ">") return node_val > compare_numeric;
            if (op == "<") return node_val < compare_numeric;
            if (op == "<>") return node_val != compare_numeric;
        } else {
            if (op == ">") return val > compare_value;
            if (op == "<") return val < compare_value;
            if (op == "<>") return val != compare_value;
        }
        return false;
    };

    for (size_t batch_off = 0; batch_off < ids.size(); batch_off += BATCH_SIZE) {
        size_t batch_end = std::min(batch_off + BATCH_SIZE, ids.size());

        // Load properties for this chunk only
        std::vector<uint64_t> chunk_ids(ids.begin() + batch_off, ids.begin() + batch_end);
        std::vector<std::pair<uint64_t, std::string>> batch_results;
        store_.get_nodes_batch(tx_, chunk_ids, batch_results);

        std::unordered_map<uint64_t, std::string*> props_map;
        props_map.reserve(batch_results.size());
        for (auto& [id, props] : batch_results) {
            props_map[id] = &props;
        }

        // ADR-0005: Parallel execution for large chunks
        if (chunk_ids.size() >= PARALLEL_THRESHOLD) {
            size_t num_threads = std::min(
                static_cast<size_t>(std::thread::hardware_concurrency()),
                chunk_ids.size() / 1000);
            num_threads = std::max(num_threads, static_cast<size_t>(2));

            std::vector<std::vector<uint64_t>> per_thread_results(num_threads);
            std::vector<std::thread> threads;
            size_t chunk_sz = (chunk_ids.size() + num_threads - 1) / num_threads;

            for (size_t t = 0; t < num_threads; ++t) {
                size_t begin = t * chunk_sz;
                size_t end = std::min(begin + chunk_sz, chunk_ids.size());
                if (begin >= chunk_ids.size()) break;

                threads.emplace_back([&, begin, end, t]() {
                    auto& local = per_thread_results[t];
                    for (size_t i = begin; i < end; ++i) {
                        if (filter_node(chunk_ids[i], props_map)) local.push_back(chunk_ids[i]);
                    }
                });
            }
            for (auto& th : threads) th.join();
            for (auto& r : per_thread_results) {
                filtered.insert(filtered.end(), r.begin(), r.end());
            }
        } else {
            for (uint64_t id : chunk_ids) {
                if (filter_node(id, props_map)) {
                    filtered.push_back(id);
                }
            }
        }
        // batch_results goes out of scope — memory freed
    }

    it->second = std::move(filtered);
    return true;
}

bool executor::execute_return(const std::shared_ptr<return_clause>& ret, std::string& result_json) {
    size_t max_rows = 1;

    // Collect all variable names referenced in RETURN items
    std::set<std::string> needed_vars;
    for (const auto& item : ret->items) {
        std::string var_name;
        if (item.expr->type() == node_type::variable) {
            var_name = std::dynamic_pointer_cast<variable>(item.expr)->name;
        } else if (item.expr->type() == node_type::property_access) {
            var_name = std::dynamic_pointer_cast<property_access>(item.expr)->variable_name;
        }
        if (!var_name.empty()) {
            needed_vars.insert(var_name);
            auto it = context_.find(var_name);
            if (it != context_.end()) {
                max_rows = std::max(max_rows, it->second.size());
            }
        }
    }

    // ADR-0012: Batch-read node properties in chunks to cap peak memory
    result_json.clear();
    result_json.reserve(std::min(max_rows, BATCH_SIZE) * ret->items.size() * 64);
    result_json += '[';

    for (size_t batch_start = 0; batch_start < max_rows; batch_start += BATCH_SIZE) {
        size_t batch_end = std::min(batch_start + BATCH_SIZE, max_rows);

        // Load properties only for this batch's rows
        std::map<std::string, std::unordered_map<uint64_t, std::string>> batch_cache;
        for (const auto& var_name : needed_vars) {
            auto ctx_it = context_.find(var_name);
            if (ctx_it == context_.end()) continue;
            std::vector<uint64_t> batch_ids;
            for (size_t r = batch_start; r < batch_end && r < ctx_it->second.size(); ++r) {
                batch_ids.push_back(ctx_it->second[r]);
            }
            if (batch_ids.empty()) continue;
            std::vector<std::pair<uint64_t, std::string>> batch_results;
            store_.get_nodes_batch(tx_, batch_ids, batch_results);
            auto& cache = batch_cache[var_name];
            cache.reserve(batch_results.size());
            for (auto& [id, props] : batch_results) {
                cache[id] = std::move(props);
            }
        }

        // Emit rows for this batch
        for (size_t row = batch_start; row < batch_end; ++row) {
            if (row > 0) result_json += ", ";
            result_json += '{';
            bool first = true;
            for (const auto& item : ret->items) {
                if (!first) result_json += ", ";
                first = false;

                if (item.expr->type() == node_type::variable) {
                    auto var = std::dynamic_pointer_cast<variable>(item.expr);
                    const std::string& alias = item.alias.empty() ? var->name : item.alias;
                    result_json += '"';
                    result_json += alias;
                    result_json += "\": ";

                    auto it = context_.find(var->name);
                    if (it != context_.end() && row < it->second.size()) {
                        uint64_t id = it->second[row];
                        auto& cache = batch_cache[var->name];
                        auto cache_it = cache.find(id);
                        if (cache_it != cache.end() && !cache_it->second.empty()) {
                            result_json += cache_it->second;
                        } else {
                            edge_data ed;
                            if (store_.get_edge(tx_, id, ed)) {
                                result_json += "{\"_id\": ";
                                result_json += std::to_string(id);
                                result_json += ", \"_from\": ";
                                result_json += std::to_string(ed.from_id);
                                result_json += ", \"_to\": ";
                                result_json += std::to_string(ed.to_id);
                                result_json += ", \"_label\": \"";
                                result_json += ed.label;
                                result_json += '"';
                                if (!ed.properties.empty() && ed.properties != "{}") {
                                    result_json += ", \"_properties\": ";
                                    result_json += ed.properties;
                                }
                                result_json += '}';
                            } else {
                                result_json += "null";
                            }
                        }
                    } else {
                        result_json += "null";
                    }
                } else if (item.expr->type() == node_type::property_access) {
                    auto pa = std::dynamic_pointer_cast<property_access>(item.expr);
                    const std::string& alias = item.alias.empty() ? pa->variable_name + "." + pa->property_key : item.alias;
                    result_json += '"';
                    result_json += alias;
                    result_json += "\": ";

                    auto it = context_.find(pa->variable_name);
                    if (it != context_.end() && row < it->second.size()) {
                        uint64_t id = it->second[row];
                        auto& cache = batch_cache[pa->variable_name];
                        auto cache_it = cache.find(id);
                        std::string props;
                        if (cache_it != cache.end()) {
                            props = cache_it->second;
                        }
                        if (!props.empty()) {
                            std::string val = get_json_value(props, pa->property_key);
                            if (!val.empty()) {
                                bool is_num = !val.empty() && (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-');
                                if (is_num) {
                                    result_json += val;
                                } else {
                                    result_json += '"';
                                    result_json += val;
                                    result_json += '"';
                                }
                            } else {
                                result_json += "null";
                            }
                        } else {
                            result_json += "null";
                        }
                    } else {
                        result_json += "null";
                    }
                }
            }
            result_json += '}';
        }
        // batch_cache goes out of scope — memory freed before next batch
    }
    result_json += ']';
    return true;
}

bool executor::execute_delete(const std::shared_ptr<delete_clause>& del) {
    for (const auto& var_name : del->variables) {
        auto it = context_.find(var_name);
        if (it == context_.end()) continue;

        for (uint64_t id : it->second) {
            std::string props;
            if (store_.get_node(tx_, id, props) && !props.empty()) {
                if (del->detach) {
                    std::vector<uint64_t> out_edges, in_edges;
                    store_.get_outgoing_edges(tx_, id, out_edges);
                    store_.get_incoming_edges(tx_, id, in_edges);
                    for (uint64_t eid : out_edges) store_.delete_edge(tx_, eid);
                    for (uint64_t eid : in_edges) store_.delete_edge(tx_, eid);
                }
                store_.delete_node(tx_, id, "");
            } else {
                store_.delete_edge(tx_, id);
            }
        }
        context_.erase(it);
    }
    return true;
}

bool executor::execute_set(const std::shared_ptr<set_clause>& set) {
    // Phase 1: Accumulate all SET assignments per node (deferred index update)
    // node_id → (label, current_properties)
    std::unordered_map<uint64_t, std::pair<std::string, std::string>> pending;

    for (const auto& asgn : set->assignments) {
        auto it = context_.find(asgn.variable);
        if (it == context_.end()) continue;

        std::string new_value;
        bool is_string = false;
        if (auto lit = std::dynamic_pointer_cast<literal>(asgn.value)) {
            new_value = lit->value;
            is_string = lit->is_string;
        }

        std::string label;
        auto label_it = context_labels_.find(asgn.variable);
        if (label_it != context_labels_.end()) label = label_it->second;

        for (uint64_t id : it->second) {
            auto& [lbl, props] = pending[id];
            if (props.empty()) {
                // First SET for this node — fetch current properties
                store_.get_node(tx_, id, props);
                lbl = label;
            }
            props = update_json_property(props, asgn.property, new_value, is_string);
        }
    }

    // Phase 2: Apply final state with single index update per node
    for (auto& [id, update] : pending) {
        auto& [label, props] = update;
        if (!label.empty()) {
            store_.update_node_with_label(tx_, id, label, props);
        } else {
            store_.update_node(tx_, id, props);
        }
    }
    return true;
}

bool executor::execute_unwind(const std::shared_ptr<unwind_clause>& unwind,
                              const statement& stmt, size_t& ci, std::string& result_json) {
    // Evaluate list expression
    if (unwind->list_expr->type() != node_type::list_literal) {
        return true; // Only list literals supported
    }
    auto list = std::dynamic_pointer_cast<list_literal_expr>(unwind->list_expr);
    if (!list || list->elements.empty()) return true;

    // Collect subsequent clause indices (everything after UNWIND)
    size_t start_ci = ci + 1;
    size_t end_ci = stmt.clauses.size();

    // For each element in the list, bind alias and execute subsequent clauses
    for (auto& item_expr : list->elements) {
        // Build unwind binding from map literal
        unwind_binding binding;
        if (item_expr->type() == node_type::map_literal) {
            auto map = std::dynamic_pointer_cast<map_literal_expr>(item_expr);
            for (auto& [key, val_expr] : map->entries) {
                if (auto lit = std::dynamic_pointer_cast<literal>(val_expr)) {
                    binding.properties[key] = lit->value;
                    binding.is_string[key] = lit->is_string;
                }
            }
        } else if (auto lit = std::dynamic_pointer_cast<literal>(item_expr)) {
            binding.properties["_value"] = lit->value;
            binding.is_string["_value"] = lit->is_string;
        }

        unwind_context_[unwind->alias] = std::move(binding);

        // Execute subsequent clauses with this binding
        for (size_t sub_ci = start_ci; sub_ci < end_ci; ++sub_ci) {
            const auto& clause = stmt.clauses[sub_ci];
            switch (clause->type()) {
                case clause_type::create:
                    if (!execute_create(std::dynamic_pointer_cast<create_clause>(clause))) return false;
                    break;
                case clause_type::match:
                    if (!execute_match(std::dynamic_pointer_cast<match_clause>(clause))) return false;
                    break;
                case clause_type::where:
                    if (!execute_where(std::dynamic_pointer_cast<where_clause>(clause))) return false;
                    break;
                case clause_type::return_clause:
                    if (!execute_return(std::dynamic_pointer_cast<return_clause>(clause), result_json)) return false;
                    break;
                case clause_type::delete_clause:
                    if (!execute_delete(std::dynamic_pointer_cast<delete_clause>(clause))) return false;
                    break;
                case clause_type::set_clause:
                    if (!execute_set(std::dynamic_pointer_cast<set_clause>(clause))) return false;
                    break;
                default:
                    break;
            }
        }

        // Clear per-iteration node context (but keep unwind_context_ for next iteration)
        context_.clear();
        context_labels_.clear();
    }

    // Clean up unwind binding
    unwind_context_.erase(unwind->alias);

    // Skip all remaining clauses (UNWIND consumed them)
    ci = end_ci - 1;
    return true;
}

std::string executor::get_json_value(const std::string& json, const std::string& key) {
    // Fast path: search for "key": pattern
    const size_t key_len = key.size();
    size_t pos = 0;

    while (pos < json.size()) {
        pos = json.find('"', pos);
        if (pos == std::string::npos) return "";
        pos++; // skip opening quote

        // Check if this key matches
        if (pos + key_len < json.size() &&
            json[pos + key_len] == '"' &&
            std::memcmp(json.data() + pos, key.data(), key_len) == 0) {

            pos += key_len + 1; // skip key and closing quote
            // Skip to colon
            while (pos < json.size() && json[pos] != ':') pos++;
            if (pos >= json.size()) return "";
            pos++; // skip ':'

            // Skip whitespace
            while (pos < json.size() && json[pos] == ' ') pos++;
            if (pos >= json.size()) return "";

            if (json[pos] == '"') {
                size_t end = json.find('"', pos + 1);
                if (end == std::string::npos) return "";
                return json.substr(pos + 1, end - pos - 1);
            } else {
                size_t end = pos;
                while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ') end++;
                return json.substr(pos, end - pos);
            }
        }

        // Skip to next quote pair
        pos = json.find('"', pos);
        if (pos == std::string::npos) return "";
        pos++; // skip closing quote of this key or value string
    }
    return "";
}

std::string executor::update_json_property(const std::string& json, const std::string& key, const std::string& value, bool is_string) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);

    std::string new_val = is_string ? ("\"" + value + "\"") : value;

    if (pos != std::string::npos) {
        auto colon = json.find(':', pos + search.size());
        if (colon == std::string::npos) return json;
        colon++;
        while (colon < json.size() && json[colon] == ' ') colon++;

        size_t val_end;
        if (json[colon] == '"') {
            val_end = json.find('"', colon + 1);
            if (val_end != std::string::npos) val_end++;
        } else {
            val_end = colon;
            while (val_end < json.size() && json[val_end] != ',' && json[val_end] != '}') val_end++;
        }

        std::string result;
        result.reserve(json.size() + new_val.size());
        result.append(json, 0, colon);
        result += new_val;
        result.append(json, val_end);
        return result;
    } else {
        auto closing = json.rfind('}');
        if (closing == std::string::npos) return json;
        std::string result;
        result.reserve(json.size() + key.size() + new_val.size() + 8);
        result.append(json, 0, closing);
        if (closing > 1 && json[closing - 1] != '{') {
            result += ", ";
        }
        result += '"';
        result += key;
        result += "\": ";
        result += new_val;
        result += '}';
        return result;
    }
}

} // namespace tateyama::framework::graph::core
