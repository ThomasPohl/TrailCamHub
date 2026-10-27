#include <doctest/doctest.h>

#include "gatt_profile.hpp"

using namespace camera::ble;

namespace
{

// GATT database of the TC100 camera as logged on hardware.
GattProfile tc100_profile()
{
    GattProfile profile;
    profile.add_service({0x1800, 1, 1});
    profile.add_service({0x1801, 2, 2});
    profile.add_service({0x8800, 3, 5});
    profile.add_service({0x8801, 6, 11});
    profile.add_service({0x180f, 12, 65535});
    profile.add_characteristic({2, 0x2a29, 4, 5, kPropRead});
    profile.add_characteristic({3, 0x0002, 7, 8, kPropWrite});
    profile.add_characteristic({3, 0x0003, 9, 10, kPropNotify});
    profile.add_characteristic({4, 0x2a19, 13, 14, kPropRead | kPropNotify});
    return profile;
}

}  // namespace

TEST_CASE("finds the camera endpoints in the TC100 profile")
{
    const GattProfile profile = tc100_profile();
    const CameraEndpoints endpoints =
        profile.find_camera(0x8801, 0x0002, 0x0003);
    REQUIRE(endpoints.found);
    CHECK(endpoints.control_value_handle == 8);
    CHECK(endpoints.control_write_with_response);
    CHECK(endpoints.response_value_handle == 10);
    CHECK(endpoints.response_end_handle == 11);
    CHECK(endpoints.response_cccd_value == kCccdNotify);
}

TEST_CASE("characteristic range ends before the next characteristic")
{
    GattProfile profile;
    profile.add_service({0x8801, 1, 20});
    profile.add_characteristic({0, 0x0003, 2, 3, kPropIndicate});
    profile.add_characteristic({0, 0x0002, 6, 7, kPropWriteNoResponse});
    const CameraEndpoints endpoints =
        profile.find_camera(0x8801, 0x0002, 0x0003);
    REQUIRE(endpoints.found);
    CHECK(endpoints.response_end_handle == 5);
    CHECK(endpoints.response_cccd_value == kCccdIndicate);
    CHECK_FALSE(endpoints.control_write_with_response);
}

TEST_CASE("rejects devices without the expected pair")
{
    GattProfile profile;
    profile.add_service({0x8801, 1, 10});
    profile.add_characteristic({0, 0x0002, 2, 3, kPropRead});
    profile.add_characteristic({0, 0x0003, 4, 5, kPropNotify});
    CHECK_FALSE(profile.find_camera(0x8801, 0x0002, 0x0003).found);

    // The characteristics must belong to the camera service.
    GattProfile other;
    other.add_service({0x1234, 1, 10});
    other.add_characteristic({0, 0x0002, 2, 3, kPropWrite});
    other.add_characteristic({0, 0x0003, 4, 5, kPropNotify});
    CHECK_FALSE(other.find_camera(0x8801, 0x0002, 0x0003).found);
}

TEST_CASE("table limits")
{
    GattProfile profile;
    for (std::size_t i = 0; i < GattProfile::kMaxServices; i++) {
        REQUIRE(profile.add_service({0x1000, 1, 2}));
    }
    CHECK_FALSE(profile.add_service({0x1000, 1, 2}));
    CHECK(profile.overflow());
    profile.reset();
    CHECK_FALSE(profile.overflow());
    CHECK_FALSE(profile.add_characteristic({0, 0x0002, 1, 2, kPropWrite}));
}
