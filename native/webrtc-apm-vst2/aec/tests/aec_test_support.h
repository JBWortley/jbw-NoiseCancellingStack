// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ncs::aec_test {

inline void Require(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": requirement failed: " + expression);
  }
}

using TestFunction = void (*)();

inline int Run(const std::vector<std::pair<const char*, TestFunction>>& tests) {
  int failures = 0;
  for (const auto& [name, function] : tests) {
    try {
      function();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << (tests.size() - static_cast<std::size_t>(failures)) << "/" << tests.size() << " tests passed\n";
  return failures == 0 ? 0 : 1;
}

// Deterministic uniform noise in [-1, 1).
class Noise final {
 public:
  explicit Noise(std::uint32_t seed) : state_(seed) {}
  float Next() noexcept {
    state_ = state_ * 1664525U + 1013904223U;
    return static_cast<float>(static_cast<std::int32_t>(state_ >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
  }

 private:
  std::uint32_t state_;
};

inline double Db(double ratio) { return ratio <= 1e-30 ? -300.0 : 10.0 * std::log10(ratio); }

// Replaces one kernel32 import in this executable's import table for the lifetime of the object.
// The statically linked WebRTC and C runtime code resolve OS calls through this table, so a hook
// observes every call they make.
class ImportHook final {
 public:
  ImportHook() = default;
  ImportHook(const ImportHook&) = delete;
  ImportHook& operator=(const ImportHook&) = delete;
  ~ImportHook() { Restore(); }

  bool Install(const char* function, void* replacement, void** original) noexcept {
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
      return false;
    }
    for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         descriptor->Name != 0; ++descriptor) {
      if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "KERNEL32.dll") != 0) {
        continue;
      }
      auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
      auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
      for (; names->u1.AddressOfData != 0; ++names, ++slots) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
          continue;
        }
        const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
        if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) {
          continue;
        }
        DWORD protection = 0;
        if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &protection)) {
          return false;
        }
        slot_ = &slots->u1.Function;
        saved_ = *slot_;
        *original = reinterpret_cast<void*>(saved_);
        *slot_ = reinterpret_cast<ULONG_PTR>(replacement);
        VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protection, &protection);
        return true;
      }
    }
    return false;
  }

  void Restore() noexcept {
    if (slot_ == nullptr) {
      return;
    }
    DWORD protection = 0;
    VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection);
    *slot_ = saved_;
    VirtualProtect(slot_, sizeof(*slot_), protection, &protection);
    slot_ = nullptr;
  }

 private:
  ULONG_PTR* slot_ = nullptr;
  ULONG_PTR saved_ = 0;
};

}  // namespace ncs::aec_test

#define AEC_REQUIRE(expression) ::ncs::aec_test::Require((expression), #expression, __FILE__, __LINE__)
