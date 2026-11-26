# SPDX-License-Identifier: GPL-3.0-only
project(
  'noise-cancelling-stack-webrtc-apm-vst2',
  'cpp',
  version: '1.2.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=false', 'b_vscrt=mt'],
)

cpp = meson.get_compiler('cpp')
ncs_werror = cpp.get_supported_arguments('/WX')
ncs_webrtc_werror = ncs_werror + cpp.get_supported_arguments(
  '/wd4068',  # Clang diagnostic pragmas in public WebRTC headers.
  '/wd4100',  # Intentionally unused extension parameters in WebRTC headers.
  '/wd4146',  # Checked unsigned arithmetic templates in WebRTC headers.
  '/wd4191',  # Test-only IAT instrumentation converts imported function pointers.
)
ncs_fst_werror = ncs_werror + cpp.get_supported_arguments(
  '/wd4191',  # GetProcAddress is the Windows ABI boundary for VST exports.
  '/wd4200',  # FST models ABI flexible arrays with zero-sized members.
)
ncs_plugin_werror = ncs_webrtc_werror + cpp.get_supported_arguments('/wd4200')

frame_adapter_tests = executable(
  'frame-adapter-tests',
  ['frame_adapter.cpp', 'tests/frame_adapter_tests.cpp'],
  include_directories: include_directories('.'),
  cpp_args: ncs_werror,
)

test('frame-adapter', frame_adapter_tests)

# Echo cancellation APO (MIT-licensed sources under aec/; no FST or GPL dependencies).
aec_include = include_directories('aec')
aec_test_include = include_directories('aec/tests')

reference_ring_tests = executable(
  'reference-ring-tests',
  ['aec/tests/reference_ring_tests.cpp'],
  include_directories: [aec_include, aec_test_include],
  cpp_args: ncs_werror,
)

test('reference-ring', reference_ring_tests)

if get_option('with_webrtc')
  webrtc_subproject = subproject(
    'webrtc-audio-processing',
    default_options: [
      'default_library=static',
      'tests=disabled',
      'b_vscrt=mt',
      'werror=false',
    ],
  )
  webrtc_dependency = webrtc_subproject.get_variable('audio_processing_dep')
  webrtc_system_dependency = webrtc_dependency.as_system('system')

  processor_library = static_library(
    'ncs-webrtc-processor',
    ['webrtc_processor.cpp'],
    include_directories: include_directories('.'),
    dependencies: webrtc_system_dependency,
    cpp_args: ncs_webrtc_werror,
  )

  webrtc_processor_tests = executable(
    'webrtc-processor-tests',
    ['tests/webrtc_processor_tests.cpp'],
    include_directories: include_directories('.'),
    link_with: processor_library,
    dependencies: webrtc_system_dependency,
    cpp_args: ncs_webrtc_werror,
  )

  test('webrtc-processor', webrtc_processor_tests)

  webrtc_plugin = shared_module(
    'webrtc_apm_16k',
    ['frame_adapter.cpp', 'vst_plugin.cpp'],
    name_prefix: '',
    include_directories: [include_directories('.'), include_directories('fst')],
    link_with: processor_library,
    dependencies: webrtc_system_dependency,
    cpp_args: ncs_plugin_werror,
    vs_module_defs: 'webrtc_apm_16k.def',
  )

  vst_smoke_host = executable(
    'vst-smoke-host',
    ['tests/vst_smoke_host.cpp'],
    include_directories: [include_directories('.'), include_directories('fst')],
    cpp_args: ncs_fst_werror,
  )

  test(
    'vst-smoke',
    vst_smoke_host,
    args: [webrtc_plugin.full_path()],
  )

  echo_processor_library = static_library(
    'ncs-echo-processor',
    ['aec/echo_processor.cpp'],
    include_directories: aec_include,
    dependencies: webrtc_system_dependency,
    cpp_args: ncs_webrtc_werror,
  )

  echo_processor_tests = executable(
    'echo-processor-tests',
    ['aec/tests/echo_processor_tests.cpp'],
    include_directories: [aec_include, aec_test_include],
    link_with: echo_processor_library,
    dependencies: webrtc_system_dependency,
    cpp_args: ncs_webrtc_werror,
  )

  test('echo-processor', echo_processor_tests, timeout: 300)

  # Windows SDK APO support: base class, media types, registration, COM.
  apo_system_libraries = []
  foreach name : ['audiobaseprocessingobject', 'audiomediatypecrt', 'audioeng',
                  'legacy_stdio_definitions', 'ole32', 'advapi32']
    apo_system_libraries += cpp.find_library(name)
  endforeach

  aec_apo = shared_module(
    'ncs_aec_apo',
    ['aec/aec_apo.cpp'],
    name_prefix: '',
    include_directories: aec_include,
    link_with: echo_processor_library,
    dependencies: [webrtc_system_dependency] + apo_system_libraries,
    cpp_args: ncs_webrtc_werror,
    vs_module_defs: 'aec/ncs_aec_apo.def',
  )

  aec_apo_smoke_host = executable(
    'aec-apo-smoke-host',
    ['aec/tests/aec_apo_smoke_host.cpp'],
    include_directories: aec_test_include,
    dependencies: [cpp.find_library('audiomediatypecrt'), cpp.find_library('ole32')],
    cpp_args: ncs_werror,
  )

  test(
    'aec-apo-smoke',
    aec_apo_smoke_host,
    args: [aec_apo.full_path()],
    timeout: 300,
  )
endif
