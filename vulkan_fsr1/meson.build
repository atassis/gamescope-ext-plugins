project('gamescope-vulkan-fsr1', 'cpp', default_options: ['cpp_std=c++17', 'buildtype=debugoptimized'])

plugin_name = 'vulkan_fsr1'
cpp = meson.get_compiler('cpp')
vulkan_dep = dependency('vulkan')

# The ABI header is the only piece shared with the host, which installs it with a pkg-config file.
abi_dep = dependency('gamescope-external-upscaler')
abi_version = cpp.get_define('GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION',
  prefix: '#include "external_upscaler.h"', dependencies: abi_dep)

glsl_compiler = find_program('glslang', 'glslangValidator', native: true)
glsl_generator = generator(
  glsl_compiler,
  output    : ['@BASENAME@.h'],
  arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@', '-I' + meson.current_source_dir() / 'shaders', '--quiet'],
)

spirv_shaders = glsl_generator.process([
  'shaders/easu.comp',
  'shaders/rcas.comp',
])

install_dir = get_option('datadir') / 'gamescope-upscalers' / plugin_name

lib = shared_library('gamescope_vulkan_fsr1',
  ['vulkan_fsr1.cpp', spirv_shaders],
  include_directories: include_directories('shaders'),
  dependencies: [vulkan_dep, abi_dep],
  install: true,
  install_dir: install_dir,
)

configure_file(
  input: 'manifest.json.in',
  output: 'manifest.json',
  configuration: {
    'name': plugin_name,
    'abi_version': abi_version,
    'library': 'libgamescope_vulkan_fsr1.so',
    'default_config': get_option('default_config'),
  },
  install: true,
  install_dir: install_dir,
)
