#include "adpcm.h"

#include <algorithm>

namespace sg::adpcm {

namespace {
const int kSteps[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                        31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                        544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                        9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndexStep[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// Applies one 4-bit code to the state and returns the new sample.
int16_t step(State& s, int code) {
  const int st = kSteps[s.index];
  int diff = st >> 3;
  if (code & 4) diff += st;
  if (code & 2) diff += st >> 1;
  if (code & 1) diff += st >> 2;
  int p = s.predictor + ((code & 8) ? -diff : diff);
  s.predictor = static_cast<int16_t>(std::clamp(p, -32768, 32767));
  s.index = static_cast<uint8_t>(std::clamp(static_cast<int>(s.index) + kIndexStep[code], 0, 88));
  return s.predictor;
}

int encodeOne(State& s, int16_t sample) {
  const int st = kSteps[s.index];
  int diff = sample - s.predictor;
  int code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  if (diff >= st) { code |= 4; diff -= st; }
  if (diff >= st >> 1) { code |= 2; diff -= st >> 1; }
  if (diff >= st >> 2) code |= 1;
  step(s, code);  // keep the encoder in lock-step with what the decoder will reconstruct
  return code;
}
}  // namespace

void encode(const int16_t* in, size_t n, State& s, uint8_t* out) {
  if (s.index > 88) s.index = 88;
  for (size_t i = 0; i + 1 < n; i += 2) {
    int lo = encodeOne(s, in[i]), hi = encodeOne(s, in[i + 1]);
    out[i / 2] = static_cast<uint8_t>(lo | (hi << 4));
  }
}

void decode(const uint8_t* in, size_t bytes, State& s, int16_t* out) {
  if (s.index > 88) s.index = 88;
  for (size_t i = 0; i < bytes; ++i) {
    out[2 * i] = step(s, in[i] & 0x0F);
    out[2 * i + 1] = step(s, in[i] >> 4);
  }
}

}  // namespace sg::adpcm
