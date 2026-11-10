#pragma once

// Typed view of the GET_DEV_INFO (cmdId 512) response: the camera's identity
// and a little state. `dev_id` is the stable per-camera key used to organise
// stored media; `bat_percent` is the battery metadata we report onward. Pure
// logic, host-testable.
//
// Only the fields listed in DeviceInfo are read, stored, logged and sent on
// (an allowlist): the answer differs between camera firmware versions and may
// carry fields we do not know, secrets among them (secureCode0..3).

#include <cstdint>
#include <optional>
#include <string>

namespace camera::artemis
{

// The camera's SD card as the hub last saw it. The camera does not say so in
// its device info: a media list that fails (getMediaListRet != 0) means no
// card. Stored in camera.json as "sdCard": "ok" / "missing" (absent while
// unknown) and reported in the check-in.
enum class SdCard : std::uint8_t {
    unknown,
    present,  // the media list was read
    missing,  // the media list failed (no card, or one the camera cannot use)
};

// "ok", "missing" or "" (unknown).
const char *sd_card_name(SdCard card);
// The inverse of sd_card_name(); anything else is unknown.
SdCard parse_sd_card(const std::string &name);

struct DeviceInfo {
    std::string dev_id;             // "devId" — stable camera identity
    std::string model_name;         // "modelName"
    std::string fw_version;         // "fwVerName"
    std::int64_t bat_percent = -1;  // "batPercent" (-1 = unknown)
    std::int64_t sd_total_mb = -1;  // "sdTotalMB"
    std::int64_t sd_free_mb = -1;   // "sdFreeMB"
    std::optional<std::int64_t> temperature_c;  // "temperature" (°C)
    std::optional<bool> charging;               // "isCharging"
    std::optional<bool> solar_charging;         // "isSolarCharging"
    std::optional<std::int64_t> bat_type;       // "batType" (camera's code)
    std::string ble_version;                    // "bleVerName"
    std::string camera_name;                    // "cameraName"
    // Not part of the camera's response; kept in the stored copy.
    SdCard sd_card = SdCard::unknown;  // "sdCard"
    std::string sd_error;              // "sdError": the camera's errorMsg
};

// Parses a device-info response. Returns false only if no device identity
// could be read (`devId` absent); other fields default when missing. `devId`
// is accepted as either a JSON string or a number.
bool parse_device_info(const std::string &json, DeviceInfo &out);

// Writes the fields back in the 512 response format, so a stored copy can be
// read with parse_device_info(). Unknown values (-1, unset optionals, empty
// strings other than the identity), an unknown SD card and an empty sd_error
// are left out.
std::string serialize_device_info(const DeviceInfo &info);

// One line of the camera's state for people, e.g.
// "battery 78 %, charging (solar), 22 °C". Unknown values are left out;
// empty if nothing is known.
std::string device_state_summary(const DeviceInfo &info);

}  // namespace camera::artemis
