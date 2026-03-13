#include <tateyama/framework/graph/core/executor.h>
#include <sstream>
#include <iostream>

namespace tateyama::framework::graph::core {

bool executor::execute(const statement& stmt, std::string& result_json) {
    for (const auto& clause : stmt.clauses) {
        if (clause->type() == clause_type::create) {
            if (!execute_create(std::dynamic_pointer_cast<create_clause>(clause))) return false;
        } else if (clause->type() == clause_type::match) {
            if (!execute_match(std::dynamic_pointer_cast<match_clause>(clause))) return false;
        } else if (clause->type() == clause_type::return_clause) {
            if (!execute_return(std::dynamic_pointer_cast<return_clause>(clause), result_json)) return false;
        }
    }
    return true;
}

std::string executor::evaluate_properties(const std::map<std::string, std::shared_ptr<expression>>& props) {
    std::stringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [key, expr] : props) {
        if (!first) ss << ", ";
        first = false;
        ss << "\"" << key << "\": ";
        if (expr->type() == node_type::literal_string) {
            auto lit = std::dynamic_pointer_cast<literal>(expr);
            ss << "\"" << lit->value << "\"";
        } else if (expr->type() == node_type::literal_number) {
            auto lit = std::dynamic_pointer_cast<literal>(expr);
            ss << lit->value;
        }
        // TODO: handle variables
    }
    ss << "}";
    return ss.str();
}

bool executor::execute_create(const std::shared_ptr<create_clause>& create) {
    for (const auto& path : create->paths) {
        uint64_t last_node_id = 0;
        
        for (const auto& elem : path.elements) {
            // Process Node
            uint64_t node_id = 0;
            // Check if variable exists in context
            if (!elem.node->variable.empty() && context_.count(elem.node->variable)) {
                node_id = context_[elem.node->variable];
            } else {
                // Create new node
                std::string props = evaluate_properties(elem.node->properties);
                // TODO: Store label as well
                if (!store_.create_node(tx_, props, node_id)) return false;
                if (!elem.node->variable.empty()) {
                    context_[elem.node->variable] = node_id;
                }
            }

            // Process Relationship (if exists and connected to previous node)
            if (last_node_id != 0 && elem.relationship) { // This element has a relationship pointing to... wait, logic is tricky
                // pattern_path structure: Node, Rel, Node
                // In my AST: Element has Node and Optional Rel TO NEXT Node?
                // Actually my AST structure was: vector<element>. element = node + optional rel.
                // It means: (Node1)-[Rel1]->(Node2)-[Rel2]->...
                // So when processing elem N, we look at elem N-1's relationship?
                // Let's re-check AST logic.
                // parser::parse_pattern_path:
                //   elem1 (Node1) pushed.
                //   if rel:
                //     rel parsed.
                //     elem2 (Node2) parsed.
                //     elem1.relationship = rel.
                //     elem2 pushed.
                // So, if we are at elem2, we need to look back at elem1 to see if there is a relationship connecting elem1 and elem2.
            }
        }

        // Re-iterating with index to handle relationships properly
        for (size_t i = 0; i < path.elements.size(); ++i) {
            auto& elem = path.elements[i];
            
            // Get or Create Node
            uint64_t node_id = 0;
            if (!elem.node->variable.empty() && context_.count(elem.node->variable)) {
                node_id = context_[elem.node->variable];
            } else {
                std::string props = evaluate_properties(elem.node->properties);
                if (!store_.create_node(tx_, props, node_id)) return false;
                if (!elem.node->variable.empty()) context_[elem.node->variable] = node_id;
            }

            // If there is a relationship from this node to the next
            if (elem.relationship && i + 1 < path.elements.size()) {
                 auto& next_elem = path.elements[i+1];
                 
                 // Ensure next node is created/resolved (it will be in next iteration, but we need ID now)
                 // This implies we should resolve all nodes first or resolve lazily?
                 // Cypher CREATE (a)-[:REL]->(b) creates both a and b.
                 // So we must resolve next node now.
                 
                 uint64_t next_node_id = 0;
                 if (!next_elem.node->variable.empty() && context_.count(next_elem.node->variable)) {
                     next_node_id = context_[next_elem.node->variable];
                 } else {
                     std::string props = evaluate_properties(next_elem.node->properties);
                     if (!store_.create_node(tx_, props, next_node_id)) return false;
                     if (!next_elem.node->variable.empty()) context_[next_elem.node->variable] = next_node_id;
                 }

                 // Create Edge
                 uint64_t edge_id;
                 std::string rel_props = evaluate_properties(elem.relationship->properties);
                 
                 // Direction handling
                 uint64_t from = node_id;
                 uint64_t to = next_node_id;
                 if (elem.relationship->direction == "<-") {
                     std::swap(from, to);
                 }
                 
                 if (!store_.create_edge(tx_, from, to, elem.relationship->type, rel_props, edge_id)) return false;
                 if (!elem.relationship->variable.empty()) context_[elem.relationship->variable] = edge_id;
            }
        }
    }
    return true;
}

bool executor::execute_match(const std::shared_ptr<match_clause>& match) {
    // Very simplified MATCH implementation: Full scan or scan all edges?
    // Prototype: Support MATCH (n) only (Scan all nodes)
    // Or MATCH (n)-[r]->(m)
    
    // For now, let's implement just iterating all nodes and binding to variable if it's a simple MATCH (n)
    // Real implementation needs a query planner.
    
    return true; 
}

bool executor::execute_return(const std::shared_ptr<return_clause>& ret, std::string& result_json) {
    // Retrieve values from context and storage
    std::stringstream ss;
    ss << "[";
    
    // Just dump one result for now (context state)
    // Real Return needs iteration over result set (Match results)
    
    ss << "{";
    bool first = true;
    for (const auto& item : ret->items) {
        if (!first) ss << ", ";
        first = false;
        
        std::string alias = item.alias;
        
        if (item.expr->type() == node_type::variable) {
            auto var = std::dynamic_pointer_cast<variable>(item.expr);
            if (alias.empty()) alias = var->name;
            
            ss << "\"" << alias << "\": ";
            if (context_.count(var->name)) {
                uint64_t id = context_[var->name];
                // Fetch from storage to get properties
                std::string props;
                if (store_.get_node(tx_, id, props)) {
                     ss << props; // Dump raw json properties
                } else {
                     ss << "null";
                }
            } else {
                ss << "null";
            }
        }
    }
    ss << "}";
    
    ss << "]";
    result_json = ss.str();
    return true;
}

} // namespace tateyama::framework::graph::core
