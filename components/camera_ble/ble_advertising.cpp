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

bool name_has_prefix(const std::uint8_t *name, std::size_t name_len,
                     const char *const *prefixes, std::size_t prefixes_len)
{
    if (name == nullptr || prefixes == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < prefixes_len; i++) {
        const std::size_t len = std::strlen(prefixes[i]);
        if (name_len > len && std::memcmp(name, prefixes[i], len) == 0) {
            return true;
        }
    }
    return false;
}

void AddressSet::clear()
{
    size_ = 0;
}

bool AddressSet::add(std::uint8_t type, const std::uint8_t address[6])
{
    if (contains(type, address)) {
        return true;
    }
    if (size_ >= kMax) {
        return false;
    }
    entries_[size_].type = type;
    std::memcpy(entries_[size_].address, address, 6);
    size_++;
    return true;
}

bool AddressSet::contains(std::uint8_t type,
                          const std::uint8_t address[6]) const
{
    for (std::size_t i = 0; i < size_; i++) {
        if (entries_[i].type == type &&
            std::memcmp(entries_[i].address, address, 6) == 0) {
            return true;
        }
    }
    return false;
}

std::size_t AddressSet::size() const
{
    return size_;
}

void format_address(const std::uint8_t address[6], char out[18])
{
    static const char kHex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; ++i) {
        const std::uint8_t b = address[5 - i];
        out[i * 3] = kHex[b >> 4];
        out[i * 3 + 1] = kHex[b & 0x0f];
        out[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
}

bool SeenList::note(const std::uint8_t address[6], const std::uint8_t *name,
                    std::size_t name_len, int rssi, std::int64_t at)
{
    char text[18];
    format_address(address, text);
    Entry *entry = nullptr;
    for (std::size_t i = 0; i < size_; ++i) {
        if (std::strcmp(entries_[i].address, text) == 0) {
            entry = &entries_[i];
            break;
        }
    }
    if (entry == nullptr) {
        if (size_ >= kMax) {
            return false;
        }
        entry = &entries_[size_++];
        *entry = Entry{};
        std::memcpy(entry->address, text, sizeof(text));
    }
    entry->rssi = rssi;
    entry->seen_at = at;
    if (name != nullptr && name_len > 0) {
        const std::size_t n = name_len < sizeof(entry->name) - 1
                                  ? name_len
                                  : sizeof(entry->name) - 1;
        std::memcpy(entry->name, name, n);
        entry->name[n] = '\0';
    }
    return true;
}

std::size_t SeenList::size() const
{
    return size_;
}

const SeenList::Entry &SeenList::at(std::size_t i) const
{
    return entries_[i];
}

void SeenList::clear()
{
    size_ = 0;
}

std::size_t AllowList::set(const char *const *addresses, std::size_t count)
{
    size_ = 0;
    for (std::size_t i = 0; i < count && size_ < kMax; ++i) {
        if (addresses[i] == nullptr || std::strlen(addresses[i]) != 17) {
            continue;
        }
        std::memcpy(addresses_[size_], addresses[i], 18);
        ++size_;
    }
    return size_;
}

bool AllowList::allows(const std::uint8_t address[6]) const
{
    if (size_ == 0) {
        return true;
    }
    char text[18];
    format_address(address, text);
    for (std::size_t i = 0; i < size_; ++i) {
        if (std::strcmp(addresses_[i], text) == 0) {
            return true;
        }
    }
    return false;
}

std::size_t AllowList::size() const
{
    return size_;
}

}  // namespace camera::ble
