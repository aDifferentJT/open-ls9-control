
debugPrint: src/debugPrint.cpp include/LS9.hpp
	clang++ -Wall -Wextra -Wno-undef-prefix --std=c++17 -D__MACOSX_CORE__ -framework CoreMIDI -framework CoreAudio -framework CoreFoundation -Iinclude src/RtMidi.cpp src/debugPrint.cpp -o debugPrint

