#pragma once

// Tiny helpers for the small, flat JSON the camera returns. Not a general
// JSON parser — just enough to read known fields. Pure logic, host-testable.

#include <cstdint>
#include <string>

namespace camera::artemis
{

// Reads an integer field from a (flat) JSON object: finds "key" followed by
// ':' and an optionally signed integer. Returns false if the key is absent or
// not followed by an integer. The search starts at `from`, which lets callers
// scope the lookup to one object within a larger document.
bool json_find_int(const std::string &json, const std::string &key,
                   std::int64_t &out, std::size_t from = 0);

// Reads a string field from a (flat) JSON object: finds "key" followed by ':'
// and a double-quoted value. Returns false if the key is absent or not
// followed by a string. Handles the common backslash escapes (\" \\ \/ \n \t
// \r). `from` scopes the lookup as in json_find_int.
bool json_find_string(const std::string &json, const std::string &key,
                      std::string &out, std::size_t from = 0);

// Returns `json` (a flat object, as written by serialize_device_info) with the
// integer field `key` set to `value`: an existing field of that name is
// replaced, otherwise it is added at the end. Other fields are kept.
std::string json_set_int(const std::string &json, const std::string &key,
                         std::int64_t value);

// `text` as a JSON string literal (with the quotes): `"` and `\` are escaped,
// control characters are dropped.
std::string json_quote(const std::string &text);

// Like json_set_int for the string field `key`. An existing field of that name
// is replaced whatever its value; the value of an existing field must be a
// string or a number.
std::string json_set_string(const std::string &json, const std::string &key,
                            const std::string &value);

// True for a field name whose value must never be logged, stored or sent on:
// the camera's screen unlock code (secureCode0 ... secureCode3; the switch
// secureCodeOnOff is not secret) and anything named like a password
// ("password", "pwd", "newPwd", ...), compared without case.
bool json_is_secret_key(const std::string &key);

// Returns `json` with the value of every secret field (json_is_secret_key) at
// any nesting depth replaced by "***", so that a camera answer can be logged.
// Strings, numbers, objects and arrays are all replaced. Everything else is
// copied unchanged; text that is not valid JSON is copied up to the point
// where it stops being readable.
std::string json_redact(const std::string &json);

}  // namespace camera::artemis
