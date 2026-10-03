#include "png_writer.hpp"
#include <windows.h>
#include <cstring>

namespace {

std::uint32_t crc32_table[256];
bool crc32_table_ready = false;

void init_crc32_table() {
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[n] = c;
    }
    crc32_table_ready = true;
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
    crc ^= 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void put_be32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

bool write_all(HANDLE file, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = size > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(size);
        if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        bytes += written;
        size -= written;
    }
    return true;
}

bool write_chunk(HANDLE file, const char type[4], const std::uint8_t* data, std::size_t size) {
    std::uint8_t header[8];
    put_be32(header, static_cast<std::uint32_t>(size));
    std::memcpy(header + 4, type, 4);

    std::uint32_t crc = crc32_update(0, header + 4, 4);
    if (size > 0) {
        crc = crc32_update(crc, data, size);
    }
    std::uint8_t trailer[4];
    put_be32(trailer, crc);

    return write_all(file, header, sizeof(header)) && (size == 0 || write_all(file, data, size)) &&
           write_all(file, trailer, sizeof(trailer));
}

}  // namespace

bool write_png_rgb(const char* path, const std::uint8_t* pixels, unsigned width, unsigned height,
                   unsigned row_pitch_bytes, bool bgra) {
    if (!crc32_table_ready) {
        init_crc32_table();
    }
    if (width == 0 || height == 0) {
        return false;
    }

    // Raw scanlines: filter byte + RGB per pixel.
    const std::size_t scanline = 1 + static_cast<std::size_t>(width) * 3;
    const std::size_t raw_size = scanline * height;

    // Zlib stream: 2-byte header, stored blocks of up to 65535 bytes (5-byte
    // block header each), 4-byte Adler-32.
    const std::size_t block_count = (raw_size + 65534) / 65535;
    const std::size_t zlib_size = 2 + raw_size + block_count * 5 + 4;

    auto* idat = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), 0, zlib_size));
    if (!idat) {
        return false;
    }

    std::size_t out = 0;
    idat[out++] = 0x78;  // CM=8, CINFO=7
    idat[out++] = 0x01;  // FCHECK, no dictionary, fastest

    std::uint32_t adler_a = 1;
    std::uint32_t adler_b = 0;
    std::size_t block_remaining = 0;
    unsigned y = 0;
    unsigned x_byte = 0;  // position within the current scanline (0 = filter byte)
    std::size_t emitted = 0;

    while (emitted < raw_size) {
        if (block_remaining == 0) {
            const std::size_t left = raw_size - emitted;
            block_remaining = left > 65535 ? 65535 : left;
            const bool final_block = (emitted + block_remaining) == raw_size;
            idat[out++] = final_block ? 1 : 0;
            idat[out++] = static_cast<std::uint8_t>(block_remaining & 0xFF);
            idat[out++] = static_cast<std::uint8_t>(block_remaining >> 8);
            idat[out++] = static_cast<std::uint8_t>(~block_remaining & 0xFF);
            idat[out++] = static_cast<std::uint8_t>((~block_remaining >> 8) & 0xFF);
        }

        std::uint8_t byte;
        if (x_byte == 0) {
            byte = 0;  // filter: none
        } else {
            const unsigned pixel_index = (x_byte - 1) / 3;
            const unsigned channel = (x_byte - 1) % 3;
            const std::uint8_t* pixel = pixels + static_cast<std::size_t>(y) * row_pitch_bytes + pixel_index * 4;
            byte = bgra ? pixel[2 - channel] : pixel[channel];
        }
        idat[out++] = byte;
        adler_a = (adler_a + byte) % 65521;
        adler_b = (adler_b + adler_a) % 65521;

        ++emitted;
        --block_remaining;
        if (++x_byte == scanline) {
            x_byte = 0;
            ++y;
        }
    }

    put_be32(idat + out, (adler_b << 16) | adler_a);
    out += 4;

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, idat);
        return false;
    }

    static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::uint8_t ihdr[13];
    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // colour type: truecolour
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace

    const bool ok = write_all(file, signature, sizeof(signature)) && write_chunk(file, "IHDR", ihdr, sizeof(ihdr)) &&
                    write_chunk(file, "IDAT", idat, out) && write_chunk(file, "IEND", nullptr, 0);

    CloseHandle(file);
    HeapFree(GetProcessHeap(), 0, idat);
    return ok;
}
