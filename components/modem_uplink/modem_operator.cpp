#include "modem_operator.hpp"

#include <cctype>
#include <cstdlib>
#include <vector>

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

namespace
{

// "ddmm.mmmm" (degrees, then minutes) as decimal degrees; false if it is not
// one. `degree_digits`: 2 for the latitude, 3 for the longitude.
bool degrees_minutes(const std::string &text, int degree_digits, double &out)
{
    if (text.size() < (std::size_t)degree_digits + 3) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') ||
              (c == '.' && i >= (std::size_t)degree_digits))) {
            return false;
        }
    }
    int degrees = 0;
    for (int i = 0; i < degree_digits; i++) {
        degrees = degrees * 10 + (text[i] - '0');
    }
    char *end = nullptr;
    const double minutes = std::strtod(text.c_str() + degree_digits, &end);
    if (end == nullptr || *end != '\0' || minutes >= 60.0) {
        return false;
    }
    out = degrees + minutes / 60.0;
    return true;
}

}  // namespace

bool parse_gnss_info(const std::string &answer, double &latitude,
                     double &longitude)
{
    const std::size_t tag = answer.find("+CGNSSINFO:");
    if (tag == std::string::npos) {
        return false;
    }
    std::size_t end = answer.find_first_of("\r\n", tag);
    if (end == std::string::npos) {
        end = answer.size();
    }
    std::string line = answer.substr(tag + 11, end - tag - 11);
    std::vector<std::string> fields;
    std::string field;
    for (char c : line) {
        if (c == ',') {
            fields.push_back(field);
            field.clear();
        } else if (c != ' ') {
            field += c;
        }
    }
    fields.push_back(field);
    if (fields.size() < 8) {
        return false;
    }
    double lat = 0;
    double lon = 0;
    if (!degrees_minutes(fields[4], 2, lat) ||
        !degrees_minutes(fields[6], 3, lon)) {
        return false;
    }
    if ((fields[5] != "N" && fields[5] != "S") ||
        (fields[7] != "E" && fields[7] != "W")) {
        return false;
    }
    if (lat > 90.0 || lon > 180.0) {
        return false;
    }
    latitude = fields[5] == "S" ? -lat : lat;
    longitude = fields[7] == "W" ? -lon : lon;
    return true;
}

}  // namespace modem_uplink
