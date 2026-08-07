#pragma once
// Sparse backing store + DPI glue for the LiteDRAM DPI sim core
// (USE_DRAM_CTRL builds). The generated litedram_core's 16 dram_dpi_mem bank
// instances call dram_dpi_read/dram_dpi_write below; the bytes live in the
// same dword-keyed sparse map scheme as the C++ AXI memory mock, so
// load_memory_from_binary, the post-sim read_memory validation, and the
// 0xBAADDEADBEEFDEAD uninitialized-read magic keep today's semantics exactly.
//
// Include this header from exactly one translation unit per simulation binary
// (it defines the extern "C" DPI symbols).
//
// Geometry constants must match litedram_dram_ctrl.yml + the fork script's
// MT40A4G8 module (see dram_ctrl/README.md); the smoke test cross-checks the
// resulting address inversion against front-door AXI traffic.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "svdpi.h"

class DramDpiStore {
 public:
  // MT40A4G8 x8 dies, 64-bit bus, sim rate 1:4 -> 512-bit words.
  static constexpr unsigned kBankBits = 4;   // 16 banks
  static constexpr unsigned kRowBits = 18;   // 2^18 rows
  static constexpr unsigned kColBits = 10;   // 1024 columns
  // One bank-memory word = burst_length(2) * nphases(4) = 8 columns = 64B.
  static constexpr unsigned kColWordBits = kColBits - 3;  // words per row = 128
  static constexpr unsigned kBytesPerWord = 64;

  static DramDpiStore& instance() {
    static DramDpiStore store;
    return store;
  }

  // Linear byte address of a (bank, bank-memory word address) pair under the
  // controller's ROW_BANK_COL mapping. wadr = row << kColWordBits | col_word
  // (see BankModel: (row*ncols | col) >> log2(burst*nphases)).
  static uint64_t linear_byte_address(unsigned bank, uint64_t word_address) {
    const uint64_t row = word_address >> kColWordBits;
    const uint64_t col_word = word_address & ((1ULL << kColWordBits) - 1);
    const uint64_t linear_word =
        (((row << kBankBits) | bank) << kColWordBits) | col_word;
    return linear_word * kBytesPerWord;
  }

  // 512-bit beat write with per-byte strobes; svBitVecVal chunks are 32-bit,
  // least-significant first, byte i of the beat = bits [8i+7:8i].
  //
  // The generated core has 16 dram_dpi_mem instances (one per bank); under a
  // Verilator --threads build these DPI calls can arrive from different
  // worker threads concurrently, so every access to `memory` below takes
  // store_mutex_.
  void write_beat(unsigned bank, uint64_t word_address,
                  const svBitVecVal* data, uint64_t strobes) {
    const uint64_t base = linear_byte_address(bank, word_address);
    std::lock_guard<std::mutex> lock(store_mutex_);
    for (unsigned byte = 0; byte < kBytesPerWord; byte++) {
      if (!((strobes >> byte) & 1ULL)) {
        continue;
      }
      const uint8_t value = static_cast<uint8_t>(
          (data[byte >> 2] >> ((byte & 3) * 8)) & 0xffu);
      write_byte_locked(base + byte, value);
    }
  }

  // 512-bit beat read; unpopulated dwords return the magic pattern.
  void read_beat(unsigned bank, uint64_t word_address, svBitVecVal* data) {
    const uint64_t base = linear_byte_address(bank, word_address);
    std::lock_guard<std::mutex> lock(store_mutex_);
    for (unsigned dword = 0; dword < kBytesPerWord / 8; dword++) {
      const uint64_t value = read_word_locked(base + dword * 8);
      data[dword * 2] = static_cast<svBitVecVal>(value);
      data[dword * 2 + 1] = static_cast<svBitVecVal>(value >> 32);
    }
  }

  // --- Same semantics as the C++ AXI memory mock ----------------------------

  uint64_t read_word(uint64_t byte_address) {
    std::lock_guard<std::mutex> lock(store_mutex_);
    return read_word_locked(byte_address);
  }

  void write_byte(uint64_t byte_address, uint8_t value) {
    std::lock_guard<std::mutex> lock(store_mutex_);
    write_byte_locked(byte_address, value);
  }

  // Read a byte-addressed range; returns false when any covering double-word
  // has not been populated (drives the post-sim binary-dump validation).
  bool read_memory(uint64_t byte_address, void* destination,
                   size_t num_bytes) {
    auto* output = static_cast<uint8_t*>(destination);
    std::lock_guard<std::mutex> lock(store_mutex_);
    for (size_t index = 0; index < num_bytes; ++index) {
      const uint64_t current_address = byte_address + index;
      const auto found = memory.find(current_address >> 3);
      if (found == memory.end()) {
        return false;
      }
      output[index] = static_cast<uint8_t>(
          (found->second >> ((current_address & 0x7) * 8)) & 0xffULL);
    }
    return true;
  }

  void load_memory_from_binary(const std::string& filename) {
    if (filename.empty()) {
      std::cout << "[DRAM_CTRL] No data binary supplied" << std::endl;
      return;
    }

    std::ifstream file(filename, std::ios::binary);
    if (!file) {
      std::cerr << "[DRAM_CTRL] ERROR: Cannot open " << filename << std::endl;
      return;
    }

    // Two on-disk layouts are accepted, distinguished by a magic prefix --
    // keep this in lockstep with AxiMemoryDrv::load_memory_from_binary, whose
    // semantics this mirrors:
    //
    //   "UDRD" | u32 version | u64 any_physical | u64 records
    //          then records of { u64 addr, u64 size, u64 is_physical, bytes }
    //
    //   legacy (no magic): u64 records, then { u64 addr, u64 size, bytes }
    //
    // The {addr, size, payload} triples are identical between the two; the
    // newer format only adds the header and the per-record is_physical flag.
    // is_physical is logged but not otherwise used: these addresses are already
    // what the AXI side sees, translated or not.
    char magic[4] = {};
    file.read(magic, sizeof(magic));
    if (!file) {
      std::cerr << "[DRAM_CTRL] ERROR: Invalid binary header in " << filename
                << std::endl;
      return;
    }
    const bool tagged_format = (magic[0] == 'U' && magic[1] == 'D' &&
                                magic[2] == 'R' && magic[3] == 'D');

    uint64_t allocation_count = 0;
    uint64_t any_physical = 0;
    if (tagged_format) {
      uint32_t version = 0;
      file.read(reinterpret_cast<char*>(&version), sizeof(version));
      file.read(reinterpret_cast<char*>(&any_physical), sizeof(any_physical));
      file.read(reinterpret_cast<char*>(&allocation_count),
                sizeof(allocation_count));
      if (!file) {
        std::cerr << "[DRAM_CTRL] ERROR: Truncated UDRD header in " << filename
                  << std::endl;
        return;
      }
      if (version != 1) {
        std::cerr << "[DRAM_CTRL] ERROR: Unsupported UDRD version " << version
                  << " in " << filename << " (expected 1)" << std::endl;
        return;
      }
    } else {
      // The legacy layout starts with the record count, so the four bytes we
      // already consumed are its low half.
      file.seekg(0, std::ios::beg);
      file.read(reinterpret_cast<char*>(&allocation_count),
                sizeof(allocation_count));
      if (!file) {
        std::cerr << "[DRAM_CTRL] ERROR: Invalid binary header in " << filename
                  << std::endl;
        return;
      }
    }

    uint64_t total_bytes = 0;
    uint64_t physical_records = 0;
    for (uint64_t allocation = 0; allocation < allocation_count;
         allocation++) {
      uint64_t start_address = 0;
      uint64_t byte_count = 0;
      file.read(reinterpret_cast<char*>(&start_address),
                sizeof(start_address));
      file.read(reinterpret_cast<char*>(&byte_count), sizeof(byte_count));
      if (tagged_format) {
        uint64_t is_physical = 0;
        file.read(reinterpret_cast<char*>(&is_physical), sizeof(is_physical));
        if (is_physical) physical_records++;
      }
      if (!file) {
        std::cerr << "[DRAM_CTRL] ERROR: Truncated allocation header"
                  << std::endl;
        return;
      }

      std::vector<uint8_t> bytes(byte_count);
      file.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(byte_count));
      if (!file) {
        std::cerr << "[DRAM_CTRL] ERROR: Truncated allocation data"
                  << std::endl;
        return;
      }

      {
        std::lock_guard<std::mutex> lock(store_mutex_);
        for (uint64_t offset = 0; offset < byte_count; offset++) {
          write_byte_locked(start_address + offset, bytes[offset]);
        }
      }
      total_bytes += byte_count;
    }

    std::cout << "[DRAM_CTRL] Loaded " << allocation_count << " allocations, "
              << total_bytes << " bytes from " << filename;
    if (tagged_format) {
      std::cout << " (UDRD, any_physical=" << any_physical << ", "
                << physical_records << "/" << allocation_count
                << " record(s) flagged physical)";
    } else {
      std::cout << " (legacy format)";
    }
    std::cout << std::endl;
  }

  size_t populated_dwords() {
    std::lock_guard<std::mutex> lock(store_mutex_);
    return memory.size();
  }

 private:
  DramDpiStore() = default;

  uint64_t read_word_locked(uint64_t byte_address) const {
    const auto found = memory.find(byte_address >> 3);
    return found == memory.end() ? 0xBAADDEADBEEFDEADULL : found->second;
  }

  void write_byte_locked(uint64_t byte_address, uint8_t value) {
    const uint64_t word_address = byte_address >> 3;
    const unsigned byte_lane = static_cast<unsigned>(byte_address & 7);
    uint64_t& word = memory[word_address];
    word &= ~(0xffULL << (byte_lane * 8));
    word |= static_cast<uint64_t>(value) << (byte_lane * 8);
  }

  std::mutex store_mutex_;
  std::unordered_map<uint64_t, uint64_t> memory;
};

// DPI entry points called by dram_dpi_mem.sv. Signatures must match the
// Verilator-generated Vdut__Dpi.h (int/longint/bit [511:0]).
extern "C" void dram_dpi_write(int bank, long long adr, const svBitVecVal* data,
                               long long strb) {
  DramDpiStore::instance().write_beat(static_cast<unsigned>(bank),
                                      static_cast<uint64_t>(adr), data,
                                      static_cast<uint64_t>(strb));
}

extern "C" void dram_dpi_read(int bank, long long adr, svBit /*wr_tgl*/,
                              svBitVecVal* data) {
  DramDpiStore::instance().read_beat(static_cast<unsigned>(bank),
                                     static_cast<uint64_t>(adr), data);
}
