#include "log_files.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sdlog
{

namespace
{

const char kPrefix[] = "log-";
const char kSuffix[] = ".txt";

}  // namespace

unsigned file_number(const char *name)
{
    const std::size_t prefix = std::strlen(kPrefix);
    if (std::strncmp(name, kPrefix, prefix) != 0) {
        return 0;
    }
    const char *digits = name + prefix;
    // strtoul would also take a sign or leading spaces.
    if (*digits < '0' || *digits > '9') {
        return 0;
    }
    // unsigned long long has 64 bits on the host and on the ESP32, so both
    // reject the same names (#106); ERANGE beyond that.
    char *end = nullptr;
    errno = 0;
    const unsigned long long number = std::strtoull(digits, &end, 10);
    if (errno == ERANGE || std::strcmp(end, kSuffix) != 0 ||
        number > 0xffffffffULL) {
        return 0;
    }
    return static_cast<unsigned>(number);
}

std::string file_name(unsigned number)
{
    char name[32];
    std::snprintf(name, sizeof(name), "%s%05u%s", kPrefix, number, kSuffix);
    return name;
}

std::vector<unsigned> files_to_remove(std::vector<unsigned> numbers,
                                      unsigned keep)
{
    if (numbers.size() <= keep) {
        return {};
    }
    std::sort(numbers.begin(), numbers.end());
    numbers.resize(numbers.size() - keep);
    return numbers;
}

std::size_t fit_line(char *line, int length, std::size_t capacity)
{
    if (length <= 0 || capacity < 2) {
        return 0;
    }
    if (static_cast<std::size_t>(length) < capacity) {
        return static_cast<std::size_t>(length);
    }
    // vsnprintf wrote capacity - 1 characters and the NUL.
    line[capacity - 2] = '\n';
    return capacity - 1;
}

std::size_t cycle_prefix(char *line, std::size_t capacity, unsigned cycle)
{
    if (cycle == 0) {
        return 0;
    }
    const int length = std::snprintf(line, capacity, "#%u ", cycle);
    if (length <= 0 || static_cast<std::size_t>(length) >= capacity) {
        return 0;
    }
    return static_cast<std::size_t>(length);
}

}  // namespace sdlog
