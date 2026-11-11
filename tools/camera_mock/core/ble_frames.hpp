#pragma once

// BLE side of the camera mock: the advertisement, the wake-up write and the
// Wi-Fi credentials frame, as seen in the app's btsnoop capture
// (pwnb0t/trailcam-automation, btsnoop/trailcam_7-1-connect-btsnoop_hci.log).
// Pure logic, host-testable.
//
// Captured advertisement (31 bytes):
//   02 01 06                        flags
//   03 03 01 88                     16-bit service UUID 0x8801
//   0b ff 87 19 <devId:6> 00 68     manufacturer data: 0x1987, MAC, productId
//   0b 09 "TC100_0CFB"              complete name, last two MAC bytes
//
// GATT: service 0x8801 with characteristic 0x0002 (write) and 0x0003
// (notify). The app writes the frame 13 57 01 00 and 16 zero bytes, the hub
// "BT_KEY_ON"; the camera answers with notifications of at most 20 bytes:
//   13 57 01 00 <JSON length:4 LE> {"ret":0,"ssid":…,"bssid":…,"pwd":…}

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace camera_mock::ble
{

constexpr std::uint16_t kServiceUuid = 0x8801;
constexpr std::uint16_t kControlUuid = 0x0002;
constexpr std::uint16_t kResponseUuid = 0x0003;

// Advertising data like the camera's for the given identity. `name` is cut
// so that the data fits the 31 bytes of a legacy advertisement.
std::vector<std::uint8_t> advertising_data(const std::uint8_t dev_id[6],
                                           std::uint16_t product_id,
                                           const std::string &name);

// True for a write that wakes the camera's Wi-Fi: "BT_KEY_ON" (the hub's
// default, CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX) or the app's request frame
// starting with 13 57 01 00.
bool is_wake_command(const std::uint8_t *data, std::size_t size);

// The credentials frame: header and JSON in the captured key order.
std::vector<std::uint8_t> credentials_frame(const std::string &ssid,
                                            const std::string &bssid,
                                            const std::string &password);

// Splits a frame into notifications of at most `chunk` bytes (20 in the
// capture, the default ATT MTU of 23 minus 3).
std::vector<std::vector<std::uint8_t>>
split_notifications(const std::vector<std::uint8_t> &frame, std::size_t chunk);

// "1C:4E:A2:7E:5D:BD" for a 6-byte MAC.
std::string format_mac(const std::uint8_t mac[6]);

// The camera's AP name: "TrailCam_" and the last two bytes of its BSSID
// ("TrailCam_5DBD" for 1C:4E:A2:7E:5D:BD in the capture).
std::string ap_ssid(const std::uint8_t bssid[6]);

}  // namespace camera_mock::ble
