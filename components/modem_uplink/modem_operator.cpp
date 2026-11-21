#include "modem_operator.hpp"

#include <cctype>

namespace modem_uplink
{

namespace
{

bool is_placeholder(const std::string &name)
{
    std::string lower;
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    }
    return lower == "unknown" || lower == "n/a" || lower == "none" ||
           lower == "no service" || lower == "no network";
}

}  // namespace

std::string clean_operator_name(const std::string &raw)
{
    std::string text = raw;
    const std::size_t open = raw.find('"');
    if (open != std::string::npos) {
        const std::size_t close = raw.find('"', open + 1);
        text =
            raw.substr(open + 1, close == std::string::npos ? std::string::npos
                                                            : close - open - 1);
    }
    std::string name;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u <= 0x7e) {
            name.push_back(c);
        }
    }
    const std::size_t first = name.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "";
    }
    name = name.substr(first, name.find_last_not_of(' ') - first + 1);
    if (name.size() > kOperatorNameMax) {
        name.resize(kOperatorNameMax);
        const std::size_t end = name.find_last_not_of(' ');
        name.resize(end == std::string::npos ? 0 : end + 1);
    }
    return is_placeholder(name) ? "" : name;
}

}  // namespace modem_uplink
