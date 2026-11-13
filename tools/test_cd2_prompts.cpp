// Host test of Crackdown 2's prompt patching (crackdown-recomp/cd2/src/cd2_prompts_core.cpp):
//   test_cd2_prompts <cd2_prompts.bin> <UI folder> <out folder>
// patches ui.pack, pause.pack and initial.pack as the game does at start-up and writes them out, so they can be
// checked (Downloads/Crackdown/Extras/cd2tools: ex.py + swfshape.py) before a console run.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cd2_prompts_core.h"

static std::vector<uint8_t> Read(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

static void Write(const std::string& p, const std::vector<uint8_t>& d) {
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  std::vector<cd2::prompts::Rule> rules;
  if (!cd2::prompts::ParseRules(Read(argv[1]), rules)) return std::puts("bad rules"), 1;
  int total = 0;
  for (const char* name : {"ui", "pause", "initial"}) {
    const std::string in = std::string(argv[2]) + "/" + name + ".pack";
    cd2::prompts::PackResult r;
    std::string err;
    if (!cd2::prompts::PatchPack(Read(in + ".toc"), Read(in), rules, r, err)) return std::printf("%s: %s\n", name, err.c_str()), 1;
    std::printf("%s.pack: %d movies, %d shapes, %zu -> %zu bytes\n", name, r.movies, r.shapes, Read(in).size(), r.pack.size());
    Write(std::string(argv[3]) + "/" + name + ".pack", r.pack);
    Write(std::string(argv[3]) + "/" + name + ".pack.toc", r.toc);
    total += r.shapes;
  }
  std::printf("%zu rules, %d shapes replaced\n", rules.size(), total);
  return 0;
}
