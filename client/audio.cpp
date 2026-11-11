// 3D audio with miniaudio: a pool of voices, distance attenuation, and a
// listener that follows the camera. Sounds are cached decoded in memory by
// miniaudio's resource manager, so playing the same file many times is cheap.
#include <miniaudio.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

#include "sfx.h"

namespace sg {

struct Audio::Impl {
  ma_engine engine{};
  bool ok = false;
  std::array<std::string, static_cast<size_t>(Sound::Count)> paths;
  struct Voice {
    ma_sound sound{};
    bool used = false;
    uint64_t order = 0;
  };
  std::array<Voice, 64> voices;
  uint64_t counter = 0;
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {}
Audio::~Audio() { shutdown(); }

bool Audio::init(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (size_t i = 1; i < static_cast<size_t>(Sound::Count); ++i) {
    std::string path = dir + "/" + soundName(static_cast<Sound>(i)) + ".wav";
    if (!std::filesystem::exists(path, ec) && !writeSoundFile(static_cast<Sound>(i), path))
      std::fprintf(stderr, "[audio] could not write %s\n", path.c_str());
    impl_->paths[i] = path;
  }
  if (ma_engine_init(nullptr, &impl_->engine) != MA_SUCCESS) {
    std::fprintf(stderr, "[audio] no audio device; playing without sound\n");
    return false;
  }
  ma_engine_listener_set_world_up(&impl_->engine, 0, 0, 1, 0);
  impl_->ok = true;
  return true;
}

void Audio::shutdown() {
  if (!impl_ || !impl_->ok) return;
  for (auto& v : impl_->voices)
    if (v.used) ma_sound_uninit(&v.sound);
  ma_engine_uninit(&impl_->engine);
  impl_->ok = false;
}

void Audio::setListener(const Vec3& pos, const Vec3& forward) {
  if (!impl_->ok) return;
  ma_engine_listener_set_position(&impl_->engine, 0, pos.x, pos.y, pos.z);
  ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
}

void Audio::setVolume(float volume) {
  if (impl_->ok) ma_engine_set_volume(&impl_->engine, volume);
}

void Audio::play(Sound s, const Vec3* pos, float volume, float pitch) {
  if (!impl_->ok || s == Sound::None || s >= Sound::Count) return;
  // Reuse a finished voice, or steal the oldest one.
  Impl::Voice* voice = nullptr;
  for (auto& v : impl_->voices)
    if (!v.used || !ma_sound_is_playing(&v.sound)) {
      voice = &v;
      break;
    }
  if (!voice) {
    voice = &impl_->voices[0];
    for (auto& v : impl_->voices)
      if (v.order < voice->order) voice = &v;
  }
  if (voice->used) ma_sound_uninit(&voice->sound);
  voice->used = false;
  ma_uint32 flags = MA_SOUND_FLAG_DECODE | (pos ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION);
  if (ma_sound_init_from_file(&impl_->engine, impl_->paths[static_cast<size_t>(s)].c_str(), flags, nullptr, nullptr,
                              &voice->sound) != MA_SUCCESS)
    return;
  voice->used = true;
  voice->order = ++impl_->counter;
  ma_sound_set_volume(&voice->sound, volume);
  ma_sound_set_pitch(&voice->sound, pitch);
  if (pos) {
    // Gunshots and explosions carry across the island; everything else is local.
    bool loud = (s >= Sound::Bow && s <= Sound::Sniper) || s == Sound::Explosion;
    ma_sound_set_position(&voice->sound, pos->x, pos->y, pos->z);
    ma_sound_set_attenuation_model(&voice->sound, ma_attenuation_model_inverse);
    ma_sound_set_rolloff(&voice->sound, 1.0f);
    ma_sound_set_min_distance(&voice->sound, loud ? 6.0f : 1.5f);
    ma_sound_set_max_distance(&voice->sound, loud ? 700.0f : 60.0f);
  }
  ma_sound_start(&voice->sound);
}

}  // namespace sg
