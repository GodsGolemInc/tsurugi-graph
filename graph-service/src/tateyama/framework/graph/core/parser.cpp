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
        if (isalpha(c)) {
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

token lexer::scan_identifier_or_keyword() {
    size_t start = pos_;
    while (!is_eof() && (isalnum(peek()) || peek() == '_')) {
        advance();
    }
    std::string text = input_.substr(start, pos_ - start);
    token_type type = token_type::identifier;
    
    std::string upper_text = text;
    std::transform(upper_text.begin(), upper_text.end(), upper_text.begin(), ::toupper);

    if (upper_text == "CREATE") type = token_type::keyword_create;
    else if (upper_text == "MATCH") type = token_type::keyword_match;
    else if (upper_text == "RETURN") type = token_type::keyword_return;
    else if (upper_text == "WHERE") type = token_type::keyword_where;

    return {type, text, start};
}

token lexer::scan_string() {
    size_t start = pos_;
    char quote = advance();
    while (!is_eof() && peek() != quote) {
        advance();
    }
    if (!is_eof()) advance(); // consume closing quote
    // Include quotes in text for simplicity now, or strip them
    return {token_type::string_literal, input_.substr(start, pos_ - start), start};
}

token lexer::scan_number() {
    size_t start = pos_;
    while (!is_eof() && isdigit(peek())) {
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
        case '-': 
            if (peek() == '>') {
                advance();
                return {token_type::arrow_right, "->", start};
            }
            return {token_type::dash, "-", start};
        case '<':
            if (peek() == '-') {
                advance();
                return {token_type::arrow_left, "<-", start};
            }
            // default fallback?
            break;
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
    return tokens_[pos_];
}

token parser::advance() {
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
        // Optional alias 'AS' ... (skip for now)
        c->items.push_back(item);
    } while (match(token_type::comma));
    return c;
}

std::shared_ptr<where_clause> parser::parse_where() {
    auto c = std::make_shared<where_clause>();
    // Simplistic parsing: var.prop = val
    auto expr = parse_expression();
    if (auto prop_access = std::dynamic_pointer_cast<property_access>(expr)) {
        c->variable = prop_access->variable_name;
        c->property = prop_access->property_key;
        
        // Assume = for now
        // TODO: parse operator
        if (peek().text == "=") { // Should check token type properly if we had operator tokens
             advance(); // consume =
             c->op = "=";
        } else {
             // fallback for prototype
             c->op = "=";
        }
        
        c->value = parse_expression();
    }
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
            // Found start of relationship: -[...]-> or <-[...]-, or just --
            auto rel = parse_relationship_pattern();
            
            // Next node
            auto next_node = parse_node_pattern();
            
            // Link previous element to relationship
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
    bool has_dash = false;
    
    if (!left_arrow) {
        has_dash = match(token_type::dash); // -
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
    // Very simplified
    if (peek().type == token_type::identifier) {
        std::string var = advance().text;
        if (match(token_type::unknown) /* . */) { 
            // NOTE: Lexer currently returns unknown for dot. 
            // We should fix lexer for dot if we want strictness.
            // For now assuming dot is unknown char or part of identifier if simplified.
            // Let's assume we implement property access properly later.
        }
        return std::make_shared<variable>(var);
    } else if (peek().type == token_type::string_literal) {
        return std::make_shared<literal>(advance().text, true);
    } else if (peek().type == token_type::number_literal) {
        return std::make_shared<literal>(advance().text, false);
    }
    throw std::runtime_error("Unexpected expression token: " + peek().text);
}

} // namespace tateyama::framework::graph::core
