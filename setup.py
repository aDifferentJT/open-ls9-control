from distutils.core import setup, Extension
from distutils.util import get_platform

from os import environ

from pathlib import Path
this_directory = Path(__file__).parent
long_description = (this_directory / "README.md").read_text()

def define_macros():
  if get_platform().startswith('macosx'):
    return [('__MACOSX_CORE__', None), ('TARGET_OS_IPHONE', 0)]
  elif get_platform().startswith('win'):
    return [('__WINDOWS_MM__', None)]

def extra_compile_args():
  if get_platform().startswith('macosx'):
    return ['--std=c++17']
  elif get_platform().startswith('win'):
    return ['/std:c++17']

def extra_link_args():
  if get_platform().startswith('macosx'):
    return ['-framework', 'CoreMIDI', '-framework', 'CoreAudio', '-framework', 'CoreFoundation']
  elif get_platform().startswith('win'):
    return []

def extra_include_dirs():
  if get_platform().startswith('macosx'):
    return []
  elif get_platform().startswith('win'):
    return [environ['GSL_INCLUDE']]

module1 = Extension \
  ( 'pyopenls9'
  , language = 'c++'
  , define_macros = define_macros()
  , extra_compile_args = extra_compile_args()
  , extra_link_args = extra_link_args()
  , include_dirs = ['include'] + extra_include_dirs()
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

