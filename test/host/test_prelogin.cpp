#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "camera_prelogin.hpp"

using namespace camera;

TEST_CASE("builds the PRE_LOGIN plaintext observed on hardware")
{
    // A PRE_LOGIN captured on hardware decrypts (AES-128-ECB) to exactly
    // this JSON followed by seven bytes of PKCS#7 padding.
    const std::uint8_t nonce[kPreLoginNonceSize] = {0xaa, 0xb2, 0xea, 0x38,
                                                    0xba, 0x65, 0x47, 0x6d};
    std::uint8_t out[96];
    std::size_t json_len = 0;
    const std::size_t padded =
        build_prelogin_plaintext(39, nonce, out, sizeof(out), &json_len);
    const std::string expected =
        "{\"utcTime\":39,\"nonce\":\"aab2ea38ba65476d\"}";
    REQUIRE(json_len == expected.size());
    CHECK(std::string(reinterpret_cast<char *>(out), json_len) == expected);
    REQUIRE(padded == 48);
    for (std::size_t i = json_len; i < padded; i++) {
        CHECK(out[i] == 7);
    }
}

TEST_CASE("PKCS#7 adds a full block to aligned data")
{
    std::uint8_t data[32] = {};
    CHECK(pkcs7_pad(data, 16, sizeof(data)) == 32);
    CHECK(data[16] == 16);
    CHECK(data[31] == 16);
}

TEST_CASE("PKCS#7 and plaintext builder respect the capacity")
{
    std::uint8_t data[16] = {};
    CHECK(pkcs7_pad(data, 16, sizeof(data)) == 0);

    const std::uint8_t nonce[kPreLoginNonceSize] = {};
    std::uint8_t small[40];
    CHECK(build_prelogin_plaintext(1700000000, nonce, small, sizeof(small),
                                   nullptr) == 0);
}
