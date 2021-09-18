#ifndef LS9_hpp
#define LS9_hpp

#include <array>
#include <cstdint>
#include <cstring>
#include <future>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RtMidi.h"

auto lerp_and_clamp(int32_t a, int32_t b, float t) {
  return t < 0 ? a : t < 0.5 ? a + t * (b - a) : t < 1 ? b + (1 - t) * (a - b) : b;
}

// C++23
template <typename Enum>
auto to_underlying(Enum x) {
  return static_cast<std::underlying_type_t<Enum>>(x);
}

auto to_hex(int x) {
  auto sstream = std::stringstream{};
  sstream << std::hex << x;
  return std::move(sstream).str();
}

template <typename T>
class lazy {
  private:
    std::optional<T> datum;

  public:
    auto operator->() -> auto& {
      if (!datum) { datum.emplace(); }
      return datum;
    }

    auto operator->() const -> auto& {
      if (!datum) { datum.emplace(); }
      return datum;
    }

    auto pop() -> std::optional<T> {
      return std::exchange(datum, {});
    }
};

struct Parameter {
  int element;
  int index;
  int channel;

  friend bool operator==(Parameter const & lhs, Parameter const & rhs) {
    return lhs.element == rhs.element && lhs.index == rhs.index && lhs.channel == rhs.channel;
  }
};

namespace std {
  template <>
  struct hash<Parameter> {
    auto operator()(Parameter param) const noexcept {
      auto hashInt = std::hash<int>{};
      return hashInt(param.element) ^ hashInt(param.index) ^ hashInt(param.channel);
    }
  };
}

template <typename T>
class shared_promise {
  private:
    std::promise<T> promise;
    std::shared_future<T> future;

  public:
    shared_promise() : future{promise.get_future()} {}

    auto get_future() { return future; }

    template <typename Arg>
    void set_value(Arg&& x) { promise.set_value(std::forward<Arg>(x)); }
};

class LS9 {
  public:
    struct timeout_expired {};

  private:
    enum class Status : uint8_t { sysEx = 0xF0 };
    enum class ManufacturerId : uint8_t { yamaha = 0x43 };
    enum class GroupId : uint8_t { digitalMixer = 0x3E };
    enum class ModelId : uint8_t { ls9 = 0x12 };

    enum class SubStatusHigh : uint8_t {
      bulkDump = 0x0, paramChange = 0x1, bulkRequest = 0x2, paramRequest = 0x3
    };

    enum class DataCategory : uint8_t {
      functionCall = 0x00, currentScene_setup_backup_userSetupData = 0x01, levelMeter = 0x21
    };

    struct MessageHeader {
      Status status = Status::sysEx;
      ManufacturerId manufacturerId = ManufacturerId::yamaha;
      uint8_t subStatusLow : 4;
      SubStatusHigh subStatusHigh : 4;
      GroupId groupId = GroupId::digitalMixer;
      ModelId modelId = ModelId::ls9;
      DataCategory dataCategory;

      MessageHeader() = default;

      MessageHeader(SubStatusHigh subStatusHigh, DataCategory dataCategory)
        : subStatusLow{0}
        , subStatusHigh{subStatusHigh}
        , dataCategory{dataCategory}
        {}
    };

    struct ParameterMessage {
      uint8_t elementHigh;
      uint8_t elementLow;
      uint8_t indexHigh;
      uint8_t indexLow;
      uint8_t channelHigh;
      uint8_t channelLow;

      ParameterMessage() = default;

      ParameterMessage(Parameter param)
        : elementHigh{static_cast<uint8_t>(param.element >> 7)}
        , elementLow {static_cast<uint8_t>(param.element & 0x7F)}
        , indexHigh  {static_cast<uint8_t>(param.index   >> 7)}
        , indexLow   {static_cast<uint8_t>(param.index   & 0x7F)}
        , channelHigh{static_cast<uint8_t>(param.channel >> 7)}
        , channelLow {static_cast<uint8_t>(param.channel & 0x7F)}
        {}
    };

    struct ParameterData : std::array<uint8_t, 5> {
      ParameterData() = default;

      ParameterData(int32_t value)
        : std::array<uint8_t, 5>
          { static_cast<uint8_t>((value >> 28) & 0x7F)
          , static_cast<uint8_t>((value >> 21) & 0x7F)
          , static_cast<uint8_t>((value >> 14) & 0x7F)
          , static_cast<uint8_t>((value >>  7) & 0x7F)
          , static_cast<uint8_t>( value        & 0x7F)
          }
        {}
    };

    struct ParamChangeMessage {
      MessageHeader header = {SubStatusHigh::paramChange, DataCategory::currentScene_setup_backup_userSetupData};
      ParameterMessage param;
      ParameterData data;
      uint8_t terminator = 0xF7;

      ParamChangeMessage() = default;

      ParamChangeMessage(Parameter param, int32_t value)
        : param{param}
        , data{value}
        {}
    };

    struct ParamRequestMessage {
      MessageHeader header = {SubStatusHigh::paramRequest, DataCategory::currentScene_setup_backup_userSetupData};
      ParameterMessage param;
      uint8_t terminator = 0xF7;

      ParamRequestMessage(Parameter param) : param{param} {}
    };

    RtMidiIn midiIn;
    RtMidiOut midiOut;

    std::vector<std::function<void(Parameter, int32_t)>> global_callbacks;
    std::unordered_map<Parameter, std::vector<std::function<void(Parameter, int32_t)>>> param_callbacks;
    lazy<shared_promise<Parameter>> next_param_touched;
    std::unordered_map<Parameter, shared_promise<int32_t>> next_value;

    struct ParseError : std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    void callback(double, std::vector<uint8_t>* message) {
            try{
      auto header = MessageHeader{};
      std::memcpy(&header, message->data(), sizeof(header));
      if (header.status != Status::sysEx) { throw ParseError{"Unsupported Status: " + to_hex(to_underlying(header.status))}; }
      if (header.manufacturerId != ManufacturerId::yamaha) { throw ParseError{"Unsupported manufacturer"}; }
      if (header.groupId != GroupId::digitalMixer) { throw ParseError{"Unsupported group"}; }
      if (header.modelId != ModelId::ls9) { throw ParseError{"Unsupported model"}; }
      switch (header.subStatusHigh) {
        case SubStatusHigh::paramChange:
          switch (header.dataCategory) {
            case DataCategory::functionCall:
              throw ParseError{"Function calls unsupported"};

            case DataCategory::currentScene_setup_backup_userSetupData:
              {
                auto rawMessage = ParamChangeMessage{};
                std::memcpy(&rawMessage, message->data(), sizeof(rawMessage));
                if (rawMessage.terminator != 0xF7) { throw ParseError{"No terminator, instead saw: " + to_hex(rawMessage.terminator)}; }
                auto param = Parameter
                  { rawMessage.param.elementHigh << 7 | rawMessage.param.elementLow
                  , rawMessage.param.indexHigh   << 7 | rawMessage.param.indexLow
                  , rawMessage.param.channelHigh << 7 | rawMessage.param.channelLow
                  };
                auto value = static_cast<int32_t>
                  ( rawMessage.data[0] << 28
                  | rawMessage.data[1] << 21
                  | rawMessage.data[2] << 14
                  | rawMessage.data[3] << 7
                  | rawMessage.data[4]
                  );

      std::cerr << "Received: " << param.channel << '\n';
                for (auto& callback : global_callbacks) {
                  callback(param, value);
                }
                try {
                  for (auto& callback : param_callbacks.at(param)) {
                    callback(param, value);
                  }
                } catch(std::out_of_range const &) {}
                if (auto promise = next_param_touched.pop()) {
                  promise->set_value(param);
                }
                try {
                  if (auto node = next_value.extract(param)) {
                    node.mapped().set_value(value);
                  }
                } catch(std::out_of_range const &) {}
              }

            case DataCategory::levelMeter:
              throw ParseError{"Level meter unsupported"};
          }
          throw ParseError{"Unsupported data category: " + to_hex(to_underlying(header.dataCategory))};
      }
      throw ParseError{"Unsupported substatus"};
            } catch (ParseError const & e) {
                    //std::cerr << e.what() << '\n';
            }
    }

    static void _callback(double timeStamp, std::vector<uint8_t>* message, void* userData) {
      static_cast<LS9*>(userData)->callback(timeStamp, message);
    }

    template <typename T>
    void send(T const & x) {
      midiOut.sendMessage(reinterpret_cast<uint8_t const *>(&x), sizeof(x));
    }

  public:
    LS9(std::string_view portName) {
      for (auto i = 0u; i < midiIn.getPortCount(); i += 1) {
        if (midiIn.getPortName(i) == portName) {
          midiIn.openPort(i);
        }
      }
      for (auto i = 0u; i < midiOut.getPortCount(); i += 1) {
        if (midiOut.getPortName(i) == portName) {
          midiOut.openPort(i);
        }
      }
    
      midiIn.setCallback(_callback, this);
    
      // Don't ignore sysex messages.
      midiIn.ignoreTypes(false);
    }

    void addGlobalCallback(std::function<void(Parameter, int32_t)> callback) {
      global_callbacks.push_back(callback);
    }

    void addParamCallback(Parameter param, std::function<void(Parameter, int32_t)> callback) {
      param_callbacks[param].push_back(callback);
    }

    auto get(Parameter const param, std::chrono::milliseconds const timeout) {
      auto future = next_value[param].get_future();
      std::cerr << "Requesting: " << param.channel << '\n';
      send(ParamRequestMessage{param});
      switch (future.wait_for(timeout)) {
        case std::future_status::deferred:
        case std::future_status::ready:
          return future.get();
        case std::future_status::timeout:
          throw timeout_expired{};
      }
    }

    void set(Parameter const param, int32_t const value) {
      send(ParamChangeMessage{param, value});
    }

    void fade(Parameter const param, int32_t const value, std::chrono::milliseconds const duration, std::chrono::milliseconds const timeout) {
      using namespace std::literals;
      auto const startValue = get(param, timeout);
      std::thread
        { [this, param, value, duration, startValue] {
            using clock = std::chrono::high_resolution_clock;
            auto const startTime = clock::now();
            auto cur = std::chrono::duration<float, std::chrono::milliseconds::period>{};
            do {
              std::this_thread::sleep_for(10ms);
              cur = clock::now() - startTime;
              set(param, lerp_and_clamp(startValue, value, cur / duration));
            } while (cur < duration);
          }
        }.detach();
    }

    auto nextParamTouched() {
      return next_param_touched->get_future().get();
    }

    auto getChannelName(int const ch, std::chrono::milliseconds const timeout) -> std::string {
      auto param1 = Parameter{148, 0, ch};
      auto param2 = Parameter{148, 1, ch};
      auto future1 = next_value[param1].get_future();
      auto future2 = next_value[param2].get_future();
      send(ParamRequestMessage{param1});
      send(ParamRequestMessage{param2});
      auto deadline = std::chrono::system_clock::now() + timeout;
      auto value1 = [&] {
        switch (future1.wait_until(deadline)) {
          case std::future_status::deferred:
          case std::future_status::ready:
            return future1.get();
          case std::future_status::timeout:
            throw timeout_expired{};
        }
      }();
      auto value2 = [&] {
        switch (future2.wait_until(deadline)) {
          case std::future_status::deferred:
          case std::future_status::ready:
            return future2.get();
          case std::future_status::timeout:
            throw timeout_expired{};
        }
      }();
      char str[] =
        { static_cast<char>(value1 >> 24)
        , static_cast<char>(value1 >> 16)
        , static_cast<char>(value1 >>  8)
        , static_cast<char>(value1)
        , static_cast<char>(value2 >> 24)
        , static_cast<char>(value2 >> 16)
        };
      return str;
    }
};

#endif
