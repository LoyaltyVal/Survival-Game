#include "reliable.h"

#include "protocol.h"

namespace sg {

constexpr size_t kReliableHeader = 4 + 1 + 4 + 1;  // magic, type, ack, count
constexpr size_t kPerMessageHeader = 4 + 2;         // seq, length

void ReliableChannel::queue(std::vector<uint8_t> msg) {
  if (msg.empty() || msg.size() > kMaxMessageSize) return;
  unacked_.push_back({nextSeq_++, std::move(msg)});
}

std::vector<std::vector<uint8_t>> ReliableChannel::build(int maxPackets) {
  std::vector<std::vector<uint8_t>> packets;
  size_t i = 0;
  do {
    ByteWriter w = packetHeader(PacketType::Reliable);
    w.u32(received_);
    size_t countPos = w.size();
    w.u8(0);
    uint8_t count = 0;
    while (i < unacked_.size() && count < 255 &&
           w.size() + kPerMessageHeader + unacked_[i].data.size() <= kMaxPacketSize) {
      w.u32(unacked_[i].seq);
      w.u16(static_cast<uint16_t>(unacked_[i].data.size()));
      w.bytes(unacked_[i].data);
      ++count;
      ++i;
    }
    w.data()[countPos] = count;
    packets.push_back(std::move(w.data()));
  } while (i < unacked_.size() && static_cast<int>(packets.size()) < maxPackets);
  ackDirty_ = false;
  return packets;
}

bool ReliableChannel::receive(const uint8_t* data, size_t size, std::vector<std::vector<uint8_t>>& delivered) {
  ByteReader r(data, size);
  uint32_t ack;
  uint8_t count;
  if (!(readPacketHeader(r, PacketType::Reliable) && r.u32(ack) && r.u8(count))) return false;
  while (!unacked_.empty() && unacked_.front().seq <= ack) unacked_.pop_front();
  for (uint8_t k = 0; k < count; ++k) {
    uint32_t seq;
    uint16_t len;
    std::vector<uint8_t> msg;
    if (!(r.u32(seq) && r.u16(len)) || len == 0 || len > kMaxMessageSize || !r.bytes(msg, len)) return false;
    if (seq == received_ + 1) {
      received_ = seq;
      delivered.push_back(std::move(msg));
    }
    // Older: duplicate. Newer: a gap; the sender will resend in order.
  }
  if (count > 0) ackDirty_ = true;
  return true;
}

void ReliableChannel::reset() {
  unacked_.clear();
  nextSeq_ = 1;
  received_ = 0;
  ackDirty_ = false;
}

}  // namespace sg
