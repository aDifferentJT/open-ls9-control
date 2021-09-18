from distutils.core import setup, Extension

from pathlib import Path
this_directory = Path(__file__).parent
long_description = (this_directory / "README.md").read_text()

module1 = Extension \
  ( 'pyopenls9'
  , language = 'c++'
  , define_macros = [('__MACOSX_CORE__', None), ('TARGET_OS_IPHONE', 0)]
  , extra_compile_args = ['--std=c++17']
  , extra_link_args = ['-framework', 'CoreMIDI', '-framework', 'CoreAudio', '-framework', 'CoreFoundation']
  , include_dirs = ['include']
  , depends = ['include/LS9.hpp', 'include/RtMidi.h']
  , sources = ['src/python.cpp', 'src/RtMidi.cpp']
  , py_limited_api = True
  )

setup \
  ( name = 'pyopenls9'
  , version = '1.1.0'
  , description = 'A library to control the Yamaha LS9'
  , author = 'Jonathan Tanner'
  , url = 'http://github.com/nixCodeX/open-ls9-control'
  , long_description = long_description
  , long_description_content_type='text/markdown'
  , ext_modules = [module1]
  )

