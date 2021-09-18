from distutils.core import setup, Extension
from distutils.util import get_platform

from os import environ

from pathlib import Path
this_directory = Path(__file__).parent
long_description = (this_directory / "README.md").read_text()

if get_platform().startswith('macosx'):
  extra_include_dirs = []
  define_macros = [('__MACOSX_CORE__', None), ('TARGET_OS_IPHONE', 0)]
  libraries = []
  extra_compile_args = ['--std=c++17']
  extra_link_args = ['-framework', 'CoreMIDI', '-framework', 'CoreAudio', '-framework', 'CoreFoundation']
elif get_platform().startswith('win'):
  extra_include_dirs = [environ['GSL_INCLUDE']]
  define_macros = [('__WINDOWS_MM__', None)]
  libraries = ['winmm']
  extra_compile_args = ['/std:c++20']
  extra_link_args = []

module1 = Extension \
  ( 'pyopenls9'
  , sources = ['src/python.cpp', 'src/RtMidi.cpp']
  , include_dirs = ['include'] + extra_include_dirs
  , define_macros = define_macros
  , libraries = libraries
  , extra_compile_args = extra_compile_args
  , extra_link_args = extra_link_args
  , depends = ['include/LS9.hpp', 'include/RtMidi.h']
  , language = 'c++'
  #, py_limited_api = True
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

