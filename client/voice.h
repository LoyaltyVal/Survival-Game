// Microphone capture for proximity voice chat (playback lives in Audio).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace sg {

class Microphone {
 public:
  Microphone();
  ~Microphone();
  // Opens the default recording device at kVoiceRate mono. False if there is none (or Windows blocks it).
  bool start();
  void stop();
  bool running() const;
  // Takes the next kVoiceFrameSamples of recorded audio, if that much has been captured.
  bool pop(std::vector<int16_t>& frame);
  float level() const;  // recent peak, 0..1 (for the settings meter)

  struct Impl;  // (public so the capture callback can reach it)

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace sg
