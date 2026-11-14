#include <doctest/doctest.h>

#include "cycle_record.hpp"

using namespace cycle_trace;

TEST_CASE("the heap minimum of a cycle")
{
    // The minimum since boot fell during the cycle: that is the cycle's.
    CHECK(cycle_heap_minimum(90000, 70000, 75000) == 70000);
    // It did not: an earlier cycle went lower, this one stayed above it and
    // only the samples say how far.
    CHECK(cycle_heap_minimum(60000, 60000, 82000) == 82000);
    // A sample cannot be below the minimum since boot.
    CHECK(cycle_heap_minimum(60000, 60000, 50000) == 60000);
}

TEST_CASE("a cycle line carries what is known")
{
    Record record;
    record.number = 12;
    record.start = 1794000000;
    record.kind = "scan";
    record.trigger = "schedule";
    record.result = "uplink-done";
    record.duration_s = 42;
    record.cameras_done = 2;
    record.cameras_read = 1;
    record.heap_min_internal = 81234;
    record.heap_min_psram = 3912345;
    record.stack.session = 2100;
    record.stack.uplink = 5400;
    record.stack.controller = 3300;
    CHECK(cycle_line(record) ==
          "{\"cycle\":12,\"start\":1794000000,\"kind\":\"scan\","
          "\"trigger\":\"schedule\",\"result\":\"uplink-done\","
          "\"durationS\":42,\"camerasDone\":2,\"camerasRead\":1,"
          "\"heapMin\":81234,\"psramMin\":3912345,\"stackSession\":2100,"
          "\"stackUplink\":5400,\"stackController\":3300}\n");
    CHECK(describe(record) ==
          "cycle 12 scan (schedule): uplink-done, 42 s, 1 of 2 cameras read; "
          "heap min 81234 B, PSRAM 3912345 B; stack free min session 2100 B, "
          "uplink 5400 B, controller 3300 B");
}

TEST_CASE("a cycle line leaves out what is unknown")
{
    Record record;
    record.number = 1;
    record.kind = "checkin";
    record.trigger = "request";
    record.result = "uplink-unreachable";
    record.duration_s = 7;
    record.heap_min_internal = 40000;
    CHECK(cycle_line(record) ==
          "{\"cycle\":1,\"kind\":\"checkin\",\"trigger\":\"request\","
          "\"result\":\"uplink-unreachable\",\"durationS\":7,"
          "\"camerasDone\":0,\"camerasRead\":0,\"heapMin\":40000}\n");
    CHECK(describe(record) ==
          "cycle 1 checkin (request): uplink-unreachable, 7 s, 0 of 0 cameras "
          "read; heap min 40000 B");
}

TEST_CASE("a cycle line stays valid JSON with an unexpected name")
{
    Record record;
    record.number = 3;
    record.kind = "sc\"an";
    record.trigger = "a\\b";
    record.result = "x\ny";
    const std::string line = cycle_line(record);
    CHECK(line.find("\"kind\":\"sc_an\"") != std::string::npos);
    CHECK(line.find("\"trigger\":\"a_b\"") != std::string::npos);
    CHECK(line.find("\"result\":\"x_y\"") != std::string::npos);
}

TEST_CASE("the boot line names the reset and the last cycle")
{
    CHECK(boot_line("panic", 12, 1794000100) ==
          "{\"boot\":\"panic\",\"time\":1794000100,\"lastCycle\":12}\n");
    CHECK(boot_line("power-on", 0, 0) ==
          "{\"boot\":\"power-on\",\"lastCycle\":0}\n");
}
