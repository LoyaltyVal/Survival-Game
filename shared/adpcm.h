// IMA ADPCM: 16-bit audio squeezed to 4 bits per sample. Used for proximity
// voice chat (16 kHz mono = 8 kB/s per speaker). Every packet carries the coder
// state it starts from, so a lost packet never garbles the ones after it.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sg::adpcm {

struct State {
  int16_t predictor = 0;
  uint8_t index = 0;  // step table index, 0..88
};

// n samples -> n/2 bytes (low nibble first). n must be even. Updates s.
void encode(const int16_t* in, size_t n, State& s, uint8_t* out);
// bytes -> bytes*2 samples. Updates s.
void decode(const uint8_t* in, size_t bytes, State& s, int16_t* out);

}  // namespace sg::adpcm
