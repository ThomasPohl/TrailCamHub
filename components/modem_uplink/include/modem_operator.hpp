#pragma once

// The name of the network the modem is registered with, for the check-in
// (#149). Pure logic without ESP-IDF, host-tested.

#include <string>

namespace modem_uplink
{

// The longest operator name kept, in bytes.
constexpr std::size_t kOperatorNameMax = 32;

// The operator name of an AT+COPS answer as the modem driver hands it over,
// cleaned for the check-in: if the text holds a quoted name (`+COPS:
// 0,0,"Telekom.de",7`), that name; otherwise the text itself. Printable ASCII
// only, trimmed, at most kOperatorNameMax bytes. Empty when the modem gave
// none or only a placeholder.
std::string clean_operator_name(const std::string &raw);

}  // namespace modem_uplink
