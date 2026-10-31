#include "png_writer.hpp"

#include <cstdio>

namespace
{

std::uint32_t crc32(const std::uint8_t *data, std::size_t size,
                    std::uint32_t crc = 0)
{
    crc = ~crc;
    for (std::size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put_chunk(std::vector<std::uint8_t> &out, const char *type,
               const std::vector<std::uint8_t> &data)
{
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_u32(out, crc32(body.data(), body.size()));
}

// Writes bits least significant first, as deflate requires.
class BitWriter
{
  public:
    explicit BitWriter(std::vector<std::uint8_t> &out) : out_(out)
    {
    }

    void bits(std::uint32_t value, int count)
    {
        for (int i = 0; i < count; i++) {
            bit((value >> i) & 1u);
        }
    }

    // Huffman codes are packed most significant bit first.
    void code(std::uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; i--) {
            bit((value >> i) & 1u);
        }
    }

    void flush()
    {
        if (used_ > 0) {
            out_.push_back(current_);
            current_ = 0;
            used_ = 0;
        }
    }

  private:
    void bit(std::uint32_t value)
    {
        current_ |= static_cast<std::uint8_t>(value << used_);
        if (++used_ == 8) {
            flush();
        }
    }

    std::vector<std::uint8_t> &out_;
    std::uint8_t current_ = 0;
    int used_ = 0;
};

// Literal/length symbol with the fixed Huffman code of deflate.
void put_symbol(BitWriter &writer, int symbol)
{
    if (symbol < 144) {
        writer.code(0x30 + symbol, 8);
    } else if (symbol < 256) {
        writer.code(0x190 + symbol - 144, 9);
    } else if (symbol < 280) {
        writer.code(symbol - 256, 7);
    } else {
        writer.code(0xc0 + symbol - 280, 8);
    }
}

// A repeat of the previous byte, 3 to 258 times (distance 1).
void put_repeat(BitWriter &writer, int length)
{
    static const int kBase[] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int kExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int index = 28;
    while (kBase[index] > length) {
        index--;
    }
    put_symbol(writer, 257 + index);
    writer.bits(static_cast<std::uint32_t>(length - kBase[index]),
                kExtra[index]);
    writer.code(0, 5);  // distance code 0: distance 1
}

// zlib stream with one fixed-Huffman deflate block that encodes runs of equal
// bytes; enough for screens made of large single-coloured areas.
std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t> &raw)
{
    std::vector<std::uint8_t> out = {0x78, 0x01};
    BitWriter writer(out);
    writer.bits(1, 1);  // last block
    writer.bits(1, 2);  // fixed Huffman codes
    std::size_t i = 0;
    while (i < raw.size()) {
        put_symbol(writer, raw[i]);
        std::size_t run = 0;
        while (i + 1 + run < raw.size() && raw[i + 1 + run] == raw[i]) {
            run++;
        }
        i++;
        while (run >= 3) {
            const int length = run > 258 ? 258 : static_cast<int>(run);
            put_repeat(writer, length);
            run -= length;
            i += length;
        }
        // A remainder of one or two bytes is sent as literals.
        for (; run > 0; run--, i++) {
            put_symbol(writer, raw[i]);
        }
    }
    put_symbol(writer, 256);  // end of block
    writer.flush();
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put_u32(out, (b << 16) | a);
    return out;
}

}  // namespace

bool write_png(const std::string &path, int width, int height,
               const std::vector<std::uint8_t> &rgb)
{
    // Filter type "Up": a row equal to the one above becomes all zeros.
    const std::size_t stride = static_cast<std::size_t>(width) * 3;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (stride + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(2);
        for (std::size_t x = 0; x < stride; x++) {
            const std::uint8_t above = y > 0 ? rgb[(y - 1) * stride + x] : 0;
            raw.push_back(
                static_cast<std::uint8_t>(rgb[y * stride + x] - above));
        }
    }

    std::vector<std::uint8_t> png = {0x89, 'P',  'N',  'G',
                                     '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> header;
    put_u32(header, static_cast<std::uint32_t>(width));
    put_u32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", zlib_compress(raw));
    put_chunk(png, "IEND", {});

    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    return std::fclose(file) == 0 && ok;
}
