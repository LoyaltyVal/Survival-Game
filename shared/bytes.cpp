#include "bytes.h"

#include <cmath>
#include <cstring>

namespace sg {

void ByteWriter::u16(uint16_t v) {
  buf_.push_back(static_cast<uint8_t>(v));
  buf_.push_back(static_cast<uint8_t>(v >> 8));
}
void ByteWriter::u32(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteWriter::f32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  u32(bits);
}
void ByteWriter::str(const std::string& s, size_t maxLen) {
  if (maxLen > 255) maxLen = 255;
  size_t n = s.size() < maxLen ? s.size() : maxLen;
  u8(static_cast<uint8_t>(n));
  buf_.insert(buf_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

bool ByteReader::u8(uint8_t& v) {
  if (remaining() < 1) return false;
  v = data_[pos_++];
  return true;
}
bool ByteReader::u16(uint16_t& v) {
  if (remaining() < 2) return false;
  v = static_cast<uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
  pos_ += 2;
  return true;
}
bool ByteReader::u32(uint32_t& v) {
  if (remaining() < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
  pos_ += 4;
  return true;
}
bool ByteReader::f32(float& v) {
  uint32_t bits;
  if (!u32(bits)) return false;
  std::memcpy(&v, &bits, 4);
  return std::isfinite(v);
}
bool ByteReader::str(std::string& s, size_t maxLen) {
  uint8_t n;
  if (!u8(n) || n > maxLen || remaining() < n) return false;
  s.assign(reinterpret_cast<const char*>(data_ + pos_), n);
  pos_ += n;
  return true;
}
bool ByteReader::bytes(std::vector<uint8_t>& out, size_t n) {
  if (remaining() < n) return false;
  out.assign(data_ + pos_, data_ + pos_ + n);
  pos_ += n;
  return true;
}

}  // namespace sg
