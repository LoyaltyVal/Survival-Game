// Little-endian byte streams used for packets, reliable messages and save files.
// Readers never trust their input: every read checks bounds and floats reject NaN/inf.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vecmath.h"

namespace sg {

class ByteWriter {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void f32(float v);
  void vec3(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
  void str(const std::string& s, size_t maxLen);  // maxLen <= 255
  void bytes(const std::vector<uint8_t>& b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
  const std::vector<uint8_t>& data() const { return buf_; }
  std::vector<uint8_t>& data() { return buf_; }
  size_t size() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool u8(uint8_t& v);
  bool u16(uint16_t& v);
  bool u32(uint32_t& v);
  bool f32(float& v);  // rejects NaN/inf
  bool vec3(Vec3& v) { return f32(v.x) && f32(v.y) && f32(v.z); }
  bool str(std::string& s, size_t maxLen);
  bool bytes(std::vector<uint8_t>& out, size_t n);
  size_t remaining() const { return size_ - pos_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

}  // namespace sg
