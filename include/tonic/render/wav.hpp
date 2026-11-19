// Minimal WAV file writer/reader (32-bit float PCM, interleaved on disk). Control thread only.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tonic::render {

namespace detail {
inline void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
inline void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
inline uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
}  // namespace detail

// channels[c][frame]; all channels must have the same length. Returns false on I/O error.
inline bool writeWavFloat(const std::string& path, const std::vector<std::vector<float>>& channels, int sampleRate) {
  const uint16_t nch = static_cast<uint16_t>(channels.size());
  const uint32_t frames = nch ? static_cast<uint32_t>(channels[0].size()) : 0;
  const uint32_t dataBytes = frames * nch * 4u;
  std::vector<uint8_t> b;
  b.reserve(44 + dataBytes);
  b.insert(b.end(), {'R', 'I', 'F', 'F'}); detail::put32(b, 36 + dataBytes);
  b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); detail::put32(b, 16);
  detail::put16(b, 3);  // IEEE float
  detail::put16(b, nch);
  detail::put32(b, static_cast<uint32_t>(sampleRate));
  detail::put32(b, static_cast<uint32_t>(sampleRate) * nch * 4u);
  detail::put16(b, static_cast<uint16_t>(nch * 4));
  detail::put16(b, 32);
  b.insert(b.end(), {'d', 'a', 't', 'a'}); detail::put32(b, dataBytes);
  for (uint32_t f = 0; f < frames; ++f)
    for (uint16_t c = 0; c < nch; ++c) {
      uint32_t bits;
      std::memcpy(&bits, &channels[c][f], 4);
      detail::put32(b, bits);
    }
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  const bool ok = std::fwrite(b.data(), 1, b.size(), fp) == b.size();
  return std::fclose(fp) == 0 && ok;
}

struct WavData {
  int sampleRate = 0;
  std::vector<std::vector<float>> channels;
};

// Reads files written by writeWavFloat (float32, plain fmt/data chunks). Returns false otherwise.
inline bool readWavFloat(const std::string& path, WavData& out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::vector<uint8_t> b;
  uint8_t buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, fp)) > 0;) b.insert(b.end(), buf, buf + n);
  std::fclose(fp);
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
  if (detail::get16(&b[20]) != 3 || detail::get16(&b[34]) != 32 || std::memcmp(&b[36], "data", 4)) return false;
  const int nch = detail::get16(&b[22]);
  const uint32_t bytes = detail::get32(&b[40]);
  if (nch <= 0 || 44 + size_t(bytes) > b.size()) return false;
  out.sampleRate = static_cast<int>(detail::get32(&b[24]));
  const size_t frames = bytes / (4u * nch);
  out.channels.assign(static_cast<size_t>(nch), std::vector<float>(frames));
  for (size_t f = 0; f < frames; ++f)
    for (int c = 0; c < nch; ++c) {
      const uint32_t bits = detail::get32(&b[44 + (f * nch + c) * 4]);
      std::memcpy(&out.channels[static_cast<size_t>(c)][f], &bits, 4);
    }
  return true;
}

}  // namespace tonic::render
