#include <tateyama/framework/graph/cypher_parser.h>

namespace tateyama::framework::graph {

std::map<std::string, std::string> cypher_parser::parse_properties(const std::string& props_str) {
    std::map<std::string, std::string> props;
    // Basic parser for {key: 'val', ...}
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
    // CREATE (n:Label {prop: val})
    std::regex create_pattern(R"(CREATE\s*\(\w+:(\w+)\s*(\{.*\})\))");
    std::smatch match;
    if (std::regex_search(query, match, create_pattern)) {
        if (match.size() > 2) {
            return { command_type::create_node, match[1].str(), parse_properties(match[2].str()) };
        }
    }

    // MATCH (n) RETURN n
    if (query.find("MATCH (n) RETURN n") != std::string::npos) {
        return { command_type::match_node, "", {} };
    }

    return { command_type::unknown, "", {} };
}

} // namespace tateyama::framework::graph
