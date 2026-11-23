// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/elf.hpp"
#include "atlan/runtime.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
void put16(std::vector<std::byte>& data, std::size_t at, std::uint16_t value) {
    data.at(at) = std::byte(value & 255); data.at(at + 1) = std::byte(value >> 8);
}
void put32(std::vector<std::byte>& data, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(at + i) = std::byte((value >> (8 * i)) & 255);
}
std::uint32_t get32(atlan::Memory& memory, std::uint32_t at) {
    std::array<std::byte, 4> bytes{};
    check(memory.read(at, bytes), "read loaded word");
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (i * 8);
    return value;
}
// This entirely synthetic ELF has no section headers. It exercises an
// unaligned writable PT_LOAD, BSS, SysV symbols, REL, PLT and constructors.
std::vector<std::byte> fixture() {
    std::vector<std::byte> data(0x1300);
    put32(data,0,0x464c457f); data[4]=std::byte{1};data[5]=std::byte{1};data[6]=std::byte{1};
    put16(data,16,3);put16(data,18,40);put32(data,20,1);put32(data,28,52);
    put32(data,36,0x05000000);put16(data,40,52);put16(data,42,32);put16(data,44,4);
    const auto ph = [&](unsigned i, std::uint32_t type, std::uint32_t offset,
                        std::uint32_t address, std::uint32_t filesz, std::uint32_t memsz,
                        std::uint32_t flags, std::uint32_t align) {
        const auto at = 52 + i * 32;
        put32(data,at,type);put32(data,at+4,offset);put32(data,at+8,address);
        put32(data,at+16,filesz);put32(data,at+20,memsz);put32(data,at+24,flags);put32(data,at+28,align);
    };
    ph(0,1,0,0,0x800,0x800,5,4096);
    ph(1,1,0x1100,0x2100,0x200,0x1000,6,4096);
    ph(2,2,0x1100,0x2100,0x98,0x98,6,4);
    ph(3,0x6474e552,0x1100,0x2100,0,0x100,4,1); // partial RELRO must not seal its final page
    put32(data,0x400,0xe0800001); // add r0,r0,r1
    put32(data,0x404,0xe12fff1e); // bx lr
    put16(data,0x408,0x3001);put16(data,0x40a,0x4770); // Thumb: adds r0,#1; bx lr
    const std::array<std::pair<std::uint32_t,std::uint32_t>,19> tags{{
        {5,0x200},{10,48},{6,0x280},{11,16},{4,0x300},
        {17,0x340},{18,24},{19,8},{23,0x358},{2,8},{20,17},
        {1,15},{25,0x2230},{27,8},{12,0x400},{14,1},{30,2},{0x6ffffffb,1},{0,0}}};
    for (std::size_t i=0;i<tags.size();++i) {
        put32(data,0x1100+i*8,tags[i].first);put32(data,0x1104+i*8,tags[i].second);
    }
    constexpr char names[]="\0add\0increment\0libexample.so\0external\0weak\0";
    for (std::size_t i=0;i<sizeof(names);++i) data[0x200+i]=std::byte(names[i]);
    const auto symbol = [&](unsigned i, unsigned name, unsigned value, unsigned info, unsigned section) {
        const auto at = 0x280+i*16;
        put32(data,at,name);put32(data,at+4,value);put32(data,at+8,0);
        data[at+12]=std::byte(info);put16(data,at+14,static_cast<std::uint16_t>(section));
    };
    symbol(1,1,0x400,0x12,1);symbol(2,5,0x409,0x12,1);
    symbol(3,29,0,0x12,0);symbol(4,38,0,0x22,0);
    put32(data,0x300,1);put32(data,0x304,5); // nbucket, nchain
    put32(data,0x340,0x2200);put32(data,0x344,23); // RELATIVE
    put32(data,0x348,0x2204);put32(data,0x34c,(2<<8)|2); // ABS32 Thumb + addend
    put32(data,0x350,0x2208);put32(data,0x354,(4<<8)|21); // weak GLOB_DAT
    put32(data,0x358,0x220c);put32(data,0x35c,(3<<8)|22); // imported JUMP_SLOT
    put32(data,0x1200,0x2240);put32(data,0x1204,4);put32(data,0x1208,0x1234);put32(data,0x120c,0x4444);
    // Init array slots themselves are relocated with RELATIVE in a separate test.
    put32(data,0x1230,0);put32(data,0x1234,UINT32_MAX);
    return data;
}
atlan::elf::Options options() {
    atlan::elf::Options options;
    options.resolve = [](std::string_view name, std::uint8_t) -> std::optional<std::uint32_t> {
        if (name=="external") return 0x60000001;
        return std::nullopt;
    };
    return options;
}
template<class Mutate> void rejects(Mutate mutate, const char* message) {
    auto file=fixture();mutate(file);
    bool rejected=false;
    try { (void)atlan::elf::load(file,options()); }
    catch (const std::exception&) { rejected=true; }
    check(rejected,message);
}
void valid_image() {
    auto file=fixture();
    for (const auto bias : {0x10000000U,0x20000000U}) {
        auto settings=options();settings.load_bias=bias;
        auto image=atlan::elf::load(file,settings);
        check(image.relocation_count==4 && image.mapped_bytes==3*4096,"relocation and mapping counts");
        check(image.dependencies.size()==1 && image.dependencies[0]=="libexample.so","DT_NEEDED");
        check(get32(image.memory,bias+0x2200)==bias+0x2240,"RELATIVE load bias");
        check(get32(image.memory,bias+0x2204)==bias+0x40d,"ABS32 preserves Thumb bit and adds A");
        check(get32(image.memory,bias+0x2208)==0,"unresolved weak symbol is zero, not addend");
        check(get32(image.memory,bias+0x220c)==0x60000001,"JUMP_SLOT replaces stale PLT word, preserves Thumb");
        check(get32(image.memory,bias+0x3000)==0,"BSS zero initialized");
        check(image.find("add") && image.find("increment") && !image.find("missing"),"export lookup");
        check(image.initializers.size()==1 && image.initializers[0]==bias+0x400,"DT_INIT catalog, skip init array sentinels");
        check(image.memory.translate(bias+0x400,atlan::MemoryAccess::write)==nullptr,"code sealed RX");
        check(image.memory.translate(bias+0x2200,atlan::MemoryAccess::execute)==nullptr,"data is non executable");
        check(image.memory.translate(bias+0x2200,atlan::MemoryAccess::write)!=nullptr,"partial RELRO keeps trailing mutable page");
        atlan::Runtime runtime;runtime.memory=std::move(image.memory);
        runtime.memory.map(0x70000000,4096,atlan::Permissions::read_write);
        const std::array<std::uint32_t,2> args{20,22};
        const auto returned=runtime.call(image.find("add")->address,args,0x70001000,0xffff0000,10);
        check(returned.event==atlan::Event::function_return && returned.retired==2 && runtime.cpu.registers[0]==42,
              "loaded ARM export executes and returns through AAPCS32 LR");
        const auto thumb=runtime.call(image.find("increment")->address,std::span{args}.first(1),0x70001000,0xffff0000,10);
        check(thumb.event==atlan::Event::function_return && runtime.cpu.registers[0]==21,"loaded Thumb export interworks back to ARM return sentinel");
    }
}
void traps_and_rejection() {
    auto file=fixture();
    bool rejected=false;
    try { (void)atlan::elf::load(file); } catch(const std::exception&) { rejected=true; }
    check(rejected,"default rejects unresolved strong imports");
    atlan::elf::Options trap;trap.unresolved=atlan::elf::UnresolvedImports::diagnostic_traps;
    auto image=atlan::elf::load(file,trap);
    check(image.trapped_imports.size()==1,"diagnostic trap catalog excludes unresolved weak");
    const auto address=get32(image.memory,trap.load_bias+0x220c);
    check(image.import_at(address) && image.import_at(address)->name=="external","trap address maps to import name");
    check(image.memory.translate(address,atlan::MemoryAccess::execute)==nullptr &&
          image.memory.translate(address,atlan::MemoryAccess::read)==nullptr,"imports never become successful no-op stubs");
    atlan::Runtime runtime;runtime.memory=std::move(image.memory);runtime.start(address);
    check(runtime.step().event==atlan::Event::memory_fault,"executing unresolved import traps");
    auto limited=options();limited.max_mapped_bytes=4096;
    rejected=false;
    try { (void)atlan::elf::load(file,limited); } catch(const std::exception&) { rejected=true; }
    check(rejected,"mapping budget enforced");
    auto high=options();high.load_bias=0xfffff000;
    rejected=false;
    try { (void)atlan::elf::load(file,high); } catch(const std::exception&) { rejected=true; }
    check(rejected,"load bias overflow rejected");
    rejects([](auto& d){d.resize(51);},"truncated ELF header");
    rejects([](auto& d){d[5]=std::byte{2};},"big-endian image rejected");
    rejects([](auto& d){put16(d,18,183);},"AArch64 image rejected");
    rejects([](auto& d){put32(d,28,UINT32_MAX);},"program-header range overflow rejected");
    rejects([](auto& d){put32(d,52+16,0x900);},"filesz greater than memsz rejected");
    rejects([](auto& d){put32(d,52+28,3);},"non power-of-two segment alignment");
    rejects([](auto& d){put32(d,52+24,7);},"writable executable load rejected");
    rejects([](auto& d){put32(d,52+32+4,0x1101);},"incongruent offset and address rejected");
    rejects([](auto& d){put32(d,0x1104,0x90000000);},"dynamic string pointer outside image rejected");
    rejects([](auto& d){put32(d,0x304,UINT32_MAX);},"oversized hash chain rejected");
    rejects([](auto& d){put32(d,0x290,1000);},"invalid symbol name rejected");
    rejects([](auto& d){put32(d,0x344,160);},"unsupported IRELATIVE rejected");
    rejects([](auto& d){put32(d,0x344,(999<<8)|2);},"invalid relocation symbol rejected");
    rejects([](auto& d){put32(d,0x340,0x2201);},"unaligned relocation rejected");
    rejects([](auto& d){put32(d,0x348,0x2200);},"duplicate relocation rejected");
    rejects([](auto& d){put32(d,0x340,0x400);},"text relocation rejected");
    rejects([](auto& d){put32(d,0x1100+5*8,7);},"RELA fails explicitly");
    rejects([](auto& d){put32(d,0x1100+18*8,15);},"dynamic array requires DT_NULL");
    rejects([](auto& d){put32(d,0x1230,0x1234);},"unrelocated initializer rejected");
}
void relro_initializers_and_imported_data() {
    auto file=fixture();
    put32(file,52+3*32+20,0xf00); // RELRO [0x2100,0x3000), seals page 0x2000
    // Replace the weak relocation by a relative initializer pointer.
    put32(file,0x350,0x2230);put32(file,0x354,23);put32(file,0x1230,0x409);
    auto image=atlan::elf::load(file,options());
    check(image.initializers.size()==2 && image.initializers[1]==0x10000409,
          "relocated Thumb constructor retains instruction-set bit");
    check(image.memory.translate(0x10002200,atlan::MemoryAccess::write)==nullptr &&
          image.memory.translate(0x10003000,atlan::MemoryAccess::write)!=nullptr,
          "RELRO seals complete pages and preserves following BSS page");
    file=fixture();
    file[0x280+3*16+12]=std::byte{0x11}; // strong undefined OBJECT
    put32(file,0x35c,(3<<8)|21); // GLOB_DAT replaces its initial addend
    auto settings=options();
    auto object=atlan::elf::load(file,settings);
    check(get32(object.memory,0x1000220c)==0x60000001,"GLOB_DAT writes resolver address without addend");
    settings.resolve={};settings.unresolved=atlan::elf::UnresolvedImports::diagnostic_traps;
    auto trapped=atlan::elf::load(file,settings);
    const auto address=get32(trapped.memory,0x1000220c);
    check(trapped.import_at(address)->type==1 &&
          trapped.memory.translate(address,atlan::MemoryAccess::read)==nullptr,
          "unresolved data imports are inaccessible rather than fabricated objects");
}
void memory_and_call_boundaries() {
    atlan::Memory memory;
    memory.map(0x1000,4096,atlan::Permissions::read_write);
    const std::array<std::byte,4> data{std::byte{1},std::byte{2},std::byte{3},std::byte{4}};
    check(!memory.write(0x1ffe,data) && get32(memory,0x1ffc)==0,"host write preflights every page");
    bool rejected=false;
    try {memory.protect(0x1000,8192,atlan::Permissions::read);}catch(const std::exception&){rejected=true;}
    check(rejected && memory.write(0x1000,data),"failed protection change is atomic");
    memory.protect(0x1000,4096,atlan::Permissions::read);
    check(!memory.write(0x1000,data),"host write respects read-only protection");
    atlan::Runtime runtime;
    const std::array<std::uint32_t,5> args{};
    rejected=false;
    try {(void)runtime.call(0x1000,args,0x2000,0xffff0000,10);}catch(const std::exception&){rejected=true;}
    check(rejected,"scalar call rejects stack arguments rather than dropping them");
}
}
int main() {
    try {valid_image();traps_and_rejection();relro_initializers_and_imported_data();memory_and_call_boundaries();
        std::cout<<checks<<" ELF and call correctness checks passed\n";
    }catch(const std::exception& error){std::cerr<<"check "<<checks<<": "<<error.what()<<'\n';return 1;}
}
