#pragma once

#include <cstdint>

// Writes an 8-bit RGB PNG from a row-major 32-bit-per-pixel buffer. The deflate
// stream uses stored (uncompressed) blocks so no zlib is needed; files are big
// but this is a debugging aid, not a codec. `bgra` swaps the channel order on
// the way out. Returns false if the file could not be written.
bool write_png_rgb(const char* path, const std::uint8_t* pixels, unsigned width, unsigned height,
                   unsigned row_pitch_bytes, bool bgra);
