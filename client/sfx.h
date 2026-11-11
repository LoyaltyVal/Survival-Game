// Procedural sound effects (see sfx.cpp) and the audio player (audio.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "items.h"
#include "vecmath.h"

namespace sg {

// Synthesizes a sound effect into a 16-bit mono WAV file.
bool writeSoundFile(Sound s, const std::string& path);
// Gunshots and explosions also get a "_far" file: what they sound like from a few hundred metres.
bool hasFarVariant(Sound s);
bool writeFarSoundFile(Sound s, const std::string& path);
// Bump when the synthesized sounds change, so old generated files get replaced.
constexpr int kSoundVersion = 3;

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
  // Looping background: wind, waves, birds by day, crickets at night (each 0..1). Fades smoothly.
  void setAmbience(float wind, float ocean, float birds, float night, float dt);
  // Proximity voice: queue a decoded chunk from a speaker (kVoiceRate mono) and move their voice with them.
  void voice(uint32_t speaker, const int16_t* pcm, size_t samples, float volume);
  void setVoicePosition(uint32_t speaker, const Vec3& pos);
  void clearVoices();  // leaving a server

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sg
