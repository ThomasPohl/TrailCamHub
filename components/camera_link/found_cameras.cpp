#include "found_cameras.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

namespace camera::found
{
namespace
{

// A clock before 2024 has not been set yet.
constexpr std::int64_t kClockValidFrom = 1704067200;

bool valid_address(const std::string &address)
{
    if (address.size() != 17) {
        return false;
    }
    for (std::size_t i = 0; i < address.size(); ++i) {
        const char c = address[i];
        const bool colon = i % 3 == 2;
        if (colon ? c != ':'
                  : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string clean_name(const std::string &name)
{
    std::string out;
    for (char c : name) {
        if (c != ',' && c != '\n' && c != '\r' &&
            static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    return out.substr(0, 32);
}

}  // namespace

std::vector<Entry> merge(const std::vector<Entry> &stored,
                         const std::vector<Entry> &heard, std::int64_t now)
{
    std::map<std::string, Entry> by_address;
    auto add = [&](const Entry &e) {
        if (!valid_address(e.address)) {
            return;
        }
        auto it = by_address.find(e.address);
        if (it == by_address.end()) {
            Entry copy = e;
            copy.name = clean_name(e.name);
            by_address.emplace(e.address, copy);
            return;
        }
        Entry &known = it->second;
        if (e.seen_at >= known.seen_at) {
            known.seen_at = e.seen_at;
            known.rssi = e.rssi;
        }
        if (!e.name.empty()) {
            known.name = clean_name(e.name);
        }
    };
    for (const Entry &e : stored) {
        add(e);
    }
    for (const Entry &e : heard) {
        add(e);
    }
    std::vector<Entry> out;
    for (const auto &[address, entry] : by_address) {
        (void)address;
        const bool old = now >= kClockValidFrom && entry.seen_at > 0 &&
                         entry.seen_at < now - kKeepS;
        if (!old) {
            out.push_back(entry);
        }
    }
    std::stable_sort(
        out.begin(), out.end(),
        [](const Entry &a, const Entry &b) { return a.seen_at > b.seen_at; });
    if (out.size() > kMaxEntries) {
        out.resize(kMaxEntries);
    }
    return out;
}

std::string serialize(const std::vector<Entry> &entries)
{
    std::string out;
    for (const Entry &e : entries) {
        out += e.address + "," + std::to_string(e.rssi) + "," +
               std::to_string(e.seen_at) + "," + clean_name(e.name) + "\n";
    }
    return out;
}

std::vector<Entry> parse(const std::string &text)
{
    std::vector<Entry> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t a = line.find(',');
        const std::size_t b =
            a == std::string::npos ? a : line.find(',', a + 1);
        const std::size_t c =
            b == std::string::npos ? b : line.find(',', b + 1);
        if (c == std::string::npos) {
            continue;
        }
        Entry e;
        e.address = line.substr(0, a);
        if (!valid_address(e.address)) {
            continue;
        }
        e.rssi = std::atoi(line.substr(a + 1, b - a - 1).c_str());
        e.seen_at =
            std::strtoll(line.substr(b + 1, c - b - 1).c_str(), nullptr, 10);
        e.name = clean_name(line.substr(c + 1));
        out.push_back(e);
    }
    return out;
}

}  // namespace camera::found
