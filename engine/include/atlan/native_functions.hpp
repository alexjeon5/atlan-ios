// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/elf.hpp"
#include "atlan/imports.hpp"
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace atlan {
// Host implementations that replace functions inside the guest image (ImportDispatcher::replace_function).
//
// Nothing is assumed about a particular build of the image. A function is found by its exported name, or else
// by a byte signature the embedder supplies; then, before it is replaced, the guest's own code and the host
// implementation run on the same inputs and must agree. A function that is not found or does not agree stays
// interpreted, so an image whose code changed runs as before, only slower.

// Runs the guest's code and the host services on chosen inputs while a set is being verified.
class NativeProbe {
public:
    NativeProbe(ImportDispatcher& dispatcher, Runtime& runtime, std::uint32_t stack_top, std::uint32_t return_target,
                std::uint32_t scratch, std::uint64_t budget)
        : dispatcher_(dispatcher), runtime_(runtime), stack_top_(stack_top), return_target_(return_target),
          scratch_(scratch), budget_(budget) {}
    struct Result {
        std::uint32_t r0{}, r1{};
        friend bool operator==(const Result&, const Result&) = default;
    };
    // The guest function at `address` (bit 0: Thumb). nullopt: it did not return normally.
    [[nodiscard]] std::optional<Result> guest(std::uint32_t address, std::span<const std::uint32_t> arguments);
    // The host service with the same arguments. nullopt: it failed or declined the call.
    [[nodiscard]] std::optional<Result> native(const ImportService& service, std::span<const std::uint32_t> arguments);
    // Read-write guest memory for arguments and results, `scratch_bytes` long, mapped during verification only.
    static constexpr std::uint32_t scratch_bytes = 0x40000;
    [[nodiscard]] std::uint32_t scratch() const noexcept { return scratch_; }
    [[nodiscard]] Memory& memory() noexcept { return runtime_.memory; }

private:
    ImportDispatcher& dispatcher_;
    Runtime& runtime_;
    std::uint32_t stack_top_, return_target_, scratch_;
    std::uint64_t budget_;
};

// Functions that are replaced together or not at all (for example the calls that share one stream state).
struct NativeSet {
    struct Member {
        std::string name;                  // counter label; also the key of a signature
        std::vector<std::string> symbols;  // exported names, any of which locates it
        ImportService service;
        bool required{true};               // a set without one of its required members is not installed
        // Optional register-only form of `service` that the block cache calls in place of the trap
        // (ImportDispatcher::replace_function_inline); false declines to `service`.
        InlineRun inline_run{};
    };
    std::string name;
    std::vector<Member> members;
    // Members that replace one instruction instead of a whole function, by member name (their `service` is unused):
    // the located address is that instruction (bit 0: Thumb; not inside an IT block). At install the factory gets the
    // instruction's encoding (an A32 word; for Thumb the first halfword in the low 16 bits and the second, if any,
    // above) and returns the service that runs in its place, or an empty one to leave it alone (the set then counts
    // as differing from the host). That service sees the CPU state at the instruction and may change registers and
    // memory; execution then continues with the next instruction, never at LR.
    std::map<std::string, std::function<ImportService(std::uint32_t encoding)>> instructions;
    // Compares guest and host on the located members (member name -> address, bit 0: Thumb). `call` runs member
    // `name` either as guest code or as its host service. Null: installed without a check.
    using Call = std::function<std::optional<NativeProbe::Result>(const std::string& name, std::span<const std::uint32_t> arguments)>;
    std::function<bool(NativeProbe& probe, const Call& guest, const Call& native)> verify;
    // Looks at the located code before verification (member name -> first address, bit 0: Thumb), for a host
    // implementation that depends on what exactly the guest function does. false: the set is left interpreted as one
    // whose guest code differs. Null: nothing to look at.
    std::function<bool(Memory& memory, const std::map<std::string, std::uint32_t>& located)> inspect;
};

// A function start as bytes of code: `mask` selects the bits that must equal `bytes`.
// Or, with `symbol` set, the exported name that locates it (a name the embedder keeps out of its sources).
struct NativeSignature {
    bool thumb{};
    std::vector<std::uint8_t> bytes, mask;
    std::string symbol;
};
// Parses lines "NAME arm|thumb HEX", HEX being byte pairs with ?? for any byte, and "NAME symbol EXPORTED";
// '#' starts a comment.
// nullopt with `why` set if a line is malformed.
[[nodiscard]] std::optional<std::map<std::string, NativeSignature>> parse_native_signatures(const std::string& text, std::string& why);

struct NativeOptions {
    std::uint32_t stack_top{}, return_target{};  // for running guest code during verification
    std::uint32_t scratch{0x6f000000};           // unused guest addresses, NativeProbe::scratch_bytes long
    std::uint64_t budget{20000000};              // instructions per guest call during verification
    std::uint32_t code_base{};                   // where signatures are searched: [code_base, code_base + code_bytes)
    std::uint64_t code_bytes{};
    std::map<std::string, NativeSignature> signatures;
    std::vector<std::string> skip;                // set names to leave interpreted (diagnosis)
    std::function<void(const std::string&)> log;  // one summary line, if the image has any of the functions
};
// Locates, verifies and installs each set. Call after the import services are bound and before the guest runs.
// Returns the number of functions replaced.
unsigned install_native_functions(ImportDispatcher& dispatcher, Runtime& runtime, std::span<const elf::Symbol> exports,
                                  std::vector<NativeSet> sets, const NativeOptions& options);

// Sets for code that toolchains and common libraries link into an image, by their standard names: the integer
// division helpers of the ARM run-time ABI, and zlib's checksums and inflate.
[[nodiscard]] std::vector<NativeSet> runtime_library_sets();
// Sets for image code by its libraries' names: libpng's row filters and the PowerVR tools' ETC1 decoder.
[[nodiscard]] std::vector<NativeSet> image_library_sets();
}
