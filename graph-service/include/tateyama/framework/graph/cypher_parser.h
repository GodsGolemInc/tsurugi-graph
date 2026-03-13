#pragma once

#include <string>
#include <vector>
#include <map>
#include <regex>

namespace tateyama::framework::graph {

class cypher_parser {
public:
    enum class command_type {
        create_node,
        create_edge,
        match_node,
        unknown
    };

    struct result {
        command_type type;
        std::string label;
        std::map<std::string, std::string> properties;
        std::string from_var;
        std::string to_var;
    };

    result parse(const std::string& query);

private:
    std::map<std::string, std::string> parse_properties(const std::string& props_str);
};

} // namespace tateyama::framework::graph
