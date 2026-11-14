#include "cycle_record.hpp"

namespace cycle_trace
{

namespace
{

// The strings are names from the firmware; anything else becomes '_'.
std::string plain(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == ' ' || c == '.';
        out += ok ? c : '_';
    }
    return out;
}

void add_number(std::string &out, const char *key, std::int64_t value)
{
    out += ",\"";
    out += key;
    out += "\":";
    out += std::to_string(value);
}

void add_text(std::string &out, const char *key, const std::string &value)
{
    out += ",\"";
    out += key;
    out += "\":\"";
    out += plain(value);
    out += "\"";
}

}  // namespace

std::int64_t cycle_heap_minimum(std::int64_t boot_minimum_at_start,
                                std::int64_t boot_minimum_now,
                                std::int64_t lowest_sample)
{
    if (boot_minimum_now < boot_minimum_at_start ||
        lowest_sample < boot_minimum_now) {
        return boot_minimum_now;
    }
    return lowest_sample;
}

std::string cycle_line(const Record &record)
{
    std::string out = "{\"cycle\":" + std::to_string(record.number);
    if (record.start > 0) {
        add_number(out, "start", record.start);
    }
    add_text(out, "kind", record.kind);
    add_text(out, "trigger", record.trigger);
    add_text(out, "result", record.result);
    add_number(out, "durationS", record.duration_s);
    add_number(out, "camerasDone", record.cameras_done);
    add_number(out, "camerasRead", record.cameras_read);
    add_number(out, "heapMin", record.heap_min_internal);
    if (record.heap_min_psram >= 0) {
        add_number(out, "psramMin", record.heap_min_psram);
    }
    if (record.stack.session >= 0) {
        add_number(out, "stackSession", record.stack.session);
    }
    if (record.stack.uplink >= 0) {
        add_number(out, "stackUplink", record.stack.uplink);
    }
    if (record.stack.controller >= 0) {
        add_number(out, "stackController", record.stack.controller);
    }
    out += "}\n";
    return out;
}

std::string boot_line(const std::string &reset_reason, std::uint32_t last_cycle,
                      std::int64_t time)
{
    std::string out = "{\"boot\":\"" + plain(reset_reason) + "\"";
    if (time > 0) {
        add_number(out, "time", time);
    }
    add_number(out, "lastCycle", last_cycle);
    out += "}\n";
    return out;
}

std::string describe(const Record &record)
{
    std::string out =
        "cycle " + std::to_string(record.number) + " " + record.kind + " (" +
        record.trigger + "): " + record.result + ", " +
        std::to_string(record.duration_s) + " s, " +
        std::to_string(record.cameras_read) + " of " +
        std::to_string(record.cameras_done) + " cameras read; heap min " +
        std::to_string(record.heap_min_internal) + " B";
    if (record.heap_min_psram >= 0) {
        out += ", PSRAM " + std::to_string(record.heap_min_psram) + " B";
    }
    if (record.stack.session >= 0 || record.stack.uplink >= 0 ||
        record.stack.controller >= 0) {
        out += "; stack free min";
        const char *separator = " ";
        const auto add = [&](const char *name, std::int32_t value) {
            if (value >= 0) {
                out += separator;
                out += name;
                out += " " + std::to_string(value) + " B";
                separator = ", ";
            }
        };
        add("session", record.stack.session);
        add("uplink", record.stack.uplink);
        add("controller", record.stack.controller);
    }
    return out;
}

}  // namespace cycle_trace
