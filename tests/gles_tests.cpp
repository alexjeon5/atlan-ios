// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gles.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
using namespace atlan;
constexpr std::uint32_t stack = 0x80000, sentinel = 0xffff0000, data = 0x4000, trap_base = 0xf0000000;

struct Recorder : GlBackend {
    std::vector<GlCall> calls;
    GlResult execute(const GlCall& call) override {
        calls.push_back(call);
        GlResult r;
        if (call.op == GlOp::get_string) r.text = "Test GL";
        if (call.op == GlOp::get_uniform_location) r.value = 7;
        if (call.op == GlOp::get_program_iv) r.value = 1;
        return r;
    }
};

struct Fixture {
    Runtime runtime;
    ImportDispatcher dispatcher;
    Recorder backend;
    Gles2 gles{backend, {data + 0x3000}};
    std::uint32_t next_trap = trap_base;
    Fixture() {
        runtime.memory.map(data, 0x4000, Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, Permissions::read_write);
        gles.install(dispatcher);
    }
    std::uint32_t trap(const char* name) { dispatcher.bind_trap(next_trap, name); const auto at = next_trap; next_trap += 4; return at; }
    CallOutcome call(const char* name, std::initializer_list<std::uint32_t> args) {
        const auto address = trap(name);
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, address, values, stack, sentinel, 10000);
    }
    void put(std::uint32_t at, const void* p, std::size_t n) {
        std::vector<std::byte> b(n);
        std::memcpy(b.data(), p, n);
        check(runtime.memory.write(at, b), "write guest memory");
    }
    std::uint32_t get32(std::uint32_t at) {
        std::array<std::byte, 4> b{};
        check(runtime.memory.read(at, b), "read guest word");
        std::uint32_t v;
        std::memcpy(&v, b.data(), 4);
        return v;
    }
};
std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }
}

int main() {
    try {
        Fixture f;
        check(f.call("glClearColor", {bits(0.25F), bits(0.5F), bits(0.75F), bits(1.F)}).stop == Stop::returned, "clear color returns");
        check(f.backend.calls.back().op == GlOp::clear_color && f.backend.calls.back().f[2] == 0.75F, "clear color floats decoded");
        check(f.call("glViewport", {0, 0, 2556, 1179}).stop == Stop::returned && f.backend.calls.back().a[2] == 2556, "viewport decoded");

        check(f.call("glGenTextures", {2, data}).stop == Stop::returned, "gen textures");
        check(f.get32(data) == 1 && f.get32(data + 4) == 2, "texture ids are written");
        check(f.call("glDeleteTextures", {1, data}).stop == Stop::returned && f.backend.calls.back().id == 1, "delete texture reads ids");

        const char* source = "void main(){}";
        f.put(data + 0x100, source, std::strlen(source) + 1);
        const std::uint32_t source_ptr = data + 0x100;
        f.put(data + 0x200, &source_ptr, 4);
        check(f.call("glShaderSource", {5, 1, data + 0x200, 0}).stop == Stop::returned && f.backend.calls.back().text == source, "shader source gathered");

        const std::uint8_t pixels[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        f.put(data + 0x300, pixels, sizeof pixels);
        check(f.call("glTexImage2D", {0xDE1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, data + 0x300}).stop == Stop::returned &&
              f.backend.calls.back().data.size() == 16, "texture pixels gathered (stack arguments included)");

        // client-side attribute: 3 vertices of 2 floats, drawn with glDrawArrays
        const float verts[6] = {0, 0, 1, 0, 0, 1};
        f.put(data + 0x400, verts, sizeof verts);
        check(f.call("glEnableVertexAttribArray", {0}).stop == Stop::returned, "enable attrib");
        check(f.call("glVertexAttribPointer", {0, 2, 0x1406, 0, 0, data + 0x400}).stop == Stop::returned, "attrib pointer");
        check(f.call("glDrawArrays", {4, 0, 3}).stop == Stop::returned, "draw arrays");
        const auto& draw = f.backend.calls.back();
        check(draw.op == GlOp::draw_arrays && draw.attribs.size() == 1 && draw.attribs[0].bytes.size() == 24, "client-side vertices gathered");

        const char* name = "uMvp";
        f.put(data + 0x500, name, 5);
        check(f.call("glGetUniformLocation", {3, data + 0x500}).stop == Stop::returned && f.runtime.cpu.registers[0] == 7, "uniform lookup returns the backend location");
        check(f.backend.calls.back().text == "uMvp", "uniform name decoded");

        check(f.call("glGetString", {0x1F01}).stop == Stop::returned && f.runtime.cpu.registers[0] == data + 0x3000, "glGetString returns the scratch string");
        check(f.call("eglGetProcAddress", {data}).stop == Stop::service_error, "eglGetProcAddress stops explicitly");
        std::cout << "gles tests passed: " << checks << " checks\n";
    } catch (const std::exception& e) {
        std::cerr << "gles test failed: " << e.what() << '\n';
        return 1;
    }
}
