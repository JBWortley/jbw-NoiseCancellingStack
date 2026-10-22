// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ncs::test {

inline void Require(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(
        std::string(file) + ":" + std::to_string(line) + ": requirement failed: " + expression);
  }
}

using TestFunction = void (*)();

inline int Run(const std::vector<std::pair<std::string, TestFunction>>& tests) {
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
  std::cout << (tests.size() - failures) << "/" << tests.size() << " tests passed\n";
  return failures == 0 ? 0 : 1;
}

}  // namespace ncs::test

#define NCS_REQUIRE(expression) ::ncs::test::Require((expression), #expression, __FILE__, __LINE__)
