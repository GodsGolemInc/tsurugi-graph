#include <tateyama/framework/graph/core/parser.h>
#include <cctype>
#include <stdexcept>
#include <algorithm>

namespace tateyama::framework::graph::core {

// --- Lexer ---

std::vector<token> lexer::tokenize() {
    std::vector<token> tokens;
    while (!is_eof()) {
        skip_whitespace();
        if (is_eof()) break;

        char c = peek();
        if (isalpha(c) || c == '_') {
            tokens.push_back(scan_identifier_or_keyword());
        } else if (isdigit(c)) {
            tokens.push_back(scan_number());
        } else if (c == '\'' || c == '"') {
            tokens.push_back(scan_string());
        } else {
            tokens.push_back(scan_symbol());
        }
    }
    tokens.push_back({token_type::eof, "", pos_});
    return tokens;
}

char lexer::peek() const {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_];
}

char lexer::advance() {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_++];
}

bool lexer::is_eof() const {
    return pos_ >= input_.size();
}

void lexer::skip_whitespace() {
    while (!is_eof() && isspace(peek())) {
        advance();
    }
}

// Case-insensitive string comparison without allocation
static bool ci_eq(const char* s, size_t len, const char* keyword) {
    for (size_t i = 0; i < len; ++i) {
        if (static_cast<char>(toupper(static_cast<unsigned char>(s[i]))) != keyword[i]) return false;
    }
    return keyword[len] == '\0';
}

token lexer::scan_identifier_or_keyword() {
    size_t start = pos_;
    while (!is_eof() && (isalnum(peek()) || peek() == '_')) {
        advance();
    }
    size_t len = pos_ - start;
    std::string text = input_.substr(start, len);
    token_type type = token_type::identifier;

    const char* s = input_.data() + start;
    switch (len) {
        case 2: if (ci_eq(s, len, "AS")) type = token_type::keyword_as; break;
        case 3: if (ci_eq(s, len, "SET")) type = token_type::keyword_set; break;
        case 5: if (ci_eq(s, len, "MATCH")) type = token_type::keyword_match;
                else if (ci_eq(s, len, "WHERE")) type = token_type::keyword_where; break;
        case 6: if (ci_eq(s, len, "CREATE")) type = token_type::keyword_create;
                else if (ci_eq(s, len, "RETURN")) type = token_type::keyword_return;
                else if (ci_eq(s, len, "DELETE")) type = token_type::keyword_delete;
                else if (ci_eq(s, len, "DETACH")) type = token_type::keyword_delete;
                else if (ci_eq(s, len, "UNWIND")) type = token_type::keyword_unwind; break;
    }

    return {type, std::move(text), start};
}

token lexer::scan_string() {
    size_t start = pos_;
    char quote = advance();
    while (!is_eof() && peek() != quote) {
        if (peek() == '\\') advance(); // skip escape char
        advance();
    }
    if (!is_eof()) advance(); // consume closing quote
    // Strip quotes directly without intermediate copy
    return {token_type::string_literal, input_.substr(start + 1, pos_ - start - 2), start};
}

token lexer::scan_number() {
    size_t start = pos_;
    while (!is_eof() && (isdigit(peek()) || peek() == '.')) {
        advance();
    }
    return {token_type::number_literal, input_.substr(start, pos_ - start), start};
}

token lexer::scan_symbol() {
    size_t start = pos_;
    char c = advance();
    switch (c) {
        case '(': return {token_type::lparen, "(", start};
        case ')': return {token_type::rparen, ")", start};
        case '{': return {token_type::lbrace, "{", start};
        case '}': return {token_type::rbrace, "}", start};
        case '[': return {token_type::lbracket, "[", start};
        case ']': return {token_type::rbracket, "]", start};
        case ':': return {token_type::colon, ":", start};
        case ',': return {token_type::comma, ",", start};
        case '.': return {token_type::dot, ".", start};
        case '=': return {token_type::equals, "=", start};
        case '>':
            if (peek() == '=') {
                advance();
                return {token_type::greater_than, ">=", start};
            }
            return {token_type::greater_than, ">", start};
        case '<':
            if (peek() == '-') {
                advance();
                return {token_type::arrow_left, "<-", start};
            }
            if (peek() == '>') {
                advance();
                return {token_type::greater_than, "<>", start}; // not-equal operator
            }
            if (peek() == '=') {
                advance();
                return {token_type::less_than, "<=", start};
            }
            return {token_type::less_than, "<", start};
        case '-':
            if (peek() == '>') {
                advance();
                return {token_type::arrow_right, "->", start};
            }
            return {token_type::dash, "-", start};
    }
    return {token_type::unknown, std::string(1, c), start};
}

// --- Parser ---

statement parser::parse() {
    statement stmt;
    while (peek().type != token_type::eof) {
        stmt.clauses.push_back(parse_clause());
    }
    return stmt;
}

const token& parser::peek() const {
    if (pos_ >= tokens_.size()) {
        static const token eof_token{token_type::eof, "", 0};
        return eof_token;
    }
    return tokens_[pos_];
}

token parser::advance() {
    if (pos_ >= tokens_.size()) {
        return {token_type::eof, "", 0};
    }
    return tokens_[pos_++];
}

bool parser::match(token_type type) {
    if (peek().type == type) {
        advance();
        return true;
    }
    return false;
}

token parser::consume(token_type type, const std::string& err_msg) {
    if (peek().type == type) {
        return advance();
    }
    throw std::runtime_error(err_msg + " Got: " + peek().text);
}

std::shared_ptr<clause> parser::parse_clause() {
    if (match(token_type::keyword_create)) return parse_create();
    if (match(token_type::keyword_match)) return parse_match();
    if (match(token_type::keyword_return)) return parse_return();
    if (match(token_type::keyword_where)) return parse_where();
    if (peek().type == token_type::keyword_delete) {
        // Handle DETACH DELETE or DELETE
        auto tok = advance();
        std::string upper = tok.text;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        bool detach = (upper == "DETACH");
        if (detach) {
            consume(token_type::keyword_delete, "Expected 'DELETE' after 'DETACH'");
        }
        return parse_delete_impl(detach);
    }
    if (match(token_type::keyword_set)) return parse_set();
    if (match(token_type::keyword_unwind)) return parse_unwind();
    throw std::runtime_error("Unexpected token at start of clause: " + peek().text);
}

std::shared_ptr<create_clause> parser::parse_create() {
    auto c = std::make_shared<create_clause>();
    do {
        c->paths.push_back(parse_pattern_path());
    } while (match(token_type::comma));
    return c;
}

std::shared_ptr<match_clause> parser::parse_match() {
    auto c = std::make_shared<match_clause>();
    do {
        c->paths.push_back(parse_pattern_path());
    } while (match(token_type::comma));
    return c;
}

std::shared_ptr<return_clause> parser::parse_return() {
    auto c = std::make_shared<return_clause>();
    do {
        return_item item;
        item.expr = parse_expression();
        // Handle AS alias
        if (match(token_type::keyword_as)) {
            if (peek().type == token_type::identifier) {
                item.alias = advance().text;
            }
        }
        c->items.push_back(item);
    } while (match(token_type::comma));
    return c;
}

std::shared_ptr<where_clause> parser::parse_where() {
    auto c = std::make_shared<where_clause>();
    auto left = parse_expression();

    // Check for comparison operator
    if (peek().type == token_type::equals ||
        peek().type == token_type::less_than ||
        peek().type == token_type::greater_than) {
        std::string op = advance().text;
        auto right = parse_expression();
        c->condition = std::make_shared<binary_expression>(left, op, right);

        // Populate legacy fields for backward compatibility
        if (auto prop_access = std::dynamic_pointer_cast<property_access>(left)) {
            c->variable = prop_access->variable_name;
            c->property = prop_access->property_key;
            c->op = op;
            c->value = right;
        }
    } else {
        c->condition = left;
    }

    return c;
}

std::shared_ptr<delete_clause> parser::parse_delete() {
    return parse_delete_impl(false);
}

std::shared_ptr<delete_clause> parser::parse_delete_impl(bool detach) {
    auto c = std::make_shared<delete_clause>();
    c->detach = detach;
    do {
        if (peek().type == token_type::identifier) {
            c->variables.push_back(advance().text);
        } else {
            throw std::runtime_error("Expected variable name in DELETE clause");
        }
    } while (match(token_type::comma));
    return c;
}

std::shared_ptr<set_clause> parser::parse_set() {
    auto c = std::make_shared<set_clause>();
    do {
        set_clause::assignment asgn;
        if (peek().type != token_type::identifier) {
            throw std::runtime_error("Expected variable name in SET clause");
        }
        asgn.variable = advance().text;
        consume(token_type::dot, "Expected '.' in SET clause");
        if (peek().type != token_type::identifier) {
            throw std::runtime_error("Expected property name in SET clause");
        }
        asgn.property = advance().text;
        consume(token_type::equals, "Expected '=' in SET clause");
        asgn.value = parse_expression();
        c->assignments.push_back(asgn);
    } while (match(token_type::comma));
    return c;
}

pattern_path parser::parse_pattern_path() {
    pattern_path path;

    // First node
    pattern_element elem;
    elem.node = parse_node_pattern();
    path.elements.push_back(elem);

    // Relationships
    while (true) {
        if (peek().type == token_type::dash || peek().type == token_type::arrow_left) {
            auto rel = parse_relationship_pattern();
            auto next_node = parse_node_pattern();
            path.elements.back().relationship = rel;

            pattern_element next_elem;
            next_elem.node = next_node;
            path.elements.push_back(next_elem);
        } else {
            break;
        }
    }
    return path;
}

std::shared_ptr<pattern_node> parser::parse_node_pattern() {
    consume(token_type::lparen, "Expected '(' for node");
    auto node = std::make_shared<pattern_node>();

    if (peek().type == token_type::identifier) {
        node->variable = advance().text;
    }

    if (match(token_type::colon)) {
        if (peek().type == token_type::identifier) {
            node->label = advance().text;
        }
    }

    if (peek().type == token_type::lbrace) {
        node->properties = parse_properties_map();
    }

    consume(token_type::rparen, "Expected ')' for node");
    return node;
}

std::shared_ptr<pattern_relationship> parser::parse_relationship_pattern() {
    auto rel = std::make_shared<pattern_relationship>();
    bool left_arrow = match(token_type::arrow_left); // <-

    if (!left_arrow) {
        match(token_type::dash); // -
    }

    if (match(token_type::lbracket)) {
        if (peek().type == token_type::identifier) {
            rel->variable = advance().text;
        }
        if (match(token_type::colon)) {
            if (peek().type == token_type::identifier) {
                rel->type = advance().text;
            }
        }
        if (peek().type == token_type::lbrace) {
            rel->properties = parse_properties_map();
        }
        consume(token_type::rbracket, "Expected ']'");
    }

    bool right_arrow = match(token_type::arrow_right); // ->
    if (!right_arrow) {
        match(token_type::dash); // -
    }

    if (left_arrow) rel->direction = "<-";
    else if (right_arrow) rel->direction = "->";
    else rel->direction = "-";

    return rel;
}

std::map<std::string, std::shared_ptr<expression>> parser::parse_properties_map() {
    std::map<std::string, std::shared_ptr<expression>> props;
    consume(token_type::lbrace, "Expected '{'");
    while (peek().type != token_type::rbrace) {
        std::string key = consume(token_type::identifier, "Expected property key").text;
        consume(token_type::colon, "Expected ':'");
        props[key] = parse_expression();
        if (!match(token_type::comma)) break;
    }
    consume(token_type::rbrace, "Expected '}'");
    return props;
}

std::shared_ptr<expression> parser::parse_expression() {
    // List literal: [expr, expr, ...]
    if (peek().type == token_type::lbracket) {
        advance(); // consume '['
        auto list = std::make_shared<list_literal_expr>();
        while (peek().type != token_type::rbracket && peek().type != token_type::eof) {
            list->elements.push_back(parse_expression());
            if (!match(token_type::comma)) break;
        }
        consume(token_type::rbracket, "Expected ']' in list literal");
        return list;
    }

    // Map literal: {key: expr, ...}
    if (peek().type == token_type::lbrace) {
        advance(); // consume '{'
        auto map = std::make_shared<map_literal_expr>();
        while (peek().type != token_type::rbrace && peek().type != token_type::eof) {
            std::string key = consume(token_type::identifier, "Expected key in map literal").text;
            consume(token_type::colon, "Expected ':' in map literal");
            map->entries[key] = parse_expression();
            if (!match(token_type::comma)) break;
        }
        consume(token_type::rbrace, "Expected '}' in map literal");
        return map;
    }

    if (peek().type == token_type::identifier) {
        std::string name = advance().text;
        // Check for property access: var.prop
        if (peek().type == token_type::dot) {
            advance(); // consume dot
            if (peek().type == token_type::identifier) {
                std::string prop = advance().text;
                return std::make_shared<property_access>(name, prop);
            }
            throw std::runtime_error("Expected property name after '.'");
        }
        return std::make_shared<variable>(name);
    } else if (peek().type == token_type::string_literal) {
        return std::make_shared<literal>(advance().text, true);
    } else if (peek().type == token_type::number_literal) {
        return std::make_shared<literal>(advance().text, false);
    }
    throw std::runtime_error("Unexpected expression token: " + peek().text);
}

std::shared_ptr<unwind_clause> parser::parse_unwind() {
    auto c = std::make_shared<unwind_clause>();
    c->list_expr = parse_expression();
    consume(token_type::keyword_as, "Expected AS in UNWIND clause");
    c->alias = consume(token_type::identifier, "Expected alias in UNWIND clause").text;
    return c;
}

} // namespace tateyama::framework::graph::core
