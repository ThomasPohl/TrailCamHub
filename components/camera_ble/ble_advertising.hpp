#pragma once

// Parsing of raw BLE advertising data (AD structures). This module has no
// ESP-IDF dependencies so it can be unit-tested on the host.

#include <cstddef>
#include <cstdint>

namespace camera::ble
{

// AD types used by the camera filter (Bluetooth Core Specification Supplement).
constexpr std::uint8_t kAdTypeUuid16Incomplete = 0x02;
constexpr std::uint8_t kAdTypeUuid16Complete = 0x03;
constexpr std::uint8_t kAdTypeShortName = 0x08;
constexpr std::uint8_t kAdTypeCompleteName = 0x09;

// Finds the first AD structure of `type` in `data`. On success, `value` points
// into `data` and `value_len` is the length of the field value.
bool find_ad_field(const std::uint8_t *data, std::size_t data_len,
                   std::uint8_t type, const std::uint8_t **value,
                   std::size_t *value_len);

// Finds the complete local name, or the shortened name if there is none.
bool find_device_name(const std::uint8_t *data, std::size_t data_len,
                      const std::uint8_t **name, std::size_t *name_len);

// Returns true if a complete or incomplete list of 16-bit service UUIDs in
// `data` contains `uuid`.
bool contains_uuid16(const std::uint8_t *data, std::size_t data_len,
                     std::uint16_t uuid);

// Returns true if `name` (not NUL-terminated) equals one of `allowlist`.
bool name_in_allowlist(const std::uint8_t *name, std::size_t name_len,
                       const char *const *allowlist, std::size_t allowlist_len);

}  // namespace camera::ble
