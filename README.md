# open-ls9-control

Open LS9 Control is a C++ library for controlling the Yamaha LS9 mixer.
There are also Python bindings included known as PyOpenLS9.

It depends on the [Yamaha Network-MIDI driver](https://uk.yamaha.com/en/support/updates/nwd_mac.html) and the port name is "<Device Name> Port 1" where <Device Name> is the name from System Preferences.

### Parameters

A parameter is defined by an element, index and channel, defined in C++ as an aggregate thus:

```
struct Parameter {
  int element;
  int index;
  int channel;
};
```

And in Python as a dataclass thus:

```
@dataclass
class Parameter:
  "Some parameter of the desk"
  element : int
  index : int
  channel : int
```

### The LS9 class

In C++ the LS9 class is constructed with the port name as a string and exposes the following methods:

- void addGlobalCallback(std::function<void(Parameter, int32_t)> callback)
- void addParamCallback(Parameter param, std::function<void(Parameter, int32_t)> callback)
- auto get(Parameter param, std::chrono::milliseconds timeout) -> int32_t
- void set(Parameter param, int32_t value)
- void fade(Parameter param, int32_t value, std::chrono::milliseconds duration, std::chrono::milliseconds timeout)
- auto nextParamTouched() -> Parameter
- auto getChannelName(int ch, std::chrono::milliseconds timeout) -> std::string

The Python class is much the same, any callable can be used where a std::function is expected and an integer number of milliseconds is used where a std::chrono::milliseconds is expected.

