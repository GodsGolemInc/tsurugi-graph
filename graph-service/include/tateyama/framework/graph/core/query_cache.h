#pragma once

#include <tateyama/framework/graph/core/ast.h>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <list>
#include <vector>

namespace tateyama::framework::graph::core {

/**
 * @brief Normalize a Cypher query by replacing literals with placeholders.
 *
 * Extracts string/number literals and replaces them with $0, $1, ...
 * Returns the normalized query string and populates out_literals with
 * the extracted literal values in order.
 *
 * Example: "MATCH (n:Person) WHERE n.name = 'Alice'" →
 *          "MATCH (n:Person) WHERE n.name = $0"  literals=["Alice"]
 */
inline std::string normalize_query(const std::string& query, std::vector<std::string>& out_literals) {
    out_literals.clear();
    std::string result;
    result.reserve(query.size());
    size_t i = 0;
    while (i < query.size()) {
        char c = query[i];
        if (c == '\'' || c == '"') {
            // String literal — extract value, replace with placeholder
            char quote = c;
            i++; // skip opening quote
            size_t start = i;
            while (i < query.size() && query[i] != quote) {
                if (query[i] == '\\') i++; // skip escape
                i++;
            }
            out_literals.emplace_back(query.substr(start, i - start));
            if (i < query.size()) i++; // skip closing quote
            result += '$';
            result += std::to_string(out_literals.size() - 1);
        } else if (std::isdigit(c) || (c == '-' && i + 1 < query.size() && std::isdigit(query[i + 1]))) {
            // Check it's not part of an identifier (preceded by alnum/_)
            if (i > 0 && (std::isalnum(query[i - 1]) || query[i - 1] == '_')) {
                result += c;
                i++;
                continue;
            }
            // Number literal
            size_t start = i;
            if (c == '-') i++;
            while (i < query.size() && (std::isdigit(query[i]) || query[i] == '.')) i++;
            out_literals.emplace_back(query.substr(start, i - start));
            result += '$';
            result += std::to_string(out_literals.size() - 1);
        } else {
            result += c;
            i++;
        }
    }
    return result;
}

/**
 * @brief Walk AST literal nodes in a deterministic order (same as bind_literals).
 *
 * The visitor function receives each literal node in order.
 * Used by both extract_literals and bind_literals for consistent ordering.
 */
inline void visit_literals(statement& stmt, std::function<void(literal*)> visitor) {
    std::function<void(expression*)> visit_expr = [&](expression* expr) {
        if (!expr) return;
        if (auto lit = dynamic_cast<literal*>(expr)) {
            visitor(lit);
        } else if (auto bin = dynamic_cast<binary_expression*>(expr)) {
            visit_expr(bin->left.get());
            visit_expr(bin->right.get());
        }
    };

    for (auto& clause_ptr : stmt.clauses) {
        switch (clause_ptr->type()) {
            case clause_type::create: {
                auto* cr = static_cast<create_clause*>(clause_ptr.get());
                for (auto& path : cr->paths) {
                    for (auto& elem : path.elements) {
                        if (elem.node) {
                            for (auto& [k, v] : elem.node->properties) visit_expr(v.get());
                        }
                        if (elem.relationship) {
                            for (auto& [k, v] : elem.relationship->properties) visit_expr(v.get());
                        }
                    }
                }
                break;
            }
            case clause_type::where: {
                auto* w = static_cast<where_clause*>(clause_ptr.get());
                visit_expr(w->condition.get());
                break;
            }
            case clause_type::set_clause: {
                auto* s = static_cast<set_clause*>(clause_ptr.get());
                for (auto& asgn : s->assignments) visit_expr(asgn.value.get());
                break;
            }
            case clause_type::unwind: {
                auto* u = static_cast<unwind_clause*>(clause_ptr.get());
                visit_expr(u->list_expr.get());
                break;
            }
            default: break;
        }
    }
}

/**
 * @brief Extract literal values from AST in deterministic traversal order.
 *
 * Used to generate a normalized cache key from a parsed AST.
 */
inline void extract_literals(statement& stmt, std::vector<std::string>& out_literals) {
    out_literals.clear();
    visit_literals(stmt, [&](literal* lit) {
        out_literals.push_back(lit->value);
    });
}

/**
 * @brief Generate a normalized cache key from AST by replacing literals with placeholders.
 *
 * Uses the same traversal order as bind_literals, ensuring consistent ordering.
 */
inline std::string ast_cache_key(statement& stmt) {
    std::string key;
    size_t lit_idx = 0;
    for (auto& clause_ptr : stmt.clauses) {
        switch (clause_ptr->type()) {
            case clause_type::create: key += "CREATE|"; break;
            case clause_type::match: {
                auto* m = static_cast<match_clause*>(clause_ptr.get());
                key += "MATCH";
                for (auto& path : m->paths) {
                    for (auto& elem : path.elements) {
                        if (elem.node) {
                            key += "(";
                            if (!elem.node->label.empty()) key += ":" + elem.node->label;
                            if (!elem.node->properties.empty()) {
                                key += "{";
                                for (auto& [k, v] : elem.node->properties) key += k + ",";
                                key += "}";
                            }
                            key += ")";
                        }
                        if (elem.relationship) {
                            key += elem.relationship->direction + ":" + elem.relationship->type;
                        }
                    }
                }
                key += "|";
                break;
            }
            case clause_type::where: {
                auto* w = static_cast<where_clause*>(clause_ptr.get());
                key += "WHERE ";
                key += w->variable + "." + w->property + " " + w->op + " $|";
                break;
            }
            case clause_type::return_clause: key += "RETURN|"; break;
            case clause_type::set_clause: {
                auto* s = static_cast<set_clause*>(clause_ptr.get());
                key += "SET ";
                for (auto& asgn : s->assignments) key += asgn.variable + "." + asgn.property + "=$,";
                key += "|";
                break;
            }
            case clause_type::delete_clause: key += "DELETE|"; break;
            case clause_type::unwind: key += "UNWIND|"; break;
        }
    }
    return key;
}

/**
 * @brief Bind literal values into an AST's literal nodes (deterministic traversal order).
 *
 * Walks the AST in the same order as extract_literals/visit_literals
 * and replaces literal node values with the provided values.
 */
inline void bind_literals(statement& stmt, const std::vector<std::string>& literals) {
    size_t idx = 0;
    visit_literals(stmt, [&](literal* lit) {
        if (idx < literals.size()) {
            lit->value = literals[idx++];
        }
    });
}

// --- Deep copy helpers for AST ---

inline std::shared_ptr<expression> clone_expr(const std::shared_ptr<expression>& src) {
    if (!src) return nullptr;
    switch (src->type()) {
        case node_type::literal_string:
        case node_type::literal_number: {
            auto* lit = static_cast<literal*>(src.get());
            return std::make_shared<literal>(lit->value, lit->is_string);
        }
        case node_type::variable: {
            auto* v = static_cast<variable*>(src.get());
            return std::make_shared<variable>(v->name);
        }
        case node_type::property_access: {
            auto* pa = static_cast<property_access*>(src.get());
            return std::make_shared<property_access>(pa->variable_name, pa->property_key);
        }
        case node_type::binary: {
            auto* bin = static_cast<binary_expression*>(src.get());
            return std::make_shared<binary_expression>(clone_expr(bin->left), bin->op, clone_expr(bin->right));
        }
        case node_type::list_literal: {
            auto* ll = static_cast<list_literal_expr*>(src.get());
            auto copy = std::make_shared<list_literal_expr>();
            for (auto& e : ll->elements) copy->elements.push_back(clone_expr(e));
            return copy;
        }
        case node_type::map_literal: {
            auto* ml = static_cast<map_literal_expr*>(src.get());
            auto copy = std::make_shared<map_literal_expr>();
            for (auto& [k, v] : ml->entries) copy->entries[k] = clone_expr(v);
            return copy;
        }
    }
    return nullptr;
}

inline std::map<std::string, std::shared_ptr<expression>> clone_props(
        const std::map<std::string, std::shared_ptr<expression>>& src) {
    std::map<std::string, std::shared_ptr<expression>> result;
    for (auto& [k, v] : src) result[k] = clone_expr(v);
    return result;
}

inline pattern_path clone_path(const pattern_path& src) {
    pattern_path result;
    for (auto& elem : src.elements) {
        pattern_element pe;
        if (elem.node) {
            pe.node = std::make_shared<pattern_node>();
            pe.node->variable = elem.node->variable;
            pe.node->label = elem.node->label;
            pe.node->properties = clone_props(elem.node->properties);
        }
        if (elem.relationship) {
            pe.relationship = std::make_shared<pattern_relationship>();
            pe.relationship->variable = elem.relationship->variable;
            pe.relationship->type = elem.relationship->type;
            pe.relationship->direction = elem.relationship->direction;
            pe.relationship->properties = clone_props(elem.relationship->properties);
        }
        result.elements.push_back(std::move(pe));
    }
    return result;
}

/**
 * @brief Deep-copy a statement AST for safe literal rebinding.
 *
 * Clones all expression nodes so that bind_literals can safely modify
 * the copy without affecting the cached original.
 */
inline std::shared_ptr<statement> deep_copy_statement(const statement& src) {
    auto copy = std::make_shared<statement>();
    for (auto& clause_ptr : src.clauses) {
        switch (clause_ptr->type()) {
            case clause_type::create: {
                auto* cr = static_cast<create_clause*>(clause_ptr.get());
                auto cc = std::make_shared<create_clause>();
                for (auto& p : cr->paths) cc->paths.push_back(clone_path(p));
                copy->clauses.push_back(std::move(cc));
                break;
            }
            case clause_type::match: {
                auto* m = static_cast<match_clause*>(clause_ptr.get());
                auto mc = std::make_shared<match_clause>();
                for (auto& p : m->paths) mc->paths.push_back(clone_path(p));
                copy->clauses.push_back(std::move(mc));
                break;
            }
            case clause_type::where: {
                auto* w = static_cast<where_clause*>(clause_ptr.get());
                auto wc = std::make_shared<where_clause>();
                wc->condition = clone_expr(w->condition);
                wc->variable = w->variable;
                wc->property = w->property;
                wc->op = w->op;
                // Legacy value must point to same object as condition->right
                // so that bind_literals updates both via condition traversal
                if (auto bin = std::dynamic_pointer_cast<binary_expression>(wc->condition)) {
                    wc->value = bin->right;
                } else {
                    wc->value = clone_expr(w->value);
                }
                copy->clauses.push_back(std::move(wc));
                break;
            }
            case clause_type::return_clause: {
                auto* r = static_cast<return_clause*>(clause_ptr.get());
                auto rc = std::make_shared<return_clause>();
                for (auto& item : r->items) {
                    rc->items.push_back({clone_expr(item.expr), item.alias});
                }
                copy->clauses.push_back(std::move(rc));
                break;
            }
            case clause_type::set_clause: {
                auto* s = static_cast<set_clause*>(clause_ptr.get());
                auto sc = std::make_shared<set_clause>();
                for (auto& asgn : s->assignments) {
                    sc->assignments.push_back({asgn.variable, asgn.property, clone_expr(asgn.value)});
                }
                copy->clauses.push_back(std::move(sc));
                break;
            }
            case clause_type::delete_clause: {
                auto* d = static_cast<delete_clause*>(clause_ptr.get());
                auto dc = std::make_shared<delete_clause>();
                dc->variables = d->variables;
                dc->detach = d->detach;
                copy->clauses.push_back(std::move(dc));
                break;
            }
            case clause_type::unwind: {
                auto* u = static_cast<unwind_clause*>(clause_ptr.get());
                auto uc = std::make_shared<unwind_clause>();
                uc->list_expr = clone_expr(u->list_expr);
                uc->alias = u->alias;
                copy->clauses.push_back(std::move(uc));
                break;
            }
        }
    }
    return copy;
}

/**
 * @brief LRU cache for parsed Cypher query ASTs (ADR-0003, ADR-0011).
 *
 * Thread-safe: all public methods are guarded by a mutex.
 * Keyed by normalized query string (ADR-0011): literals replaced with
 * placeholders for higher cache hit rate.
 */
class query_cache {
public:
    explicit query_cache(size_t max_size = 1024) : max_size_(max_size) {}

    /**
     * @brief Look up a cached statement for the given query string.
     * @return shared_ptr to the cached statement, or nullptr on cache miss.
     */
    std::shared_ptr<statement> get(const std::string& query) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(query);
        if (it == cache_.end()) return nullptr;

        // Move to front of LRU list
        lru_order_.erase(lru_map_.at(query));
        lru_order_.push_front(query);
        lru_map_[query] = lru_order_.begin();

        return it->second;
    }

    /**
     * @brief Store a parsed statement in the cache.
     */
    void put(const std::string& query, std::shared_ptr<statement> stmt) {
        std::lock_guard<std::mutex> lock(mutex_);

        // If already cached, update and move to front
        auto it = cache_.find(query);
        if (it != cache_.end()) {
            it->second = std::move(stmt);
            lru_order_.erase(lru_map_[query]);
            lru_order_.push_front(query);
            lru_map_[query] = lru_order_.begin();
            return;
        }

        // Evict LRU entry if at capacity
        if (cache_.size() >= max_size_) {
            const auto& lru_key = lru_order_.back();
            cache_.erase(lru_key);
            lru_map_.erase(lru_key);
            lru_order_.pop_back();
        }

        cache_[query] = std::move(stmt);
        lru_order_.push_front(query);
        lru_map_[query] = lru_order_.begin();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_order_.clear();
        lru_map_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

private:
    size_t max_size_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<statement>> cache_;
    mutable std::list<std::string> lru_order_;
    mutable std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;
};

} // namespace tateyama::framework::graph::core
