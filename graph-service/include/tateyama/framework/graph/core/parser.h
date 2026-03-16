#pragma once

#include <string>
#include <vector>
#include <optional>
#include "ast.h"

namespace tateyama::framework::graph::core {

enum class token_type {
    keyword_create,
    keyword_match,
    keyword_return,
    keyword_where,
    identifier,
    string_literal,
    number_literal,
    lparen, rparen,
    lbracket, rbracket,
    lbrace, rbrace,
    colon,
    comma,
    arrow_right, // ->
    arrow_left, // <-
    dash, // -
    dot, // .
    equals, // =
    less_than, // <
    greater_than, // >
    keyword_as,
    keyword_delete,
    keyword_set,
    keyword_unwind,
    eof,
    unknown
};

struct token {
    token_type type;
    std::string text;
    size_t position;
};

class lexer {
public:
    lexer(std::string input) : input_(std::move(input)), pos_(0) {}
    std::vector<token> tokenize();

private:
    std::string input_;
    size_t pos_;

    char peek() const;
    char advance();
    bool is_eof() const;
    void skip_whitespace();
    token scan_identifier_or_keyword();
    token scan_string();
    token scan_number();
    token scan_symbol();
};

class parser {
public:
    parser(std::vector<token> tokens) : tokens_(std::move(tokens)), pos_(0) {}
    statement parse();

private:
    std::vector<token> tokens_;
    size_t pos_;

    const token& peek() const;
    token advance();
    bool match(token_type type);
    token consume(token_type type, const std::string& err_msg);

    // Parse methods
    std::shared_ptr<clause> parse_clause();
    std::shared_ptr<create_clause> parse_create();
    std::shared_ptr<match_clause> parse_match();
    std::shared_ptr<return_clause> parse_return();
    std::shared_ptr<where_clause> parse_where();
    std::shared_ptr<delete_clause> parse_delete();
    std::shared_ptr<delete_clause> parse_delete_impl(bool detach);
    std::shared_ptr<set_clause> parse_set();
    std::shared_ptr<unwind_clause> parse_unwind();

    pattern_path parse_pattern_path();
    std::shared_ptr<pattern_node> parse_node_pattern();
    std::shared_ptr<pattern_relationship> parse_relationship_pattern();
    std::map<std::string, std::shared_ptr<expression>> parse_properties_map();
    std::shared_ptr<expression> parse_expression();
};

} // namespace tateyama::framework::graph::core
