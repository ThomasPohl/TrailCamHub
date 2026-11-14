#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "log_files.hpp"

TEST_CASE("file_number parses log file names and rejects others")
{
    CHECK(sdlog::file_number("log-00042.txt") == 42);
    CHECK(sdlog::file_number("log-00001.txt") == 1);
    CHECK(sdlog::file_number("log-123456.txt") == 123456);
    CHECK(sdlog::file_number("log-00000.txt") == 0);
    CHECK(sdlog::file_number("log-.txt") == 0);
    CHECK(sdlog::file_number("log-00042.txt.tmp") == 0);
    CHECK(sdlog::file_number("log-00042.TXT") == 0);
    CHECK(sdlog::file_number("log-00042") == 0);
    CHECK(sdlog::file_number("log-+42.txt") == 0);
    // The same on the host and the ESP32 (#106).
    CHECK(sdlog::file_number("log-4294967295.txt") == 4294967295u);
    CHECK(sdlog::file_number("log-4294967296.txt") == 0);
    CHECK(sdlog::file_number("log-99999999999999999999999.txt") == 0);
    CHECK(sdlog::file_number("log- 42.txt") == 0);
    CHECK(sdlog::file_number("log-4a.txt") == 0);
    CHECK(sdlog::file_number("LOG-00042.txt") == 0);
    CHECK(sdlog::file_number("catalog.csv") == 0);
    CHECK(sdlog::file_number("") == 0);
}

TEST_CASE("file_name and file_number round-trip")
{
    CHECK(sdlog::file_name(42) == "log-00042.txt");
    CHECK(sdlog::file_name(123456) == "log-123456.txt");
    for (unsigned number : {1u, 99999u, 100000u, 4000000000u}) {
        CHECK(sdlog::file_number(sdlog::file_name(number).c_str()) == number);
    }
}

TEST_CASE("files_to_remove keeps the newest files")
{
    using V = std::vector<unsigned>;
    CHECK(sdlog::files_to_remove(V{}, 3).empty());
    CHECK(sdlog::files_to_remove(V{1, 2, 3}, 3).empty());
    CHECK(sdlog::files_to_remove(V{1, 2, 3, 4, 5}, 3) == V{1, 2});
    // Directory order is arbitrary, and pruning leaves gaps.
    CHECK(sdlog::files_to_remove(V{9, 3, 12, 7}, 2) == V{3, 7});
    // One slot kept free for a new file (SD_LOG_MAX_FILES - 1).
    CHECK(sdlog::files_to_remove(V{5, 6, 7}, 2) == V{5});
    CHECK(sdlog::files_to_remove(V{5, 6, 7}, 0) == V{5, 6, 7});
}

TEST_CASE("fit_line cuts overlong lines and keeps the newline")
{
    char line[8];
    std::strcpy(line, "abc\n");
    CHECK(sdlog::fit_line(line, 4, sizeof(line)) == 4);
    CHECK(std::string(line, 4) == "abc\n");

    // vsnprintf of a 12-character line into 8 bytes: 7 characters and NUL.
    std::strcpy(line, "abcdefg");
    CHECK(sdlog::fit_line(line, 12, sizeof(line)) == 7);
    CHECK(std::string(line, 7) == "abcdef\n");

    // Exactly the capacity: vsnprintf cut the last character too.
    std::strcpy(line, "abcdefg");
    CHECK(sdlog::fit_line(line, 8, sizeof(line)) == 7);
    CHECK(line[6] == '\n');

    CHECK(sdlog::fit_line(line, 7, sizeof(line)) == 7);
    CHECK(sdlog::fit_line(line, 0, sizeof(line)) == 0);
    CHECK(sdlog::fit_line(line, -1, sizeof(line)) == 0);
}

TEST_CASE("cycle_prefix marks the lines of a cycle")
{
    char line[16];
    CHECK(sdlog::cycle_prefix(line, sizeof(line), 12) == 4);
    CHECK(std::string(line, 4) == "#12 ");
    // Between cycles the lines carry no mark.
    CHECK(sdlog::cycle_prefix(line, sizeof(line), 0) == 0);
    // No room: no mark instead of a cut one.
    CHECK(sdlog::cycle_prefix(line, 4, 12) == 0);
}
