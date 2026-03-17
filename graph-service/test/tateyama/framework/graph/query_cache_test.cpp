#include <tateyama/framework/graph/core/parser.h>
#include <tateyama/framework/graph/core/query_cache.h>
#include <iostream>
#include <cassert>
#include <cstdlib>

using namespace tateyama::framework::graph::core;

// ======== normalize_query tests ========

void test_normalize_string_literal() {
    std::vector<std::string> lits;
    auto result = normalize_query("MATCH (n:Person) WHERE n.name = 'Alice'", lits);
    assert(result.find("$0") != std::string::npos);
    assert(result.find("Alice") == std::string::npos);
    assert(lits.size() == 1);
    assert(lits[0] == "Alice");
    std::cout << "test_normalize_string_literal: PASS" << std::endl;
}

void test_normalize_number_literal() {
    std::vector<std::string> lits;
    auto result = normalize_query("MATCH (n:Person) WHERE n.age > 30", lits);
    assert(result.find("$") != std::string::npos);
    assert(result.find("30") == std::string::npos);
    bool found_30 = false;
    for (auto& l : lits) { if (l == "30") found_30 = true; }
    assert(found_30);
    std::cout << "test_normalize_number_literal: PASS" << std::endl;
}

void test_normalize_mixed_literals() {
    std::vector<std::string> lits;
    auto result = normalize_query("CREATE (n:Person {name: 'Bob', age: 25})", lits);
    assert(lits.size() >= 2);
    bool has_bob = false, has_25 = false;
    for (auto& l : lits) {
        if (l == "Bob") has_bob = true;
        if (l == "25") has_25 = true;
    }
    assert(has_bob);
    assert(has_25);
    std::cout << "test_normalize_mixed_literals: PASS" << std::endl;
}

void test_normalize_no_literals() {
    std::vector<std::string> lits;
    auto result = normalize_query("MATCH (n) RETURN n", lits);
    assert(lits.empty());
    assert(result == "MATCH (n) RETURN n");
    std::cout << "test_normalize_no_literals: PASS" << std::endl;
}

void test_normalize_double_quote_string() {
    std::vector<std::string> lits;
    auto result = normalize_query("MATCH (n) WHERE n.name = \"Alice\"", lits);
    assert(lits.size() >= 1);
    assert(lits[0] == "Alice");
    std::cout << "test_normalize_double_quote_string: PASS" << std::endl;
}

// ======== normalize_query idempotence (property test) ========

void test_normalize_same_pattern_same_key() {
    // Property: two queries with same structure but different literals
    // should produce the same normalized key
    std::vector<std::string> lits1, lits2;
    auto norm1 = normalize_query("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n", lits1);
    auto norm2 = normalize_query("MATCH (n:Person) WHERE n.name = 'Bob' RETURN n", lits2);
    assert(norm1 == norm2);
    assert(lits1[0] == "Alice");
    assert(lits2[0] == "Bob");
    std::cout << "test_normalize_same_pattern_same_key: PASS" << std::endl;
}

// ======== visit_literals / extract_literals tests ========

void test_extract_literals_where() {
    lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    std::vector<std::string> lits;
    extract_literals(stmt, lits);
    assert(!lits.empty());
    bool found = false;
    for (auto& v : lits) { if (v == "Alice") found = true; }
    assert(found);
    std::cout << "test_extract_literals_where: PASS" << std::endl;
}

void test_extract_literals_create() {
    lexer l("CREATE (n:Person {name: 'Carol', age: 28})");
    parser p(l.tokenize());
    auto stmt = p.parse();

    std::vector<std::string> lits;
    extract_literals(stmt, lits);
    assert(lits.size() >= 2);
    std::cout << "test_extract_literals_create: PASS" << std::endl;
}

void test_extract_literals_set() {
    lexer l("MATCH (n:Person) SET n.age = 40 RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    std::vector<std::string> lits;
    extract_literals(stmt, lits);
    bool found_40 = false;
    for (auto& v : lits) { if (v == "40") found_40 = true; }
    assert(found_40);
    std::cout << "test_extract_literals_set: PASS" << std::endl;
}

void test_visit_literals_counts() {
    lexer l("CREATE (n:Person {name: 'X', age: 10})");
    parser p(l.tokenize());
    auto stmt = p.parse();

    int count = 0;
    visit_literals(stmt, [&](literal*) { count++; });
    assert(count >= 2);
    std::cout << "test_visit_literals_counts: PASS" << std::endl;
}

// ======== ast_cache_key tests ========

void test_ast_cache_key_same_pattern() {
    lexer l1("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
    parser p1(l1.tokenize());
    auto stmt1 = p1.parse();

    lexer l2("MATCH (n:Person) WHERE n.name = 'Bob' RETURN n");
    parser p2(l2.tokenize());
    auto stmt2 = p2.parse();

    auto key1 = ast_cache_key(stmt1);
    auto key2 = ast_cache_key(stmt2);
    assert(key1 == key2);
    std::cout << "test_ast_cache_key_same_pattern: PASS" << std::endl;
}

void test_ast_cache_key_different_pattern() {
    lexer l1("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
    parser p1(l1.tokenize());
    auto stmt1 = p1.parse();

    lexer l2("MATCH (n:Company) WHERE n.name = 'Acme' RETURN n");
    parser p2(l2.tokenize());
    auto stmt2 = p2.parse();

    auto key1 = ast_cache_key(stmt1);
    auto key2 = ast_cache_key(stmt2);
    assert(key1 != key2);
    std::cout << "test_ast_cache_key_different_pattern: PASS" << std::endl;
}

// ======== bind_literals tests ========

void test_bind_literals_where() {
    lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    bind_literals(stmt, {"Charlie"});

    std::vector<std::string> lits;
    extract_literals(stmt, lits);
    bool found = false;
    for (auto& v : lits) { if (v == "Charlie") found = true; }
    assert(found);
    std::cout << "test_bind_literals_where: PASS" << std::endl;
}

void test_bind_extract_roundtrip() {
    // Property test: bind values, then extract should return the same values
    lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    std::vector<std::string> new_vals = {"NewValue"};
    bind_literals(stmt, new_vals);

    std::vector<std::string> extracted;
    extract_literals(stmt, extracted);
    assert(!extracted.empty());
    assert(extracted[0] == "NewValue");
    std::cout << "test_bind_extract_roundtrip: PASS" << std::endl;
}

// ======== clone_expr tests ========

void test_clone_literal() {
    auto orig = std::make_shared<literal>("hello", true);
    auto copy = clone_expr(orig);
    assert(copy->type() == node_type::literal_string);
    auto* lit = static_cast<literal*>(copy.get());
    assert(lit->value == "hello");
    assert(lit->is_string == true);
    // Modify copy, original unchanged
    lit->value = "world";
    assert(static_cast<literal*>(orig.get())->value == "hello");
    std::cout << "test_clone_literal: PASS" << std::endl;
}

void test_clone_variable() {
    auto orig = std::make_shared<variable>("n");
    auto copy = clone_expr(orig);
    assert(copy->type() == node_type::variable);
    assert(static_cast<variable*>(copy.get())->name == "n");
    std::cout << "test_clone_variable: PASS" << std::endl;
}

void test_clone_property_access() {
    auto orig = std::make_shared<property_access>("n", "name");
    auto copy = clone_expr(orig);
    assert(copy->type() == node_type::property_access);
    auto* pa = static_cast<property_access*>(copy.get());
    assert(pa->variable_name == "n");
    assert(pa->property_key == "name");
    std::cout << "test_clone_property_access: PASS" << std::endl;
}

void test_clone_binary() {
    auto left = std::make_shared<property_access>("n", "age");
    auto right = std::make_shared<literal>("30", false);
    auto orig = std::make_shared<binary_expression>(left, ">", right);
    auto copy = clone_expr(orig);
    assert(copy->type() == node_type::binary);
    auto* bin = static_cast<binary_expression*>(copy.get());
    assert(bin->op == ">");
    // Modify copy, original unchanged
    static_cast<literal*>(bin->right.get())->value = "50";
    assert(static_cast<literal*>(right.get())->value == "30");
    std::cout << "test_clone_binary: PASS" << std::endl;
}

void test_clone_list_literal() {
    auto orig = std::make_shared<list_literal_expr>();
    orig->elements.push_back(std::make_shared<literal>("a", true));
    orig->elements.push_back(std::make_shared<literal>("b", true));
    auto copy = clone_expr(orig);
    assert(copy->type() == node_type::list_literal);
    auto* ll = static_cast<list_literal_expr*>(copy.get());
    assert(ll->elements.size() == 2);
    // Modify copy
    static_cast<literal*>(ll->elements[0].get())->value = "x";
    assert(static_cast<literal*>(orig->elements[0].get())->value == "a");
    std::cout << "test_clone_list_literal: PASS" << std::endl;
}

void test_clone_map_literal() {
    auto orig = std::make_shared<map_literal_expr>();
    orig->entries["key1"] = std::make_shared<literal>("val1", true);
    auto copy = clone_expr(orig);
    assert(copy->type() == node_type::map_literal);
    auto* ml = static_cast<map_literal_expr*>(copy.get());
    assert(ml->entries.size() == 1);
    assert(static_cast<literal*>(ml->entries["key1"].get())->value == "val1");
    std::cout << "test_clone_map_literal: PASS" << std::endl;
}

void test_clone_null() {
    auto copy = clone_expr(nullptr);
    assert(copy == nullptr);
    std::cout << "test_clone_null: PASS" << std::endl;
}

// ======== clone_props tests ========

void test_clone_props() {
    std::map<std::string, std::shared_ptr<expression>> props;
    props["name"] = std::make_shared<literal>("Alice", true);
    props["age"] = std::make_shared<literal>("30", false);

    auto copy = clone_props(props);
    assert(copy.size() == 2);
    assert(static_cast<literal*>(copy["name"].get())->value == "Alice");

    // Modify copy, original unchanged
    static_cast<literal*>(copy["name"].get())->value = "Bob";
    assert(static_cast<literal*>(props["name"].get())->value == "Alice");
    std::cout << "test_clone_props: PASS" << std::endl;
}

// ======== clone_path tests ========

void test_clone_path() {
    pattern_path path;
    pattern_element elem;
    elem.node = std::make_shared<pattern_node>();
    elem.node->variable = "n";
    elem.node->label = "Person";
    elem.node->properties["name"] = std::make_shared<literal>("Alice", true);
    elem.relationship = std::make_shared<pattern_relationship>();
    elem.relationship->type = "KNOWS";
    elem.relationship->direction = "->";
    path.elements.push_back(elem);

    auto copy = clone_path(path);
    assert(copy.elements.size() == 1);
    assert(copy.elements[0].node->variable == "n");
    assert(copy.elements[0].node->label == "Person");
    assert(copy.elements[0].relationship->type == "KNOWS");

    // Modify copy, original unchanged
    copy.elements[0].node->label = "Company";
    assert(path.elements[0].node->label == "Person");
    std::cout << "test_clone_path: PASS" << std::endl;
}

// ======== deep_copy_statement tests ========

void test_deep_copy_create() {
    lexer l("CREATE (n:Person {name: 'Alice'})");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto copy = deep_copy_statement(stmt);
    assert(copy->clauses.size() == 1);
    assert(copy->clauses[0]->type() == clause_type::create);

    // Modify copy literal, original unchanged
    std::vector<std::string> orig_lits, copy_lits;
    extract_literals(stmt, orig_lits);
    bind_literals(*copy, {"Bob"});
    extract_literals(stmt, copy_lits);
    assert(copy_lits[0] == orig_lits[0]); // Original unmodified
    std::cout << "test_deep_copy_create: PASS" << std::endl;
}

void test_deep_copy_where() {
    lexer l("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto copy = deep_copy_statement(stmt);
    assert(copy->clauses.size() == 3);

    // Verify WHERE value and condition->right are shared
    auto* wc = static_cast<where_clause*>(copy->clauses[1].get());
    if (auto bin = std::dynamic_pointer_cast<binary_expression>(wc->condition)) {
        assert(wc->value.get() == bin->right.get()); // same object
    }

    // Modify copy, original unchanged
    bind_literals(*copy, {"Bob"});
    std::vector<std::string> orig_lits;
    extract_literals(stmt, orig_lits);
    assert(orig_lits[0] == "Alice");
    std::cout << "test_deep_copy_where: PASS" << std::endl;
}

void test_deep_copy_set() {
    lexer l("MATCH (n:Person) SET n.age = 31 RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto copy = deep_copy_statement(stmt);
    bind_literals(*copy, {"99"});

    std::vector<std::string> orig_lits;
    extract_literals(stmt, orig_lits);
    bool has_31 = false;
    for (auto& v : orig_lits) { if (v == "31") has_31 = true; }
    assert(has_31);
    std::cout << "test_deep_copy_set: PASS" << std::endl;
}

void test_deep_copy_delete() {
    lexer l("MATCH (n:Person) DELETE n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto copy = deep_copy_statement(stmt);
    assert(copy->clauses.size() == 2);
    auto* dc = static_cast<delete_clause*>(copy->clauses[1].get());
    assert(dc->variables.size() == 1);
    assert(dc->variables[0] == "n");
    assert(!dc->detach);
    std::cout << "test_deep_copy_delete: PASS" << std::endl;
}

void test_deep_copy_match() {
    lexer l("MATCH (n:Person) RETURN n");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto copy = deep_copy_statement(stmt);
    assert(copy->clauses.size() == 2);
    assert(copy->clauses[0]->type() == clause_type::match);
    auto* mc = static_cast<match_clause*>(copy->clauses[0].get());
    assert(mc->paths.size() == 1);
    std::cout << "test_deep_copy_match: PASS" << std::endl;
}

void test_deep_copy_return() {
    lexer l("MATCH (n:Person) RETURN n.name AS name");
    parser p(l.tokenize());
    auto stmt = p.parse();

    auto copy = deep_copy_statement(stmt);
    auto* rc = static_cast<return_clause*>(copy->clauses[1].get());
    assert(rc->items.size() == 1);
    assert(rc->items[0].alias == "name");
    std::cout << "test_deep_copy_return: PASS" << std::endl;
}

// ======== deep copy isolation property test ========

void test_deep_copy_isolation() {
    // Property: modifying a deep copy must not affect the original
    std::string queries[] = {
        "CREATE (n:Person {name: 'Alice', age: 30})",
        "MATCH (n:Person) WHERE n.name = 'Alice' RETURN n",
        "MATCH (n:Person) SET n.age = 40 RETURN n",
    };
    for (auto& q : queries) {
        lexer l(q);
        parser p(l.tokenize());
        auto stmt = p.parse();

        std::vector<std::string> orig_lits;
        extract_literals(stmt, orig_lits);

        auto copy = deep_copy_statement(stmt);
        // Bind different values into copy
        std::vector<std::string> new_vals(orig_lits.size(), "CHANGED");
        bind_literals(*copy, new_vals);

        // Original must be unchanged
        std::vector<std::string> check_lits;
        extract_literals(stmt, check_lits);
        assert(check_lits.size() == orig_lits.size());
        for (size_t i = 0; i < orig_lits.size(); ++i) {
            assert(check_lits[i] == orig_lits[i]);
        }
    }
    std::cout << "test_deep_copy_isolation: PASS" << std::endl;
}

// ======== query_cache class tests ========

void test_cache_put_get() {
    query_cache cache;
    lexer l("CREATE (n:Person {name: 'Alice'})");
    parser p(l.tokenize());
    auto stmt = std::make_shared<statement>(p.parse());

    cache.put("query1", stmt);
    assert(cache.size() == 1);

    auto result = cache.get("query1");
    assert(result != nullptr);
    assert(result.get() == stmt.get());

    auto miss = cache.get("nonexistent");
    assert(miss == nullptr);
    std::cout << "test_cache_put_get: PASS" << std::endl;
}

void test_cache_clear() {
    query_cache cache;
    auto stmt = std::make_shared<statement>();
    cache.put("q1", stmt);
    cache.put("q2", stmt);
    assert(cache.size() == 2);

    cache.clear();
    assert(cache.size() == 0);
    assert(cache.get("q1") == nullptr);
    std::cout << "test_cache_clear: PASS" << std::endl;
}

void test_cache_size() {
    query_cache cache;
    assert(cache.size() == 0);
    auto stmt = std::make_shared<statement>();
    cache.put("q1", stmt);
    assert(cache.size() == 1);
    cache.put("q2", stmt);
    assert(cache.size() == 2);
    // Overwrite
    cache.put("q1", stmt);
    assert(cache.size() == 2);
    std::cout << "test_cache_size: PASS" << std::endl;
}

void test_cache_lru_eviction() {
    query_cache cache(3); // max 3 entries
    auto stmt = std::make_shared<statement>();

    cache.put("q1", stmt);
    cache.put("q2", stmt);
    cache.put("q3", stmt);
    assert(cache.size() == 3);

    // Access q1 to make it recently used
    cache.get("q1");

    // Add q4 - should evict q2 (oldest unused)
    cache.put("q4", stmt);
    assert(cache.size() == 3);
    assert(cache.get("q1") != nullptr); // still present
    assert(cache.get("q2") == nullptr); // evicted
    assert(cache.get("q3") != nullptr); // still present
    assert(cache.get("q4") != nullptr); // newly added
    std::cout << "test_cache_lru_eviction: PASS" << std::endl;
}

void test_cache_update_existing() {
    query_cache cache;
    auto stmt1 = std::make_shared<statement>();
    auto stmt2 = std::make_shared<statement>();

    cache.put("q1", stmt1);
    cache.put("q1", stmt2);
    assert(cache.size() == 1);
    assert(cache.get("q1").get() == stmt2.get());
    std::cout << "test_cache_update_existing: PASS" << std::endl;
}

int main() {
    // normalize_query
    test_normalize_string_literal();
    test_normalize_number_literal();
    test_normalize_mixed_literals();
    test_normalize_no_literals();
    test_normalize_double_quote_string();
    test_normalize_same_pattern_same_key();

    // visit_literals / extract_literals
    test_extract_literals_where();
    test_extract_literals_create();
    test_extract_literals_set();
    test_visit_literals_counts();

    // ast_cache_key
    test_ast_cache_key_same_pattern();
    test_ast_cache_key_different_pattern();

    // bind_literals
    test_bind_literals_where();
    test_bind_extract_roundtrip();

    // clone_expr (all types)
    test_clone_literal();
    test_clone_variable();
    test_clone_property_access();
    test_clone_binary();
    test_clone_list_literal();
    test_clone_map_literal();
    test_clone_null();

    // clone_props
    test_clone_props();

    // clone_path
    test_clone_path();

    // deep_copy_statement (all clause types)
    test_deep_copy_create();
    test_deep_copy_where();
    test_deep_copy_set();
    test_deep_copy_delete();
    test_deep_copy_match();
    test_deep_copy_return();

    // Property tests
    test_deep_copy_isolation();

    // query_cache class
    test_cache_put_get();
    test_cache_clear();
    test_cache_size();
    test_cache_lru_eviction();
    test_cache_update_existing();

    std::cout << "\nAll query_cache tests passed!" << std::endl;
    return 0;
}
