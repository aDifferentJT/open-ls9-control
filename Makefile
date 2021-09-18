
debugPrint: src/debugPrint.cpp include/LS9.hpp
	clang++ -Wall -Wextra -Wno-undef-prefix --std=c++17 -D__MACOSX_CORE__ -framework CoreMIDI -framework CoreAudio -framework CoreFoundation -Iinclude src/RtMidi.cpp src/debugPrint.cpp -o debugPrint

PyLS9: src/python.cpp include/LS9.hpp
	clang++ -Wall -Wextra -Wno-undef-prefix --std=c++17 -D__MACOSX_CORE__ -framework CoreMIDI -framework CoreAudio -framework CoreFoundation -Iinclude src/RtMidi.cpp src/python.cpp $$(python3-config --cflags) -shared

