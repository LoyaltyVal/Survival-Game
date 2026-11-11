// Procedural sound effects. Every sound is synthesized at first launch and
// written to sounds/<name>.wav; drop in your own .wav with the same name to
// replace any of them.
//
// Gunshots are built the way real recordings sound: a sharp supersonic crack,
// a filtered "body" from the muzzle blast, a low thump, then early echoes and
// a reverb tail from the surroundings. Each gun gets its own mix.
#include "sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace sg {

namespace {

constexpr int kRate = 44100;
constexpr float kTau = 6.28318530718f;

struct Rng {
  uint32_t s;
  float next() {  // -1..1
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) & 0xFFFF) / 32767.5f - 1.0f;
  }
};

struct LowPass {  // one-pole filter
  float a, z = 0;
  explicit LowPass(float hz) : a(std::exp(-kTau * hz / kRate)) {}
  float operator()(float x) { return z = x * (1 - a) + z * a; }
};

std::vector<float> buffer(float seconds) { return std::vector<float>(static_cast<size_t>(seconds * kRate), 0.0f); }
float timeOf(size_t i) { return static_cast<float>(i) / kRate; }

void normalize(std::vector<float>& v, float peak = 0.95f) {
  float m = 0;
  for (float x : v) m = std::max(m, std::fabs(x));
  if (m > 0)
    for (float& x : v) x *= peak / m;
}

// Gentle saturation: makes transients punchy without hard clipping.
void saturate(std::vector<float>& v, float drive) {
  for (float& x : v) x = std::tanh(x * drive) / std::tanh(drive);
}

void echoes(std::vector<float>& v, std::initializer_list<std::pair<float, float>> taps) {
  std::vector<float> dry = v;
  for (auto [delay, gain] : taps) {
    size_t d = static_cast<size_t>(delay * kRate);
    LowPass soften(2500);  // reflections lose their highs
    for (size_t i = d; i < v.size(); ++i) v[i] += soften(dry[i - d]) * gain;
  }
}

void addClick(std::vector<float>& v, float at, float freq, float gain, uint32_t seed) {
  Rng r{seed};
  size_t start = static_cast<size_t>(at * kRate), len = static_cast<size_t>(0.04f * kRate);
  for (size_t k = 0; k < len && start + k < v.size(); ++k) {
    float t = timeOf(k);
    v[start + k] += (r.next() * std::exp(-t / 0.0018f) * 0.9f + std::sin(kTau * freq * t) * std::exp(-t / 0.006f) * 0.6f) * gain;
  }
}

struct GunShape {
  float crack;      // supersonic snap
  float bodyHz;     // muzzle blast colour
  float bodyDecay;  // seconds
  float thumpHz;
  float thump;
  float tail;       // reverb decay, seconds
  float length;
};

std::vector<float> gunshot(const GunShape& g, uint32_t seed) {
  auto v = buffer(g.length);
  Rng r{seed};
  LowPass body(g.bodyHz), tailLp(900), tailLp2(350);
  float phase = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    float t = timeOf(i), n = r.next();
    float crack = n * std::exp(-t / 0.0022f) * g.crack;
    float bod = body(n) * std::exp(-t / g.bodyDecay) * 3.0f;
    float f = g.thumpHz * (1.0f + 1.6f * std::exp(-t / 0.018f));  // pitch drops as the blast expands
    phase += kTau * f / kRate;
    float thump = std::sin(phase) * std::exp(-t / 0.075f) * g.thump;
    float tail = tailLp2(tailLp(n)) * std::exp(-t / g.tail) * 1.2f * std::min(1.0f, t / 0.015f);
    v[i] = crack + bod + thump + tail;
  }
  echoes(v, {{0.045f, 0.32f}, {0.105f, 0.2f}, {0.19f, 0.12f}, {0.33f, 0.06f}});
  saturate(v, 1.8f);
  normalize(v);
  return v;
}

std::vector<float> silencedShot() {
  auto v = buffer(0.35f);
  Rng r{77};
  LowPass body(1100);
  float phase = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    float t = timeOf(i), n = r.next();
    phase += kTau * 150.0f / kRate;
    v[i] = body(n) * std::exp(-t / 0.018f) * 2.2f + std::sin(phase) * std::exp(-t / 0.03f) * 0.35f +
           n * std::exp(-t / 0.001f) * 0.25f;
  }
  addClick(v, 0.012f, 3200, 0.35f, 5);  // the action cycling
  addClick(v, 0.06f, 2100, 0.2f, 6);
  normalize(v, 0.8f);
  return v;
}

std::vector<float> bowShot() {
  auto v = buffer(0.6f);
  // Karplus-Strong plucked string for the twang.
  const int period = kRate / 115;
  std::vector<float> ring(static_cast<size_t>(period));
  Rng r{11};
  for (float& x : ring) x = r.next();
  size_t idx = 0;
  LowPass whooshLp(2500), whooshLp2(700);
  for (size_t i = 0; i < v.size(); ++i) {
    float t = timeOf(i);
    size_t next = (idx + 1) % ring.size();
    float s = ring[idx];
    ring[idx] = 0.497f * (ring[idx] + ring[next]);
    idx = next;
    float n = r.next();
    float whoosh = (whooshLp(n) - whooshLp2(n)) * std::sin(std::min(1.0f, t / 0.3f) * 3.14159f) * 0.8f;
    v[i] = s * std::exp(-t / 0.16f) * 0.9f + whoosh + n * std::exp(-t / 0.004f) * 0.3f;
  }
  normalize(v, 0.8f);
  return v;
}

std::vector<float> explosion() {
  auto v = buffer(3.2f);
  Rng r{99};
  LowPass lp1(400), lp2(160), rumble(70);
  float phase = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    float t = timeOf(i), n = r.next();
    phase += kTau * (40.0f + 60.0f * std::exp(-t / 0.05f)) / kRate;
    float boom = std::sin(phase) * std::exp(-t / 0.35f) * 1.4f;
    float blast = lp2(lp1(n)) * std::exp(-t / 0.6f) * 6.0f;
    float roll = rumble(n) * std::exp(-t / 1.3f) * 9.0f;
    float crack = n * std::exp(-t / 0.004f);
    v[i] = boom + blast + roll + crack;
  }
  echoes(v, {{0.12f, 0.35f}, {0.3f, 0.25f}, {0.6f, 0.15f}});
  saturate(v, 2.5f);
  normalize(v);
  return v;
}

std::vector<float> reloadSound() {
  auto v = buffer(2.0f);
  addClick(v, 0.15f, 2200, 0.7f, 1);  // magazine release
  addClick(v, 0.24f, 1500, 0.8f, 2);  // magazine out
  Rng r{3};
  LowPass fr(1800);
  for (size_t i = static_cast<size_t>(0.6f * kRate); i < static_cast<size_t>(0.85f * kRate); ++i)  // new mag slides in
    v[i] += fr(r.next()) * 0.6f * std::sin((timeOf(i) - 0.6f) / 0.25f * 3.14159f);
  addClick(v, 0.88f, 1900, 0.9f, 4);  // seated
  addClick(v, 0.93f, 2700, 0.6f, 5);
  addClick(v, 1.45f, 1300, 0.8f, 6);  // charging handle back
  addClick(v, 1.6f, 2100, 1.0f, 7);   // and forward
  normalize(v, 0.7f);
  return v;
}

struct Tone {
  float start, freq, decay, gain;
};

std::vector<float> tones(float length, std::initializer_list<Tone> parts) {
  auto v = buffer(length);
  for (const Tone& tn : parts) {
    const float start = tn.start, f = tn.freq, decay = tn.decay, gain = tn.gain;
    size_t s = static_cast<size_t>(start * kRate);
    for (size_t i = s; i < v.size(); ++i) {
      float t = timeOf(i - s);
      v[i] += std::sin(kTau * f * t) * std::exp(-t / decay) * gain * std::min(1.0f, t / 0.002f);
    }
  }
  return v;
}

std::vector<float> knock(float f1, float f2, float noiseHz, float noiseDecay, uint32_t seed, float length = 0.35f) {
  auto v = tones(length, {{0, f1, 0.1f, 0.8f}, {0, f2, 0.06f, 0.5f}, {0, f2 * 1.63f, 0.035f, 0.3f}});
  Rng r{seed};
  LowPass lp(noiseHz);
  for (size_t i = 0; i < v.size(); ++i) v[i] += lp(r.next()) * std::exp(-timeOf(i) / noiseDecay) * 2.5f;
  return v;
}

// Seamless loop: the last `fade` seconds are blended into the start, then cut off.
void makeLoop(std::vector<float>& v, float fade) {
  size_t f = static_cast<size_t>(fade * kRate);
  if (v.size() <= 2 * f) return;
  size_t n = v.size() - f;
  for (size_t i = 0; i < f; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(f);
    v[i] = v[i] * std::sqrt(t) + v[n + i] * std::sqrt(1 - t);
  }
  v.resize(n);
}

std::vector<float> ambWind() {
  auto v = buffer(13.0f);
  Rng r{70};
  float z1 = 0, z2 = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    float t = timeOf(i), n = r.next();
    float gust = 0.55f + 0.3f * std::sin(t * 0.7f) + 0.15f * std::sin(t * 1.9f + 1.3f);
    float a = std::exp(-kTau * (250.0f + 350.0f * gust) / kRate);  // brighter in gusts
    z1 = n * (1 - a) + z1 * a;
    z2 = z1 * (1 - a) + z2 * a;
    v[i] = z2 * gust * 3.0f;
  }
  makeLoop(v, 1.0f);
  normalize(v, 0.5f);
  return v;
}

std::vector<float> ambOcean() {
  auto v = buffer(17.0f);  // a wave every ~5.3 s
  Rng r{71};
  LowPass roar(380), roar2(200), fizz(5000), fizzLo(1500);
  for (size_t i = 0; i < v.size(); ++i) {
    float t = timeOf(i), n = r.next();
    float ph = std::fmod(t, 5.33f) / 5.33f;
    float swell = std::pow(std::sin(ph * 3.14159f), 2.0f);
    float breakAt = std::max(0.0f, std::sin((ph - 0.35f) * 3.14159f / 0.65f));  // the break and wash after the crest
    v[i] = roar2(roar(n)) * (0.4f + 1.4f * swell) * 2.5f + (fizz(n) - fizzLo(n)) * breakAt * breakAt * 0.5f;
  }
  makeLoop(v, 1.0f);
  normalize(v, 0.55f);
  return v;
}

std::vector<float> ambBirds() {
  auto v = buffer(15.0f);
  Rng r{72};
  auto u01 = [&]() { return r.next() * 0.5f + 0.5f; };
  // A few songbirds: short whistled phrases at random times.
  for (int k = 0; k < 14; ++k) {
    float start = u01() * 13.0f;
    float base = 2600.0f + 1800.0f * u01();
    int notes = 2 + static_cast<int>(u01() * 5);
    float gain = 0.15f + 0.25f * u01();
    for (int nn = 0; nn < notes; ++nn) {
      float ns = start + static_cast<float>(nn) * 0.11f, len = 0.07f + 0.04f * u01();
      float sweep = r.next() * 900.0f;
      float phase = 0;
      for (size_t i = static_cast<size_t>(ns * kRate); i < v.size() && i < static_cast<size_t>((ns + len) * kRate); ++i) {
        float t = timeOf(i) - ns, u = t / len;
        float f = base + sweep * u + 120.0f * std::sin(t * kTau * 38.0f);
        phase += kTau * f / kRate;
        v[i] += std::sin(phase) * std::sin(u * 3.14159f) * gain;
      }
    }
  }
  echoes(v, {{0.09f, 0.25f}, {0.21f, 0.12f}});
  makeLoop(v, 1.0f);
  normalize(v, 0.35f);
  return v;
}

std::vector<float> ambNight() {
  auto v = buffer(9.0f);
  Rng r{73};
  // Crickets: trains of ~4.5 kHz pulses, several insects slightly out of step.
  for (int bug = 0; bug < 4; ++bug) {
    float f = 4300.0f + 500.0f * static_cast<float>(bug), period = 0.55f + 0.13f * static_cast<float>(bug);
    float offset = 0.17f * static_cast<float>(bug), gain = 0.25f / (1 + static_cast<float>(bug) * 0.4f);
    for (size_t i = 0; i < v.size(); ++i) {
      float t = timeOf(i) + offset;
      float inChirp = std::fmod(t, period);
      if (inChirp > 0.18f) continue;
      float pulse = std::sin(std::fmod(inChirp, 0.045f) / 0.045f * 3.14159f);
      v[i] += std::sin(kTau * f * t) * pulse * pulse * gain;
    }
  }
  LowPass lp(300);
  for (size_t i = 0; i < v.size(); ++i) v[i] += lp(r.next()) * 0.25f;  // soft night air
  makeLoop(v, 0.6f);
  normalize(v, 0.3f);
  return v;
}

std::vector<float> synth(Sound s) {
  std::vector<float> v;
  switch (s) {
    case Sound::Pistol: v = gunshot({1.0f, 2600, 0.032f, 115, 0.6f, 0.26f, 0.9f}, 10); break;
    case Sound::Revolver: v = gunshot({1.0f, 1800, 0.05f, 85, 0.95f, 0.42f, 1.2f}, 11); break;
    case Sound::SMG: v = gunshot({0.85f, 3100, 0.022f, 135, 0.45f, 0.18f, 0.55f}, 12); break;
    case Sound::Shotgun:  // blast, then the pump racked back and forward
      v = gunshot({0.75f, 1100, 0.085f, 65, 1.1f, 0.55f, 1.4f}, 13);
      addClick(v, 0.52f, 900, 0.35f, 50);
      addClick(v, 0.66f, 1300, 0.4f, 51);
      break;
    case Sound::AssaultRifle: v = gunshot({1.0f, 2200, 0.038f, 100, 0.8f, 0.34f, 0.9f}, 14); break;
    case Sound::HuntingRifle:  // shot, then the bolt cycled
      v = gunshot({1.0f, 1600, 0.06f, 80, 1.0f, 0.65f, 1.5f}, 15);
      addClick(v, 0.62f, 2400, 0.22f, 52);
      addClick(v, 0.74f, 1700, 0.25f, 53);
      addClick(v, 0.9f, 2100, 0.25f, 54);
      break;
    case Sound::Sniper:
      v = gunshot({1.0f, 1350, 0.07f, 62, 1.1f, 0.95f, 2.0f}, 16);
      addClick(v, 1.0f, 2300, 0.18f, 55);
      addClick(v, 1.13f, 1600, 0.2f, 56);
      addClick(v, 1.32f, 2000, 0.2f, 57);
      break;
    case Sound::Silenced: v = silencedShot(); break;
    case Sound::Bow: v = bowShot(); break;
    case Sound::Explosion: v = explosion(); break;
    case Sound::Reload: v = reloadSound(); break;
    case Sound::DryFire:
      v = buffer(0.15f);
      addClick(v, 0.0f, 2400, 0.6f, 20);
      addClick(v, 0.014f, 1500, 0.4f, 21);
      break;
    case Sound::Swing: {
      v = buffer(0.28f);
      Rng r{30};
      LowPass hi(1600), lo(350);
      for (size_t i = 0; i < v.size(); ++i) {
        float n = r.next(), e = std::sin(timeOf(i) / 0.26f * 3.14159f);
        v[i] = (hi(n) - lo(n)) * e * e * 2.0f;
      }
      normalize(v, 0.5f);
      break;
    }
    case Sound::HitWood: v = knock(185, 330, 1200, 0.015f, 31); normalize(v, 0.8f); break;
    case Sound::HitStone: {
      v = knock(900, 1430, 5000, 0.01f, 32, 0.3f);
      for (int k = 0; k < 4; ++k) addClick(v, 0.03f + 0.025f * k, 2500.0f + 500.0f * k, 0.3f, 40u + k);  // debris
      normalize(v, 0.8f);
      break;
    }
    case Sound::HitFlesh: v = knock(90, 140, 500, 0.04f, 33, 0.22f); normalize(v, 0.7f); break;
    case Sound::Hitmarker: v = tones(0.08f, {{0, 2300, 0.02f, 0.7f}, {0, 4600, 0.012f, 0.25f}}); break;
    case Sound::Headshot: v = tones(0.3f, {{0, 2300, 0.02f, 0.6f}, {0.02f, 3200, 0.12f, 0.5f}, {0.02f, 6400, 0.05f, 0.15f}}); break;
    case Sound::Kill: v = tones(0.55f, {{0, 880, 0.2f, 0.5f}, {0.08f, 1320, 0.28f, 0.5f}}); break;
    case Sound::Build: {
      v = knock(160, 290, 1000, 0.015f, 34, 0.45f);
      auto second = knock(150, 270, 1000, 0.015f, 35, 0.45f);
      size_t off = static_cast<size_t>(0.13f * kRate);
      for (size_t i = off; i < v.size(); ++i) v[i] += second[i - off] * 0.7f;
      normalize(v, 0.8f);
      break;
    }
    case Sound::Door: {
      v = buffer(0.6f);
      LowPass lp(1100);
      float phase = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        float t = timeOf(i);
        float f = 210.0f + 18.0f * std::sin(t * kTau * 6.0f) + 60.0f * t;  // creak
        phase += f / kRate;
        float saw = 2.0f * (phase - std::floor(phase)) - 1.0f;
        v[i] = lp(saw) * std::sin(std::min(1.0f, t / 0.45f) * 3.14159f) * 0.7f;
      }
      addClick(v, 0.47f, 1200, 1.0f, 36);  // latch
      normalize(v, 0.6f);
      break;
    }
    case Sound::Pickup: {
      v = buffer(0.22f);
      Rng r{37};
      LowPass lp(1500);
      for (size_t i = 0; i < v.size(); ++i) {
        float n = r.next(), t = timeOf(i);
        v[i] = (n - lp(n)) * (0.5f + 0.5f * std::sin(t * 70.0f)) * std::exp(-t / 0.08f);
      }
      normalize(v, 0.5f);
      break;
    }
    case Sound::Footstep: {
      v = buffer(0.16f);
      Rng r{38};
      LowPass lp(650), crunch(3200);
      for (size_t i = 0; i < v.size(); ++i) {
        float n = r.next(), t = timeOf(i);
        v[i] = lp(n) * std::exp(-t / 0.03f) * 3.0f + crunch(n) * std::exp(-t / 0.012f) * 0.6f;
      }
      normalize(v, 0.45f);
      break;
    }
    case Sound::Beep: v = tones(0.1f, {{0, 2000, 0.05f, 0.5f}, {0, 4000, 0.02f, 0.1f}}); break;
    case Sound::Unlock: v = tones(0.2f, {{0, 1200, 0.04f, 0.5f}, {0.07f, 1800, 0.06f, 0.5f}}); break;
    case Sound::Denied: {
      v = buffer(0.3f);
      for (size_t i = 0; i < v.size(); ++i) {
        float t = timeOf(i);
        v[i] = (std::sin(kTau * 180.0f * t) > 0 ? 0.35f : -0.35f) * std::min(1.0f, (0.3f - t) / 0.05f);
      }
      break;
    }
    case Sound::StepWood: {  // hollow knock of boots on planks
      v = knock(140, 260, 900, 0.012f, 60, 0.18f);
      for (size_t i = 0; i < v.size(); ++i) v[i] *= std::exp(-timeOf(i) / 0.05f);
      normalize(v, 0.45f);
      break;
    }
    case Sound::StepMetal: {  // ringing deck plate
      v = tones(0.4f, {{0, 520, 0.12f, 0.3f}, {0, 1310, 0.07f, 0.25f}, {0, 2870, 0.04f, 0.15f}});
      auto k = knock(180, 330, 2500, 0.01f, 61, 0.4f);
      for (size_t i = 0; i < v.size() && i < k.size(); ++i) v[i] += k[i] * 0.5f;
      normalize(v, 0.45f);
      break;
    }
    case Sound::Swim: {  // a stroke: slosh and trickle
      v = buffer(0.7f);
      Rng r{62};
      LowPass lo(900), hi(4000);
      for (size_t i = 0; i < v.size(); ++i) {
        float t = timeOf(i), n = r.next();
        float env = std::sin(std::min(1.0f, t / 0.5f) * 3.14159f);
        float bubbles = std::sin(kTau * (600 + 900 * std::fabs(r.next())) * t) * (r.next() > 0.97f ? 1.0f : 0.0f);
        v[i] = lo(n) * env * 1.6f + (hi(n) - lo(n)) * env * env * 0.4f + bubbles * 0.15f * env;
      }
      normalize(v, 0.45f);
      break;
    }
    case Sound::StepGrass: {  // boot through grass: a soft heel, then blades swishing and a few snapping stems
      v = buffer(0.3f);
      Rng r{80};
      LowPass hi(6500), lo(1400), heel(220);
      for (size_t i = 0; i < v.size(); ++i) {
        float t = timeOf(i), n = r.next();
        float swish = (hi(n) - lo(n)) * std::exp(-t / 0.07f) * std::min(1.0f, t / 0.012f);
        v[i] = heel(n) * std::exp(-t / 0.02f) * 2.2f + swish * 1.4f;
      }
      for (int k = 0; k < 5; ++k) addClick(v, 0.01f + 0.017f * static_cast<float>(k), 3000.0f + 700.0f * static_cast<float>(k), 0.12f, 81u + k);
      normalize(v, 0.42f);
      break;
    }
    case Sound::StepSand: {  // soft, dull and gritty: grains shifting under the foot, no click
      v = buffer(0.32f);
      Rng r{85};
      LowPass lp(2600), lp2(500), sub(160);
      float grain = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        float t = timeOf(i), n = r.next();
        if (i % 60 == 0) grain = 0.4f + 0.6f * std::fabs(r.next());  // granular flutter
        float body = (lp(n) - lp2(n)) * grain * std::exp(-t / 0.09f) * std::min(1.0f, t / 0.03f);
        v[i] = body * 1.8f + sub(n) * std::exp(-t / 0.05f) * 1.2f;
      }
      normalize(v, 0.38f);
      break;
    }
    case Sound::StepRock: {  // hard heel tap on stone, then a scrape of loose gravel
      v = knock(420, 980, 5200, 0.006f, 86, 0.28f);
      for (size_t i = 0; i < v.size(); ++i) v[i] *= std::exp(-timeOf(i) / 0.035f);
      for (int k = 0; k < 7; ++k)
        addClick(v, 0.02f + 0.011f * static_cast<float>(k), 2200.0f + 450.0f * static_cast<float>(k % 4), 0.18f, 87u + k);
      normalize(v, 0.45f);
      break;
    }
    case Sound::StepSnow: {  // packed-snow crunch: dense little crackles with a squeak
      v = buffer(0.35f);
      Rng r{95};
      LowPass lp(3500), lo(700);
      for (size_t i = 0; i < v.size(); ++i) {
        float t = timeOf(i), n = r.next();
        float env = std::sin(std::min(1.0f, t / 0.2f) * 3.14159f);
        float crackle = (r.next() > 0.82f ? 1.0f : 0.15f) * (lp(n) - lo(n));
        v[i] = crackle * env * 1.6f + std::sin(kTau * 900.0f * t) * env * env * 0.05f;
      }
      normalize(v, 0.42f);
      break;
    }
    case Sound::AmbWind: v = ambWind(); break;
    case Sound::AmbOcean: v = ambOcean(); break;
    case Sound::AmbBirds: v = ambBirds(); break;
    case Sound::AmbNight: v = ambNight(); break;
    case Sound::HitArmor: {  // bullet glancing off a plate: metallic ping over a dull thud
      v = tones(0.35f, {{0, 3100, 0.09f, 0.45f}, {0, 4700, 0.05f, 0.25f}, {0.003f, 7300, 0.03f, 0.12f}});
      auto thud = knock(220, 400, 1800, 0.012f, 39, 0.3f);
      for (size_t i = 0; i < v.size() && i < thud.size(); ++i) v[i] += thud[i] * 0.5f;
      normalize(v, 0.6f);
      break;
    }
    case Sound::Equip: {  // cloth rustle and a buckle
      v = buffer(0.35f);
      Rng r{41};
      LowPass hi(2500), lo(500);
      for (size_t i = 0; i < v.size(); ++i) {
        float n = r.next(), t = timeOf(i);
        v[i] = (hi(n) - lo(n)) * std::sin(std::min(1.0f, t / 0.3f) * 3.14159f) * (0.6f + 0.4f * std::sin(t * 55.0f));
      }
      addClick(v, 0.27f, 2200, 0.5f, 42);
      normalize(v, 0.45f);
      break;
    }
    default:
      v = buffer(0.05f);
      break;
  }
  return v;
}

}  // namespace

static bool writeWav(const std::vector<float>& v, const std::string& path);

bool writeSoundFile(Sound s, const std::string& path) { return writeWav(synth(s), path); }

bool hasFarVariant(Sound s) { return (s >= Sound::Bow && s <= Sound::Silenced) || s == Sound::Explosion; }

// A shot heard from far away: the crack and highs are gone, and it rolls back off the hills.
bool writeFarSoundFile(Sound s, const std::string& path) {
  std::vector<float> nearShot = synth(s);
  std::vector<float> v(nearShot.size() + static_cast<size_t>(1.8f * kRate), 0.0f);
  LowPass a(650), b(500), c(380);
  for (size_t i = 0; i < v.size(); ++i) v[i] = c(b(a(i < nearShot.size() ? nearShot[i] : 0.0f)));
  echoes(v, {{0.28f, 0.45f}, {0.6f, 0.35f}, {0.95f, 0.25f}, {1.4f, 0.15f}});
  normalize(v, 0.8f);
  return writeWav(v, path);
}

static bool writeWav(const std::vector<float>& v, const std::string& path) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  auto u32 = [&](uint32_t x) { f.write(reinterpret_cast<const char*>(&x), 4); };
  auto u16 = [&](uint16_t x) { f.write(reinterpret_cast<const char*>(&x), 2); };
  const uint32_t dataBytes = static_cast<uint32_t>(v.size() * 2);
  f.write("RIFF", 4);
  u32(36 + dataBytes);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);  // PCM
  u16(1);  // mono
  u32(kRate);
  u32(kRate * 2);
  u16(2);
  u16(16);
  f.write("data", 4);
  u32(dataBytes);
  for (float x : v) {
    int s16 = static_cast<int>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
    u16(static_cast<uint16_t>(static_cast<int16_t>(s16)));
  }
  return static_cast<bool>(f);
}

}  // namespace sg
