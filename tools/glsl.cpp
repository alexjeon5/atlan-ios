// SPDX-License-Identifier: GPL-2.0-or-later
// atlan-glsl VERTEX.glsl FRAGMENT.glsl — prints the MSL translation (or the compile/link log).
#include "atlan/glsl.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

static std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: atlan-glsl VERTEX FRAGMENT\n"; return 2; }
    using namespace atlan::glsl;
    const auto vs = compile(Stage::vertex, slurp(argv[1]));
    const auto fs = compile(Stage::fragment, slurp(argv[2]));
    if (!vs.ok) { std::cerr << "vertex: " << vs.log; return 1; }
    if (!fs.ok) { std::cerr << "fragment: " << fs.log; return 1; }
    const auto linked = link(vs, fs);
    std::cerr << linked.log;
    if (!linked.ok) return 1;
    std::cout << linked.msl;
    std::cerr << "uniform_bytes=" << linked.uniform_bytes << " attributes=" << linked.attributes.size() << " samplers=" << linked.samplers.size() << '\n';
    return 0;
}
