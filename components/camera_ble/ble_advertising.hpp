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

// Returns true if `name` (not NUL-terminated) starts with one of `prefixes`
// and is longer than it.
bool name_has_prefix(const std::uint8_t *name, std::size_t name_len,
                     const char *const *prefixes, std::size_t prefixes_len);

// The BLE addresses of the cameras done in the current scan, so that the
// next activation picks another camera. Pure logic; addresses are the 6
// bytes of the address plus its type.
class AddressSet
{
  public:
    static constexpr std::size_t kMax = 8;

    void clear();
    // False if the set is full (the address is then not skipped).
    bool add(std::uint8_t type, const std::uint8_t address[6]);
    bool contains(std::uint8_t type, const std::uint8_t address[6]) const;
    std::size_t size() const;

  private:
    struct Entry {
        std::uint8_t type;
        std::uint8_t address[6];
    };
    Entry entries_[kMax] = {};
    std::size_t size_ = 0;
};

// "AA:BB:CC:DD:EE:FF" for an address as NimBLE stores it (least significant
// byte first, as in the log lines). `out` needs 18 bytes.
void format_address(const std::uint8_t address[6], char out[18]);

// Cameras heard in the scans of one wake-up, read or not (#296): address,
// advertised name, signal, time. Pure logic; camera_ble fills it, the session
// controller takes it to the SD card.
class SeenList
{
  public:
    static constexpr std::size_t kMax = 16;
    struct Entry {
        char address[18];
        char name[33];
        int rssi;
        std::int64_t seen_at;
    };

    // Notes an advertisement; `name` may be null or empty (a report without
    // a name, e.g. an advertisement whose name comes in the scan response).
    // False if the list is full with other addresses.
    bool note(const std::uint8_t address[6], const std::uint8_t *name,
              std::size_t name_len, int rssi, std::int64_t at);
    std::size_t size() const;
    const Entry &at(std::size_t i) const;
    void clear();

  private:
    Entry entries_[kMax] = {};
    std::size_t size_ = 0;
};

}  // namespace camera::ble
