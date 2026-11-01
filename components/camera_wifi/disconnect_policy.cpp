#include "disconnect_policy.hpp"

#include <cstring>

namespace wifipolicy
{

bool is_for_target(const uint8_t *event_ssid, size_t event_ssid_len,
                   const char *target_ssid, size_t target_ssid_len)
{
    if (event_ssid == nullptr || event_ssid_len == 0) {
        return true;
    }
    return event_ssid_len == target_ssid_len && target_ssid != nullptr &&
           std::memcmp(event_ssid, target_ssid, target_ssid_len) == 0;
}

DisconnectAction on_disconnected(bool connect_requested, bool joining,
                                 bool for_target, unsigned retries,
                                 unsigned max_retries)
{
    if (!connect_requested || !for_target) {
        return DisconnectAction::ignore;
    }
    if (!joining) {
        return DisconnectAction::report_lost;
    }
    return retries >= max_retries ? DisconnectAction::give_up
                                  : DisconnectAction::retry;
}

}  // namespace wifipolicy
