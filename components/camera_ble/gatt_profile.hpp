#pragma once

// Model of a peer's GATT database as discovered over BLE, and the check
// whether it exposes the camera's Wi-Fi control service. This module has no
// ESP-IDF or Bluetooth stack dependencies so it can be unit-tested on the
// host.

#include <array>
#include <cstddef>
#include <cstdint>

namespace camera::ble
{

// Characteristic properties (Bluetooth Core Specification, Vol 3, Part G).
constexpr std::uint8_t kPropRead = 0x02;
constexpr std::uint8_t kPropWriteNoResponse = 0x04;
constexpr std::uint8_t kPropWrite = 0x08;
constexpr std::uint8_t kPropNotify = 0x10;
constexpr std::uint8_t kPropIndicate = 0x20;

// Client Characteristic Configuration values.
constexpr std::uint16_t kCccdNotify = 0x0001;
constexpr std::uint16_t kCccdIndicate = 0x0002;

struct Service {
    // 16-bit UUID, or 0 for 32/128-bit UUIDs.
    std::uint16_t uuid16;
    std::uint16_t start_handle;
    std::uint16_t end_handle;
};

struct Characteristic {
    std::size_t service_index;
    // 16-bit UUID, or 0 for 32/128-bit UUIDs.
    std::uint16_t uuid16;
    std::uint16_t definition_handle;
    std::uint16_t value_handle;
    std::uint8_t properties;
};

struct CameraEndpoints {
    bool found = false;
    std::uint16_t control_value_handle = 0;
    bool control_write_with_response = false;
    std::uint16_t response_value_handle = 0;
    // Last handle belonging to the response characteristic; its descriptors
    // (including the CCCD) lie between value and end handle.
    std::uint16_t response_end_handle = 0;
    // CCCD value that enables notifications and/or indications.
    std::uint16_t response_cccd_value = 0;
};

class GattProfile
{
  public:
    static constexpr std::size_t kMaxServices = 16;
    static constexpr std::size_t kMaxCharacteristics = 32;

    void reset();
    // Returns false if the table is full; `overflow()` then becomes true.
    bool add_service(const Service &service);
    bool add_characteristic(const Characteristic &characteristic);

    std::size_t service_count() const;
    const Service &service(std::size_t index) const;
    std::size_t characteristic_count() const;
    bool overflow() const;

    // Looks for `service_uuid` containing a writable `control_uuid` and a
    // notifying or indicating `response_uuid` characteristic.
    CameraEndpoints find_camera(std::uint16_t service_uuid,
                                std::uint16_t control_uuid,
                                std::uint16_t response_uuid) const;

  private:
    std::array<Service, kMaxServices> services_{};
    std::array<Characteristic, kMaxCharacteristics> characteristics_{};
    std::size_t service_count_ = 0;
    std::size_t characteristic_count_ = 0;
    bool overflow_ = false;
};

}  // namespace camera::ble
