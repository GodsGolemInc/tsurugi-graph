#include <tateyama/framework/graph/cypher_parser.h>

namespace tateyama::framework::graph {

std::map<std::string, std::string> cypher_parser::parse_properties(const std::string& props_str) {
    std::map<std::string, std::string> props;
    std::regex prop_regex(R"((\w+):\s*('[^']*'|"[^"]*"|\w+))");
    auto words_begin = std::sregex_iterator(props_str.begin(), props_str.end(), prop_regex);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        props[match[1].str()] = match[2].str();
    }
    return props;
}

cypher_parser::result cypher_parser::parse(const std::string& query) {
    // 1. CREATE Edge: (n1)-[r:LABEL {props}]->(n2)
    std::regex edge_pattern(R"(CREATE\s*\((\w+)\)-\[(\w+):(\w+)\s*(\{.*\})\]->\((\w+)\))");
    std::smatch match;
    if (std::regex_search(query, match, edge_pattern)) {
        if (match.size() > 5) {
            result res;
            res.type = command_type::create_edge;
            res.from_var = match[1].str();
            // res.edge_var = match[2].str();
            res.label = match[3].str();
            res.properties = parse_properties(match[4].str());
            res.to_var = match[5].str();
            return res;
        }
    }

    // 2. CREATE Node: (n:Label {prop: val})
    std::regex node_pattern(R"(CREATE\s*\(\w+:(\w+)\s*(\{.*\})\))");
    if (std::regex_search(query, match, node_pattern)) {
        if (match.size() > 2) {
            result res;
            res.type = command_type::create_node;
            res.label = match[1].str();
            res.properties = parse_properties(match[2].str());
            return res;
        }
    }

    // 3. MATCH (n) RETURN n
    if (query.find("MATCH (n) RETURN n") != std::string::npos) {
        return { command_type::match_node, "", {}, "", "" };
    }

    return { command_type::unknown, "", {}, "", "" };
}

} // namespace tateyama::framework::graph
