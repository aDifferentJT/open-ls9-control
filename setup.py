from distutils.core import setup, Extension

module1 = Extension(
    'pyls9',
    language = 'c++',
    define_macros = [('__MACOSX_CORE__', None), ('TARGET_OS_IPHONE', 0)],
    extra_compile_args = ['--std=c++17'],
    extra_link_args = ['-framework', 'CoreMIDI', '-framework', 'CoreAudio', '-framework', 'CoreFoundation'],
    include_dirs = ['include'],
    sources = ['src/RtMidi.cpp', 'src/python.cpp'])

setup (name = 'pyls9',
       version = '1.0',
       description = 'A library to control the Yamaha LS9',
       author = 'Jonathan Tanner',
       url = 'http://github.com/nixCodeX/pyls9',
       long_description = '''
A library to control the Yamaha LS9
''',
       ext_modules = [module1])

