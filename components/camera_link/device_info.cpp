#include "device_info.hpp"

#include "artemis_json.hpp"

namespace camera::artemis
{

bool parse_device_info(const std::string &json, DeviceInfo &out)
{
    out = DeviceInfo{};

    std::string s;
    std::int64_t n = 0;
    if (json_find_string(json, "devId", s)) {
        out.dev_id = s;
    } else if (json_find_int(json, "devId", n)) {
        out.dev_id = std::to_string(n);
    } else {
        return false;
    }

    if (json_find_string(json, "modelName", s))
        out.model_name = s;
    if (json_find_string(json, "fwVerName", s))
        out.fw_version = s;
    if (json_find_int(json, "batPercent", n))
        out.bat_percent = n;
    if (json_find_int(json, "sdTotalMB", n))
        out.sd_total_mb = n;
    if (json_find_int(json, "sdFreeMB", n))
        out.sd_free_mb = n;
    if (json_find_string(json, "sdCard", s))
        out.sd_card = parse_sd_card(s);
    if (json_find_string(json, "sdError", s))
        out.sd_error = s;
    if (json_find_int(json, "temperature", n))
        out.temperature_c = n;
    bool b = false;
    if (json_find_bool(json, "isCharging", b))
        out.charging = b;
    if (json_find_bool(json, "isSolarCharging", b))
        out.solar_charging = b;
    if (json_find_int(json, "batType", n))
        out.bat_type = n;
    if (json_find_string(json, "bleVerName", s))
        out.ble_version = s;
    if (json_find_string(json, "cameraName", s))
        out.camera_name = s;

    return true;
}

const char *sd_card_name(SdCard card)
{
    switch (card) {
    case SdCard::present:
        return "ok";
    case SdCard::missing:
        return "missing";
    case SdCard::unknown:
        break;
    }
    return "";
}

SdCard parse_sd_card(const std::string &name)
{
    if (name == "ok")
        return SdCard::present;
    if (name == "missing")
        return SdCard::missing;
    return SdCard::unknown;
}

std::string serialize_device_info(const DeviceInfo &info)
{
    std::string out = "{\"devId\":" + json_quote(info.dev_id) +
                      ",\"modelName\":" + json_quote(info.model_name) +
                      ",\"fwVerName\":" + json_quote(info.fw_version);
    if (info.bat_percent >= 0)
        out += ",\"batPercent\":" + std::to_string(info.bat_percent);
    if (info.sd_total_mb >= 0)
        out += ",\"sdTotalMB\":" + std::to_string(info.sd_total_mb);
    if (info.sd_free_mb >= 0)
        out += ",\"sdFreeMB\":" + std::to_string(info.sd_free_mb);
    if (info.sd_card != SdCard::unknown)
        out +=
            std::string(",\"sdCard\":\"") + sd_card_name(info.sd_card) + "\"";
    if (!info.sd_error.empty())
        out += ",\"sdError\":" + json_quote(info.sd_error);
    if (info.temperature_c)
        out += ",\"temperature\":" + std::to_string(*info.temperature_c);
    if (info.charging)
        out += std::string(",\"isCharging\":") +
               (*info.charging ? "true" : "false");
    if (info.solar_charging)
        out += std::string(",\"isSolarCharging\":") +
               (*info.solar_charging ? "true" : "false");
    if (info.bat_type)
        out += ",\"batType\":" + std::to_string(*info.bat_type);
    if (!info.ble_version.empty())
        out += ",\"bleVerName\":" + json_quote(info.ble_version);
    if (!info.camera_name.empty())
        out += ",\"cameraName\":" + json_quote(info.camera_name);
    return out + "}";
}

std::string device_state_summary(const DeviceInfo &info)
{
    std::string out;
    auto add = [&out](const std::string &part) {
        out += (out.empty() ? "" : ", ") + part;
    };
    if (info.bat_percent >= 0)
        add("battery " + std::to_string(info.bat_percent) + " %");
    if (info.charging.value_or(false) || info.solar_charging.value_or(false))
        add(info.solar_charging.value_or(false) ? "charging (solar)"
                                                : "charging");
    if (info.temperature_c)
        add(std::to_string(*info.temperature_c) + " \u00b0C");
    return out;
}

}  // namespace camera::artemis
