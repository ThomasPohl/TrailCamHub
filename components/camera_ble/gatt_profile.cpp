#include "gatt_profile.hpp"

namespace camera::ble
{

void GattProfile::reset()
{
    service_count_ = 0;
    characteristic_count_ = 0;
    overflow_ = false;
}

bool GattProfile::add_service(const Service &service)
{
    if (service_count_ >= services_.size()) {
        overflow_ = true;
        return false;
    }
    services_[service_count_++] = service;
    return true;
}

bool GattProfile::add_characteristic(const Characteristic &characteristic)
{
    if (characteristic_count_ >= characteristics_.size() ||
        characteristic.service_index >= service_count_) {
        overflow_ = true;
        return false;
    }
    characteristics_[characteristic_count_++] = characteristic;
    return true;
}

std::size_t GattProfile::service_count() const
{
    return service_count_;
}

const Service &GattProfile::service(std::size_t index) const
{
    return services_[index];
}

std::size_t GattProfile::characteristic_count() const
{
    return characteristic_count_;
}

bool GattProfile::overflow() const
{
    return overflow_;
}

CameraEndpoints GattProfile::find_camera(std::uint16_t service_uuid,
                                         std::uint16_t control_uuid,
                                         std::uint16_t response_uuid) const
{
    CameraEndpoints endpoints;
    bool has_control = false;
    bool has_response = false;
    for (std::size_t i = 0; i < characteristic_count_; i++) {
        const Characteristic &chr = characteristics_[i];
        const Service &svc = services_[chr.service_index];
        if (svc.uuid16 != service_uuid) {
            continue;
        }
        if (chr.uuid16 == control_uuid &&
            (chr.properties & (kPropWrite | kPropWriteNoResponse)) != 0) {
            has_control = true;
            endpoints.control_value_handle = chr.value_handle;
            endpoints.control_write_with_response =
                (chr.properties & kPropWrite) != 0;
        } else if (chr.uuid16 == response_uuid &&
                   (chr.properties & (kPropNotify | kPropIndicate)) != 0) {
            has_response = true;
            endpoints.response_value_handle = chr.value_handle;
            endpoints.response_cccd_value = static_cast<std::uint16_t>(
                ((chr.properties & kPropNotify) != 0 ? kCccdNotify : 0) |
                ((chr.properties & kPropIndicate) != 0 ? kCccdIndicate : 0));
            // The characteristic ends before the next one in the same
            // service, or with the service.
            endpoints.response_end_handle = svc.end_handle;
            if (i + 1 < characteristic_count_ &&
                characteristics_[i + 1].service_index == chr.service_index) {
                endpoints.response_end_handle = static_cast<std::uint16_t>(
                    characteristics_[i + 1].definition_handle - 1);
            }
        }
    }
    endpoints.found = has_control && has_response;
    return endpoints;
}

}  // namespace camera::ble
