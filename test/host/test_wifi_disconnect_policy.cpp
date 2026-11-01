#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "disconnect_policy.hpp"

using wifipolicy::DisconnectAction;
using wifipolicy::is_for_target;
using wifipolicy::on_disconnected;

namespace
{

bool event_for(const char *event_ssid, const char *target)
{
    return is_for_target(reinterpret_cast<const uint8_t *>(event_ssid),
                         std::strlen(event_ssid), target, std::strlen(target));
}

}  // namespace

TEST_CASE("wifi disconnect: the event SSID must name the wanted network")
{
    CHECK(event_for("UNSER-WLAN", "UNSER-WLAN"));
    CHECK_FALSE(event_for("TC100_ED3B", "UNSER-WLAN"));
    CHECK_FALSE(event_for("UNSER", "UNSER-WLAN"));
    CHECK_FALSE(event_for("UNSER-WLAN-5G", "UNSER-WLAN"));
    // An event without an SSID cannot be attributed: treat it as ours.
    CHECK(is_for_target(nullptr, 0, "UNSER-WLAN", 10));
    CHECK(event_for("", "UNSER-WLAN"));
}

TEST_CASE("wifi disconnect: a stale camera event does not fail the home join")
{
    // Leaving the camera network and joining the home network right away:
    // the camera's disconnect event arrives while the home join runs.
    const bool for_target = event_for("TC100_ED3B", "UNSER-WLAN");
    CHECK(on_disconnected(true, true, for_target, 0, 5) ==
          DisconnectAction::ignore);
}

TEST_CASE("wifi disconnect: failed join attempts are retried, then given up")
{
    CHECK(on_disconnected(true, true, true, 0, 5) == DisconnectAction::retry);
    CHECK(on_disconnected(true, true, true, 4, 5) == DisconnectAction::retry);
    CHECK(on_disconnected(true, true, true, 5, 5) == DisconnectAction::give_up);
}

TEST_CASE("wifi disconnect: losing an established connection is reported")
{
    CHECK(on_disconnected(true, false, true, 0, 5) ==
          DisconnectAction::report_lost);
}

TEST_CASE("wifi disconnect: nothing happens when no connection is wanted")
{
    CHECK(on_disconnected(false, false, true, 0, 5) ==
          DisconnectAction::ignore);
    CHECK(on_disconnected(false, true, true, 0, 5) == DisconnectAction::ignore);
}
