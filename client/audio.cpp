// 3D audio with miniaudio: a pool of voices, distance attenuation, and a
// listener that follows the camera. Sounds are cached decoded in memory by
// miniaudio's resource manager, so playing the same file many times is cheap.
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "protocol.h"
#include "sfx.h"

namespace sg {

// One player's voice: a never-ending stream fed by network packets, played as a 3D sound at their head.
// A short jitter buffer (~100 ms) smooths out packets arriving unevenly.
struct VoiceStream {
  ma_data_source_base base{};  // must be first: miniaudio treats this struct as the data source
  std::mutex mu;
  std::deque<float> samples;
  bool primed = false;
  ma_sound sound{};
  bool soundOk = false;
  double lastHeard = 0;
};

static ma_result voiceRead(ma_data_source* ds, void* out, ma_uint64 frames, ma_uint64* read) {
  auto* v = reinterpret_cast<VoiceStream*>(ds);
  auto* o = static_cast<float*>(out);
  std::lock_guard<std::mutex> lock(v->mu);
  if (!v->primed && v->samples.size() >= static_cast<size_t>(kVoiceRate / 10)) v->primed = true;
  for (ma_uint64 i = 0; i < frames; ++i) {
    if (v->primed && !v->samples.empty()) {
      o[i] = v->samples.front();
      v->samples.pop_front();
    } else {
      o[i] = 0.0f;
      v->primed = false;  // ran dry: wait for the buffer to refill before speaking again
    }
  }
  if (read) *read = frames;
  return MA_SUCCESS;
}
static ma_result voiceSeek(ma_data_source*, ma_uint64) { return MA_SUCCESS; }
static ma_result voiceFormat(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel* map,
                             size_t mapCap) {
  *format = ma_format_f32;
  *channels = 1;
  *rate = kVoiceRate;
  if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, 1);
  return MA_SUCCESS;
}
static ma_result voiceCursor(ma_data_source*, ma_uint64* cursor) {
  *cursor = 0;
  return MA_SUCCESS;
}
static ma_result voiceLength(ma_data_source*, ma_uint64* length) {
  *length = 0;
  return MA_NOT_IMPLEMENTED;
}
static ma_data_source_vtable g_voiceVtable = {voiceRead, voiceSeek, voiceFormat, voiceCursor, voiceLength, nullptr, 0};

struct Audio::Impl {
  ma_engine engine{};
  bool ok = false;
  std::array<std::string, static_cast<size_t>(Sound::Count)> paths, farPaths;
  Vec3 listener;
  struct Loop {
    ma_sound sound{};
    bool ok = false;
    float volume = 0;
  };
  std::array<Loop, 4> ambience;  // wind, ocean, birds, night
  std::unordered_map<uint32_t, std::unique_ptr<VoiceStream>> voices_;
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
  // Regenerate everything once when the built-in sounds change (your own replacements: copy them in again after).
  int version = 0;
  {
    std::ifstream vf(dir + "/version.txt");
    vf >> version;
  }
  const bool regenerate = version != kSoundVersion;
  for (size_t i = 1; i < static_cast<size_t>(Sound::Count); ++i) {
    const Sound snd = static_cast<Sound>(i);
    std::string path = dir + "/" + soundName(snd) + ".wav";
    if ((regenerate || !std::filesystem::exists(path, ec)) && !writeSoundFile(snd, path))
      std::fprintf(stderr, "[audio] could not write %s\n", path.c_str());
    impl_->paths[i] = path;
    if (hasFarVariant(snd)) {
      std::string far = dir + "/" + soundName(snd) + "_far.wav";
      if ((regenerate || !std::filesystem::exists(far, ec)) && !writeFarSoundFile(snd, far))
        std::fprintf(stderr, "[audio] could not write %s\n", far.c_str());
      impl_->farPaths[i] = far;
    }
  }
  if (regenerate) std::ofstream(dir + "/version.txt") << kSoundVersion << "\n";
  if (ma_engine_init(nullptr, &impl_->engine) != MA_SUCCESS) {
    std::fprintf(stderr, "[audio] no audio device; playing without sound\n");
    return false;
  }
  ma_engine_listener_set_world_up(&impl_->engine, 0, 0, 1, 0);
  impl_->ok = true;
  const Sound loops[4] = {Sound::AmbWind, Sound::AmbOcean, Sound::AmbBirds, Sound::AmbNight};
  for (size_t k = 0; k < 4; ++k) {
    auto& l = impl_->ambience[k];
    l.ok = ma_sound_init_from_file(&impl_->engine, impl_->paths[static_cast<size_t>(loops[k])].c_str(),
                                   MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, &l.sound) == MA_SUCCESS;
    if (!l.ok) continue;
    ma_sound_set_looping(&l.sound, MA_TRUE);
    ma_sound_set_volume(&l.sound, 0);
    ma_sound_start(&l.sound);
  }
  return true;
}

void Audio::shutdown() {
  if (!impl_ || !impl_->ok) return;
  for (auto& v : impl_->voices)
    if (v.used) ma_sound_uninit(&v.sound);
  for (auto& l : impl_->ambience)
    if (l.ok) ma_sound_uninit(&l.sound);
  clearVoices();
  ma_engine_uninit(&impl_->engine);
  impl_->ok = false;
}

void Audio::setListener(const Vec3& pos, const Vec3& forward) {
  if (!impl_->ok) return;
  impl_->listener = pos;
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
  // Far away, a gunshot is a muffled boom rolling over the hills, not a crack.
  const std::string* path = &impl_->paths[static_cast<size_t>(s)];
  if (pos && !impl_->farPaths[static_cast<size_t>(s)].empty()) {
    Vec3 d = *pos - impl_->listener;
    if (d.x * d.x + d.y * d.y + d.z * d.z > 120.0f * 120.0f) {
      path = &impl_->farPaths[static_cast<size_t>(s)];
      volume *= 1.6f;
    }
  }
  if (ma_sound_init_from_file(&impl_->engine, path->c_str(), flags, nullptr, nullptr,
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

namespace sg {
void Audio::setAmbience(float wind, float ocean, float birds, float night, float dt) {
  if (!impl_->ok) return;
  const float want[4] = {wind * 0.35f, ocean * 0.55f, birds * 0.3f, night * 0.25f};
  const float k = dt > 0 ? std::min(1.0f, dt * 0.8f) : 1.0f;  // ~1 s fades
  for (size_t i = 0; i < 4; ++i) {
    auto& l = impl_->ambience[i];
    if (!l.ok) continue;
    l.volume += (want[i] - l.volume) * k;
    ma_sound_set_volume(&l.sound, l.volume);
  }
}
}  // namespace sg

namespace sg {
void Audio::voice(uint32_t speaker, const int16_t* pcm, size_t n, float volume) {
  if (!impl_->ok) return;
  auto& slot = impl_->voices_[speaker];
  if (!slot) {
    slot = std::make_unique<VoiceStream>();
    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable = &g_voiceVtable;
    if (ma_data_source_init(&cfg, &slot->base) != MA_SUCCESS) {
      impl_->voices_.erase(speaker);
      return;
    }
    slot->soundOk = ma_sound_init_from_data_source(&impl_->engine, &slot->base, 0, nullptr, &slot->sound) == MA_SUCCESS;
    if (slot->soundOk) {
      ma_sound_set_attenuation_model(&slot->sound, ma_attenuation_model_linear);
      ma_sound_set_min_distance(&slot->sound, 2.0f);
      ma_sound_set_max_distance(&slot->sound, kVoiceRange);
      ma_sound_set_rolloff(&slot->sound, 1.0f);
      ma_sound_start(&slot->sound);
    }
  }
  if (slot->soundOk) ma_sound_set_volume(&slot->sound, volume * 1.6f);
  std::lock_guard<std::mutex> lock(slot->mu);
  for (size_t i = 0; i < n; ++i) slot->samples.push_back(static_cast<float>(pcm[i]) / 32768.0f);
  while (slot->samples.size() > static_cast<size_t>(kVoiceRate / 2)) slot->samples.pop_front();  // never fall > 0.5 s behind
}

void Audio::setVoicePosition(uint32_t speaker, const Vec3& pos) {
  auto it = impl_->voices_.find(speaker);
  if (it != impl_->voices_.end() && it->second->soundOk) ma_sound_set_position(&it->second->sound, pos.x, pos.y, pos.z);
}

void Audio::clearVoices() {
  for (auto& [id, v] : impl_->voices_) {
    if (v->soundOk) ma_sound_uninit(&v->sound);
    ma_data_source_uninit(&v->base);
  }
  impl_->voices_.clear();
}
}  // namespace sg
