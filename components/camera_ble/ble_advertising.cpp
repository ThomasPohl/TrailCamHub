#include "ble_advertising.hpp"

#include <cstring>

namespace camera::ble
{

bool find_ad_field(const std::uint8_t *data, std::size_t data_len,
                   std::uint8_t type, const std::uint8_t **value,
                   std::size_t *value_len)
{
    if (data == nullptr || value == nullptr || value_len == nullptr) {
        return false;
    }
    for (std::size_t offset = 0; offset < data_len;) {
        const std::size_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > data_len) {
            break;
        }
        if (data[offset + 1] == type) {
            *value = data + offset + 2;
            *value_len = field_len - 1;
            return true;
        }
        offset += field_len + 1;
    }
    return false;
}

bool find_device_name(const std::uint8_t *data, std::size_t data_len,
                      const std::uint8_t **name, std::size_t *name_len)
{
    return find_ad_field(data, data_len, kAdTypeCompleteName, name, name_len) ||
           find_ad_field(data, data_len, kAdTypeShortName, name, name_len);
}

bool contains_uuid16(const std::uint8_t *data, std::size_t data_len,
                     std::uint16_t uuid)
{
    if (data == nullptr) {
        return false;
    }
    for (std::size_t offset = 0; offset < data_len;) {
        const std::size_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > data_len) {
            break;
        }
        const std::uint8_t type = data[offset + 1];
        if (type == kAdTypeUuid16Complete || type == kAdTypeUuid16Incomplete) {
            const std::uint8_t *value = data + offset + 2;
            const std::size_t value_len = field_len - 1;
            for (std::size_t i = 0; i + 1 < value_len; i += 2) {
                const std::uint16_t candidate = static_cast<std::uint16_t>(
                    value[i] | (static_cast<std::uint16_t>(value[i + 1]) << 8));
                if (candidate == uuid) {
                    return true;
                }
            }
        }
        offset += field_len + 1;
    }
    return false;
}

bool name_in_allowlist(const std::uint8_t *name, std::size_t name_len,
                       const char *const *allowlist, std::size_t allowlist_len)
{
    if (name == nullptr || name_len == 0 || allowlist == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < allowlist_len; i++) {
        if (std::strlen(allowlist[i]) == name_len &&
            std::memcmp(name, allowlist[i], name_len) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace camera::ble
