// Procedural sound effects (see sfx.cpp) and the audio player (audio.cpp).
#pragma once
#include <memory>
#include <string>

#include "items.h"
#include "vecmath.h"

namespace sg {

// Synthesizes a sound effect into a 16-bit mono WAV file.
bool writeSoundFile(Sound s, const std::string& path);

// 3D sound playback on top of miniaudio.
class Audio {
 public:
  Audio();
  ~Audio();
  // Loads sounds/<name>.wav, creating any that are missing. False = no audio device (game still runs).
  bool init(const std::string& soundDir);
  void shutdown();
  void setListener(const Vec3& pos, const Vec3& forward);
  void setVolume(float volume);  // 0..1 master
  // pos = nullptr plays it "in your head" (your own gun, hit markers).
  void play(Sound s, const Vec3* pos, float volume = 1.0f, float pitch = 1.0f);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sg
