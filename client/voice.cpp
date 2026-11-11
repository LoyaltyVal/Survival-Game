#include "voice.h"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>

#include "protocol.h"

namespace sg {

struct Microphone::Impl {
  ma_device device{};
  bool open = false;
  std::mutex mu;
  std::deque<int16_t> buffer;
  std::atomic<float> level{0};
};

static void onCapture(ma_device* dev, void*, const void* input, ma_uint32 frames) {
  auto* m = static_cast<Microphone::Impl*>(dev->pUserData);
  if (!input) return;
  const auto* in = static_cast<const int16_t*>(input);
  int peak = 0;
  {
    std::lock_guard<std::mutex> lock(m->mu);
    for (ma_uint32 i = 0; i < frames; ++i) {
      m->buffer.push_back(in[i]);
      peak = std::max(peak, std::abs(static_cast<int>(in[i])));
    }
    while (m->buffer.size() > static_cast<size_t>(kVoiceRate)) m->buffer.pop_front();  // keep at most 1 s
  }
  float p = static_cast<float>(peak) / 32768.0f;
  float old = m->level.load();
  m->level.store(std::max(p, old * 0.9f));
}

Microphone::Microphone() : impl_(std::make_unique<Impl>()) {}
Microphone::~Microphone() { stop(); }

bool Microphone::start() {
  if (impl_->open) return true;
  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.format = ma_format_s16;
  cfg.capture.channels = 1;
  cfg.sampleRate = kVoiceRate;
  cfg.dataCallback = onCapture;
  cfg.pUserData = impl_.get();
  if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
    std::fprintf(stderr, "[voice] no microphone found\n");
    return false;
  }
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    ma_device_uninit(&impl_->device);
    std::fprintf(stderr, "[voice] could not start the microphone\n");
    return false;
  }
  impl_->open = true;
  return true;
}

void Microphone::stop() {
  if (!impl_ || !impl_->open) return;
  ma_device_uninit(&impl_->device);
  impl_->open = false;
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->buffer.clear();
  impl_->level.store(0);
}

bool Microphone::running() const { return impl_->open; }

bool Microphone::pop(std::vector<int16_t>& frame) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->buffer.size() < kVoiceFrameSamples) return false;
  frame.assign(impl_->buffer.begin(), impl_->buffer.begin() + static_cast<std::ptrdiff_t>(kVoiceFrameSamples));
  impl_->buffer.erase(impl_->buffer.begin(), impl_->buffer.begin() + static_cast<std::ptrdiff_t>(kVoiceFrameSamples));
  return true;
}

float Microphone::level() const { return impl_->open ? impl_->level.load() : 0.0f; }

}  // namespace sg
