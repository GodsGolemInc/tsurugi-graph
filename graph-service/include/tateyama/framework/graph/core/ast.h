#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <optional>
#include <variant>

namespace tateyama::framework::graph::core {

// --- Basic Types ---

enum class node_type {
    variable,
    literal_string,
    literal_number,
    property_access
};

struct expression {
    virtual ~expression() = default;
    virtual node_type type() const = 0;
};

struct variable : public expression {
    std::string name;
    node_type type() const override { return node_type::variable; }
    variable(std::string n) : name(std::move(n)) {}
};

struct literal : public expression {
    std::string value;
    bool is_string; // true if string literal, false if number
    node_type type() const override { return is_string ? node_type::literal_string : node_type::literal_number; }
    literal(std::string v, bool is_str) : value(std::move(v)), is_string(is_str) {}
};

struct property_access : public expression {
    std::string variable_name;
    std::string property_key;
    node_type type() const override { return node_type::property_access; }
    property_access(std::string v, std::string k) : variable_name(std::move(v)), property_key(std::move(k)) {}
};

struct binary_expression : public expression {
    // Simplified: left OP right
    // For prototype, we might not implement full expression tree yet, 
    // but keep structure ready.
};

// --- Pattern Matching ---

struct pattern_node {
    std::string variable;
    std::string label; // optional
    std::map<std::string, std::shared_ptr<expression>> properties;
};

struct pattern_relationship {
    std::string variable;
    std::string type; // label
    std::string direction; // "->", "<-", "-"
    std::map<std::string, std::shared_ptr<expression>> properties;
};

struct pattern_element {
    std::shared_ptr<pattern_node> node;
    std::shared_ptr<pattern_relationship> relationship; // Optional, connects to next node
};

struct pattern_path {
    std::vector<pattern_element> elements; // (Node)-[Rel]-(Node)...
};

// --- Clauses ---

enum class clause_type {
    create,
    match,
    return_clause,
    where
};

struct clause {
    virtual ~clause() = default;
    virtual clause_type type() const = 0;
};

struct create_clause : public clause {
    std::vector<pattern_path> paths;
    clause_type type() const override { return clause_type::create; }
};

struct match_clause : public clause {
    std::vector<pattern_path> paths;
    clause_type type() const override { return clause_type::match; }
};

struct return_item {
    std::shared_ptr<expression> expr;
    std::string alias;
};

struct return_clause : public clause {
    std::vector<return_item> items;
    clause_type type() const override { return clause_type::return_clause; }
};

struct where_clause : public clause {
    // Simplified: property = value
    std::string variable;
    std::string property;
    std::string op; // =, >, <
    std::shared_ptr<expression> value;
    clause_type type() const override { return clause_type::where; }
};

// --- Statement ---

struct statement {
    std::vector<std::shared_ptr<clause>> clauses;
};

} // namespace tateyama::framework::graph::core
