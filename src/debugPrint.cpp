
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>

#include "RtMidi.h"

#include "LS9.hpp"

using namespace std::literals;

void mycallback(Parameter param, int32_t value) {
  std::cout << "Element: " << param.element << " Index: " << param.index << " Channel: " << param.channel << " Value: " << value << '\n';
}

int main(int, char**) {
  try {
    auto ls9 = LS9("Cathedral Port 1");

    ls9.addGlobalCallback(&mycallback);

    ls9.fade({51, 0, 10}, 0, 5s, 1s);

    std::cout << "\nReading MIDI input ... press <enter> to quit.\n";
    char input;
    std::cin.get(input);
  } catch (RtMidiError &error) {
    error.printMessage();
    exit(EXIT_FAILURE);
  }
}

