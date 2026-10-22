# SPDX-License-Identifier: GPL-3.0-only
project(
  'noise-cancelling-stack-webrtc-apm-vst2',
  'cpp',
  version: '1.1.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=true', 'b_vscrt=mt'],
)

frame_adapter_tests = executable(
  'frame-adapter-tests',
  ['frame_adapter.cpp', 'tests/frame_adapter_tests.cpp'],
  include_directories: include_directories('.'),
)

test('frame-adapter', frame_adapter_tests)
