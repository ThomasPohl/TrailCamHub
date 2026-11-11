#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog_file.hpp"

using camera::store::recover_replaced_file;
using camera::store::replace_file;
using camera::store::replacement_path;
using camera::store::SaveThrottle;

namespace
{
// A directory of its own for each test case, removed with its files.
struct TempDir {
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/trailcamhub-catalog-XXXXXX";
        REQUIRE(mkdtemp(name) != nullptr);
        path = name;
    }
    ~TempDir()
    {
        remove((path + "/catalog.csv").c_str());
        remove((path + "/catalog.csv.tmp").c_str());
        rmdir(path.c_str());
    }
};

void write(const std::string &path, const std::string &text)
{
    FILE *f = fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    REQUIRE(fwrite(text.data(), 1, text.size(), f) == text.size());
    REQUIRE(fclose(f) == 0);
}

std::string read(const std::string &path)
{
    std::string text;
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return "<missing>";
    char buffer[64];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
        text.append(buffer, n);
    fclose(f);
    return text;
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}
}  // namespace

TEST_CASE("replace_file swaps in the temporary file")
{
    TempDir dir;
    const std::string path = dir.path + "/catalog.csv";
    write(path, "old");
    write(replacement_path(path), "new");
    CHECK(replace_file(path));
    CHECK(read(path) == "new");
    CHECK_FALSE(exists(replacement_path(path)));
}

TEST_CASE("replace_file creates the file on the first save")
{
    TempDir dir;
    const std::string path = dir.path + "/catalog.csv";
    write(replacement_path(path), "first");
    CHECK(replace_file(path));
    CHECK(read(path) == "first");
}

TEST_CASE("replace_file fails without a temporary file")
{
    TempDir dir;
    CHECK_FALSE(replace_file(dir.path + "/catalog.csv"));
}

TEST_CASE("a save interrupted after the old file was removed is recovered")
{
    TempDir dir;
    const std::string path = dir.path + "/catalog.csv";
    // Power loss between remove() and rename(): only the temporary file.
    write(replacement_path(path), "new");
    CHECK(recover_replaced_file(path));
    CHECK(read(path) == "new");
    CHECK_FALSE(exists(replacement_path(path)));
    // Nothing left to recover.
    CHECK_FALSE(recover_replaced_file(path));
}

TEST_CASE("a temporary file next to the catalog is not taken over")
{
    TempDir dir;
    const std::string path = dir.path + "/catalog.csv";
    // Power loss while the temporary file was written: it may be cut off.
    write(path, "complete");
    write(replacement_path(path), "cut o");
    CHECK_FALSE(recover_replaced_file(path));
    CHECK(read(path) == "complete");
}

TEST_CASE("nothing is recovered when no file exists")
{
    TempDir dir;
    CHECK_FALSE(recover_replaced_file(dir.path + "/catalog.csv"));
}

TEST_CASE("SaveThrottle saves after the given number of changes")
{
    SaveThrottle throttle(3, 1000);
    CHECK_FALSE(throttle.pending());
    CHECK_FALSE(throttle.note_change(0));
    CHECK(throttle.pending());
    CHECK_FALSE(throttle.note_change(1));
    CHECK(throttle.note_change(2));
    throttle.saved();
    CHECK_FALSE(throttle.pending());
    CHECK_FALSE(throttle.note_change(3));
}

TEST_CASE("SaveThrottle saves when the oldest change is too old")
{
    SaveThrottle throttle(100, 1000);
    CHECK_FALSE(throttle.note_change(5000));
    CHECK_FALSE(throttle.note_change(5999));
    CHECK(throttle.note_change(6000));
    throttle.saved();
    // The age counts from the first change after the save.
    CHECK_FALSE(throttle.note_change(9000));
    CHECK_FALSE(throttle.note_change(9999));
    CHECK(throttle.note_change(10000));
}

TEST_CASE("SaveThrottle with one change saves every time")
{
    SaveThrottle throttle(1, 1000);
    CHECK(throttle.note_change(0));
    throttle.saved();
    CHECK(throttle.note_change(1));
}
