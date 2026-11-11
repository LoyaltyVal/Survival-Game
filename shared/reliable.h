// Reliable, ordered messages on top of UDP (go-back-N).
//
// Every message gets a sequence number and is resent in each outgoing Reliable
// packet until the other side acknowledges it. The receiver only delivers the
// next expected sequence number, so messages arrive exactly once and in order.
// Used for inventory, building, crafting, chat: things that must not be lost.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace sg {

class ReliableChannel {
 public:
  static constexpr size_t kMaxMessageSize = 1000;

  // Queues a message. Messages larger than kMaxMessageSize are dropped (bug).
  void queue(std::vector<uint8_t> msg);

  // True if there are unacknowledged messages or an ack to send.
  bool needsSend() const { return !unacked_.empty() || ackDirty_; }

  // Up to maxPackets Reliable packets carrying our ack and the oldest unacked messages.
  std::vector<std::vector<uint8_t>> build(int maxPackets);

  // Handles a Reliable packet. Appends newly delivered messages (in order) to
  // `delivered`. Returns false if the packet is malformed.
  bool receive(const uint8_t* data, size_t size, std::vector<std::vector<uint8_t>>& delivered);

  size_t unackedCount() const { return unacked_.size(); }
  void reset();

 private:
  struct Out {
    uint32_t seq;
    std::vector<uint8_t> data;
  };
  std::deque<Out> unacked_;
  uint32_t nextSeq_ = 1;
  uint32_t received_ = 0;  // highest sequence number delivered so far
  bool ackDirty_ = false;
};

}  // namespace sg
