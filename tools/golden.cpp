// Writes reference renders to tests/golden/<name>.f32 (raw little-endian float32).
// Usage: tonic2_golden --update <golden-dir>
#include <cstdio>
#include <cstring>
#include <string>

#include "../tests/renders.hpp"

int main(int argc, char** argv) {
  if (argc != 3 || std::strcmp(argv[1], "--update") != 0) {
    std::fprintf(stderr, "usage: %s --update <golden-dir>\n", argv[0]);
    return 2;
  }
  for (const auto& r : renders::all()) {
    const auto data = r.fn();
    const std::string path = std::string(argv[2]) + "/" + r.name + ".f32";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::perror(path.c_str()); return 1; }
    std::fwrite(data.data(), sizeof(float), data.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%zu samples)\n", path.c_str(), data.size());
  }
  return 0;
}
