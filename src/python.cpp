
#define PY_SSIZE_T_CLEAN
#define Py_LIMITED_API 0x03030000
#include <Python.h>

#include "LS9.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <gsl/gsl>

// TODO: do this better
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wc99-designator"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma clang diagnostic ignored "-Wwritable-strings"

using namespace std::literals;

namespace {
  class scoped_gil {
    private:
      PyGILState_STATE gstate = PyGILState_Ensure();

    public:
      scoped_gil() = default;
      scoped_gil(scoped_gil const &) = delete;
      scoped_gil& operator=(scoped_gil const &) = delete;
      ~scoped_gil() { PyGILState_Release(gstate); }
  };

  class scoped_release_gil {
    private:
      PyThreadState* save = PyEval_SaveThread();

    public:
      scoped_release_gil() = default;
      scoped_release_gil(scoped_release_gil const &) = delete;
      scoped_release_gil& operator=(scoped_release_gil const &) = delete;
      ~scoped_release_gil() { PyEval_RestoreThread(save); }
  };

  class owned_PyObject;

  // TODO: Better safety of nullability here
  class view_PyObject {
    private:
    public:
      PyObject* object;

    public:
      view_PyObject() = default;

      view_PyObject(std::nullptr_t) : object{nullptr} {}

      view_PyObject(PyObject* object) : object{object} {}
      view_PyObject(PyTypeObject* object) : object{reinterpret_cast<PyObject*>(object)} {}

      template <typename T = PyObject>
      auto get() const { return reinterpret_cast<T*>(object); }

      operator PyObject* () const { return object; }

      template <typename ...Args>
      auto operator()(Args const & ...args) const -> owned_PyObject;

      auto operator[](char const * attr_name) const -> owned_PyObject;
  };

  static_assert(std::is_trivial_v<view_PyObject>, "view_PyObject is not trivial");

  class owned_PyObject {
    private:
    public:
      PyObject* object;

      owned_PyObject(PyObject* object) : object{object} {}
  
    public:
      owned_PyObject(std::nullptr_t) : object{nullptr} {}
  
      owned_PyObject(owned_PyObject const & that) : object{that.object} {
        Py_XINCREF(object);
      }
  
      owned_PyObject(owned_PyObject && that) : object{std::exchange(that.object, nullptr)} {}

      owned_PyObject(view_PyObject that) : object{that} {
        Py_XINCREF(object);
      }
  
      friend void swap(owned_PyObject & lhs, owned_PyObject & rhs) {
        std::swap(lhs.object, rhs.object);
      }
  
      owned_PyObject & operator=(owned_PyObject that) {
        swap(*this, that);
        return *this;
      }
  
      ~owned_PyObject() { Py_XDECREF(object); }
  
      template <typename T>
      static owned_PyObject take_ref(T* object) {
        Py_XINCREF(object);
        return owned_PyObject{reinterpret_cast<PyObject*>(object)};
      }
  
      template <typename T>
      static owned_PyObject steal_ref(T* object) {
        return owned_PyObject{reinterpret_cast<PyObject*>(object)};
      }
  
      auto release() { return std::exchange(object, nullptr); }

      auto view() const -> view_PyObject { return {object}; }

      template <typename T = PyObject>
      auto get() const { return reinterpret_cast<T*>(object); }

      operator view_PyObject() { return view(); }

      operator PyObject* () const & { return get(); }
      operator PyObject* () const && = delete;

      template <typename ...Args>
      auto operator()(Args const & ...args) const -> owned_PyObject {
        return view()(args...);
      }
      auto operator[](char const * attr_name) const -> owned_PyObject {
        return view()[attr_name];
      }
  };

  auto view_PyObject::operator[](char const * attr_name) const -> owned_PyObject {
    return owned_PyObject::steal_ref(PyObject_GetAttrString(object, attr_name));
  }

  template <typename T>
  extern PyType_Spec py_iterator_type_spec;
  
  template <typename T>
  struct py_iterator : public PyObject {
    typename T::const_iterator cur;
    typename T::const_iterator end;

    bool immutable;

    static void dealloc(py_iterator* self) {
      std::destroy_at(&self->cur);
      std::destroy_at(&self->end);
    }
  
    static auto next(py_iterator* self) -> PyObject*;
  };
  
  template <typename> struct iterator_name;

  template <> struct iterator_name<std::vector<std::string>> {
    static constexpr auto name = "vector<string>";
  };
  
  template <typename T>
  static PyType_Slot py_iterator_slots[] =
    { {Py_tp_dealloc, (void*)py_iterator<T>::dealloc}
    , {Py_tp_iter, (void*)PyObject_SelfIter}
    , {Py_tp_iternext, (void*)py_iterator<T>::next}
    , {0, nullptr}
    };
  
  template <typename T>
  static auto py_iterator_spec = PyType_Spec
    { .name = iterator_name<std::remove_cv_t<T>>::name
    , .basicsize = sizeof(py_iterator<T>)
    , .itemsize = 0
    , .flags = Py_TPFLAGS_DEFAULT
    , .slots = py_iterator_slots<T>
    };

  template <typename T>
  static auto py_iterator_factory(T & t, bool immutable) -> owned_PyObject {
    auto py_iterator_type = owned_PyObject::steal_ref(PyType_FromSpec(&py_iterator_spec<T>));
    if (!py_iterator_type) { return nullptr; }
    using it_type = py_iterator<T>;
    auto it = PyObject_New(it_type, py_iterator_type.template get<PyTypeObject>());
    if (!it) { return nullptr; }
    new(&it->cur) typename T::const_iterator(t.begin());
    new(&it->end) typename T::const_iterator(t.end());
    new(&it->immutable) bool(immutable);
    return owned_PyObject::steal_ref(it);
  }

  // Use template and explicit specialisations to suppress implicit conversions
  template <typename T> struct PyObject_conv;

  // At the point at which this is thrown the python exception should already be set
  struct from_PyObject_failed {};

  template <>
  struct PyObject_conv<view_PyObject> {
    static auto to_py(view_PyObject obj, bool) -> owned_PyObject { return obj; }
    static auto from_py(view_PyObject obj) -> view_PyObject { return obj; }
  };

  template <>
  struct PyObject_conv<owned_PyObject> {
    static auto to_py(owned_PyObject obj, bool) -> owned_PyObject { return obj; }
    static auto from_py(view_PyObject obj) -> owned_PyObject { return obj; }
  };

  template <>
  struct PyObject_conv<bool> {
    static auto to_py(bool b, bool) -> owned_PyObject {
      return owned_PyObject::steal_ref(PyBool_FromLong(b));
    }

    static auto from_py(view_PyObject obj) -> bool {
      if (!PyBool_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a bool");
        throw from_PyObject_failed{};
      }
      auto b = PyObject_IsTrue(obj);
      if (b < 0) { throw from_PyObject_failed{}; }
      return b;
    }
  };

  template <> struct PyObject_conv<uint8_t> {
    static auto to_py(uint8_t x, bool) -> owned_PyObject { return owned_PyObject::steal_ref(PyLong_FromLong(x)); }
    static auto from_py(view_PyObject obj) -> uint8_t {
      if (!PyLong_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a uint8_t");
        throw from_PyObject_failed{};
      }
      auto x = PyLong_AsLong(obj);
      if (PyErr_Occurred()) { throw from_PyObject_failed{}; }
      return x;
    }
  };

  template <> struct PyObject_conv<int> {
    static auto to_py(int x, bool) -> owned_PyObject { return owned_PyObject::steal_ref(PyLong_FromLong(x)); }
    static auto from_py(view_PyObject obj) -> int {
      if (!PyLong_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected an int");
        throw from_PyObject_failed{};
      }
      auto x = PyLong_AsLong(obj);
      if (PyErr_Occurred()) { throw from_PyObject_failed{}; }
      return x;
    }

    static auto parse_type() { return std::tuple<int>{}; }
    static constexpr auto format = "i";
    static auto post_parse(int x) -> int { return x; }
  };

  template <> struct PyObject_conv<long> {
    static auto to_py(long x, bool) -> owned_PyObject { return owned_PyObject::steal_ref(PyLong_FromLong(x)); }
    static auto from_py(view_PyObject obj) -> long {
      if (!PyLong_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a long");
        throw from_PyObject_failed{};
      }
      auto x = PyLong_AsLong(obj);
      if (PyErr_Occurred()) { throw from_PyObject_failed{}; }
      return x;
    }

    static auto parse_type() { return std::tuple<long>{}; }
    static constexpr auto format = "l";
    static auto post_parse(long x) -> long { return x; }
  };

  template <> struct PyObject_conv<unsigned long> {
    static auto to_py(unsigned long x, bool) -> owned_PyObject { return owned_PyObject::steal_ref(PyLong_FromUnsignedLong(x)); }
    static auto from_py(view_PyObject obj) -> unsigned long {
      if (!PyLong_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a long");
        throw from_PyObject_failed{};
      }
      auto x = PyLong_AsUnsignedLong(obj);
      if (PyErr_Occurred()) { throw from_PyObject_failed{}; }
      return x;
    }

    static auto parse_type() { return std::tuple<unsigned long>{}; }
    static constexpr auto format = "k";
    static auto post_parse(unsigned long x) -> unsigned long { return x; }
  };

  template <> struct PyObject_conv<long long> {
    static auto to_py(long long x, bool) -> owned_PyObject { return owned_PyObject::steal_ref(PyLong_FromLongLong(x)); }
    static auto from_py(view_PyObject obj) -> long long {
      if (!PyLong_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a long");
        throw from_PyObject_failed{};
      }
      auto x = PyLong_AsLongLong(obj);
      if (PyErr_Occurred()) { throw from_PyObject_failed{}; }
      return x;
    }

    static auto parse_type() { return std::tuple<long long>{}; }
    static constexpr auto format = "L";
    static auto post_parse(long long x) -> long long { return x; }
  };

  template <> struct PyObject_conv<double> {
    static auto to_py(double x, bool) -> owned_PyObject { return owned_PyObject::steal_ref(PyFloat_FromDouble(x)); }
    static auto from_py(view_PyObject obj) -> unsigned long {
      if (!PyFloat_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a float");
        throw from_PyObject_failed{};
      }
      auto x = PyFloat_AsDouble(obj);
      if (PyErr_Occurred()) { throw from_PyObject_failed{}; }
      return x;
    }
  };

  template <>
  struct PyObject_conv<std::string> {
    static auto to_py(std::string const & str, bool) -> owned_PyObject {
      return owned_PyObject::steal_ref(PyUnicode_FromStringAndSize(str.data(), str.size()));
    }
    static auto from_py(view_PyObject str_obj) -> std::string {
      if (!PyUnicode_Check(str_obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a str");
        throw from_PyObject_failed{};
      }
      // Would like to use PyUnicode_AsUTF8AndSize but that doesn't join the Limited API until 3.10
      auto bytes_obj = PyUnicode_AsUTF8String(str_obj);
      if (!bytes_obj) { throw from_PyObject_failed{}; }
      char* data;
      Py_ssize_t size;
      if (PyBytes_AsStringAndSize(bytes_obj, &data, &size) < 0) { throw from_PyObject_failed{}; }
      return {data, static_cast<std::size_t>(size)};
    }
  };

  template <>
  struct PyObject_conv<std::string_view> {
    static auto to_py(std::string_view str, bool) -> owned_PyObject {
      return owned_PyObject::steal_ref(PyUnicode_FromStringAndSize(str.data(), str.size()));
    }
  };

  template <typename First, typename Second>
  struct PyObject_conv<std::pair<First, Second>> {
    static auto to_py(std::pair<First, Second> const &, bool immutable) -> owned_PyObject;
    static auto from_py(view_PyObject) -> std::pair<First, Second>;
  };

  template <typename ...Args>
  struct PyObject_conv<std::tuple<Args...>> {
    template <std::size_t ...Is>
    static auto to_py_index_sequence(std::tuple<Args...> const &, bool immutable, std::index_sequence<Is...>) -> owned_PyObject;

    static auto to_py(std::tuple<Args...> const & tuple, bool immutable) -> owned_PyObject {
      return to_py_index_sequence(tuple, immutable, std::index_sequence_for<Args...>());
    }

    template <std::size_t ...Is>
    static auto from_py_index_sequence(view_PyObject, std::index_sequence<Is...>) -> std::tuple<Args...>;

    static auto from_py(view_PyObject obj) -> std::tuple<Args...> {
      return from_py_index_sequence(obj, std::index_sequence_for<Args...>());
    }
  };

  template <typename T>
  struct PyObject_conv<std::vector<T>> {
    static auto to_py(std::vector<T> const &, bool immutable) -> owned_PyObject;
    static auto from_py(view_PyObject) -> std::vector<T>;
  };

  template <typename Key, typename T>
  struct PyObject_conv<std::map<Key, T>> {
    static auto to_py(std::map<Key, T> const &, bool immutable) -> owned_PyObject;
    static auto from_py(view_PyObject) -> std::map<Key, T>;
  };

  template <typename T>
  struct PyObject_conv<std::reference_wrapper<T>> {
    static auto to_py(std::reference_wrapper<T>, bool immutable) -> owned_PyObject;
    static auto from_py(view_PyObject) -> std::reference_wrapper<T> = delete;
  };

  template <typename T> auto to_PyObject(T const & x, bool immutable) -> owned_PyObject {
    return PyObject_conv<T>::to_py(x, immutable);
  }

  template <typename T> auto from_PyObject(view_PyObject obj) -> T {
    return PyObject_conv<T>::from_py(obj);
  }

  template <typename ...Args>
  auto view_PyObject::operator()(Args const & ...args) const -> owned_PyObject {
    auto py_args = to_PyObject(std::tuple{std::ref(args)...}, false);
    return owned_PyObject::steal_ref(PyObject_CallObject(object, py_args.get()));
  }

  template <typename First, typename Second>
  auto PyObject_conv<std::pair<First, Second>>::to_py(std::pair<First, Second> const & pair, bool immutable) -> owned_PyObject {
    auto & [x, y] = pair;
    auto x_py = to_PyObject(x, immutable);
    if (!x_py) { return nullptr; }
    auto y_py = to_PyObject(y, immutable);
    if (!y_py) { return nullptr; }
    return owned_PyObject::steal_ref(PyTuple_Pack(2, x_py.get(), y_py.get()));
  }

  template <typename First, typename Second>
  auto PyObject_conv<std::pair<First, Second>>::from_py(view_PyObject obj) -> std::pair<First, Second> {
    if (!PyTuple_Check(obj)) {
      PyErr_SetString(PyExc_TypeError, "expected a tuple");
      throw from_PyObject_failed{};
    }
    auto x = PyTuple_GetItem(obj, 0);
    if (!x) { throw from_PyObject_failed{}; }
    auto y = PyTuple_GetItem(obj, 1);
    if (!y) { throw from_PyObject_failed{}; }
    return {from_PyObject<First>(x), from_PyObject<Second>(y)};
  }

  template <typename ...Args>
  template <std::size_t ...Is>
  auto PyObject_conv<std::tuple<Args...>>::to_py_index_sequence(std::tuple<Args...> const & tuple, bool immutable, std::index_sequence<Is...>) -> owned_PyObject {
    auto tuple_PyObject = std::tuple{to_PyObject(std::get<Is>(tuple), immutable)...};

    if ((!std::get<Is>(tuple_PyObject) || ...)) {
      return nullptr;
    }

    return owned_PyObject::steal_ref
      ( PyTuple_Pack
        ( sizeof...(Args)
        , std::get<Is>(tuple_PyObject).get()...
        )
      );
  }

  template <typename ...Args>
  template <std::size_t ...Is>
  auto PyObject_conv<std::tuple<Args...>>::from_py_index_sequence(view_PyObject obj, std::index_sequence<Is...>) -> std::tuple<Args...> {
    if (!PyTuple_Check(obj)) {
      PyErr_SetString(PyExc_TypeError, "expected a tuple");
      throw from_PyObject_failed{};
    }
    return
      { [&] {
          auto x = PyTuple_GetItem(obj, Is);
          if (!x) { throw from_PyObject_failed{}; }
          return from_PyObject<std::tuple_element_t<Is, std::tuple<Args...>>>(x);
        }()...
      };
  }

  template <typename T>
  auto PyObject_conv<std::vector<T>>::to_py(std::vector<T> const & vect, bool immutable) -> owned_PyObject {
    if (immutable) {
      return view_PyObject{&PyTuple_Type}(py_iterator_factory(vect, immutable));
    } else {
      return view_PyObject{&PyList_Type}(py_iterator_factory(vect, immutable));
    }
  }

  template <typename T>
  auto PyObject_conv<std::vector<T>>::from_py(view_PyObject obj) -> std::vector<T> {
    if (!PyList_Check(obj)) {
      PyErr_SetString(PyExc_TypeError, "expected a list");
      throw from_PyObject_failed{};
    }
    auto vect = std::vector<T>{};
    auto n = PyList_Size(obj);
    vect.reserve(n);
    for (int i = 0; i < n; ++i) {
      vect.push_back(from_PyObject<T>(PyList_GetItem(obj, i)));
    }
    return vect;
  }

  template <typename Key, typename T>
  auto PyObject_conv<std::map<Key, T>>::to_py(std::map<Key, T> const & map, bool) -> owned_PyObject {
    return view_PyObject{&PyDict_Type}(py_iterator_factory(map, true));
  }

  template <typename Key, typename T>
  auto PyObject_conv<std::map<Key, T>>::from_py(view_PyObject obj) -> std::map<Key, T> {
    if (!PyDict_Check(obj)) {
      PyErr_SetString(PyExc_TypeError, "expected a dict");
      throw from_PyObject_failed{};
    }
    auto map = std::map<Key, T>{};
    Py_ssize_t pos = 0;
    PyObject* key;
    PyObject* value;
    while (PyDict_Next(obj, &pos, &key, &value)) {
      map.emplace(from_PyObject<Key>(key), from_PyObject<T>(value));
    }
    return map;
  }

  template <typename T>
  auto PyObject_conv<std::reference_wrapper<T>>::to_py(std::reference_wrapper<T> ref, bool immutable) -> owned_PyObject {
    return to_PyObject(ref.get(), immutable);
  }

  template <typename T>
  auto py_iterator<T>::next(py_iterator* self) -> PyObject* {
    if (self->cur == self->end) {
      return nullptr;
    } else {
      return to_PyObject(*(self->cur++), self->immutable).release();
    }
  }
}

static view_PyObject parameter_type = nullptr;
static view_PyObject ls9_type = nullptr;

template <>
struct PyObject_conv<Parameter> {
  static auto to_py(Parameter param, bool) -> owned_PyObject { return parameter_type(param.element, param.index, param.channel); }
  static auto from_py(view_PyObject obj) -> Parameter {
    if (!PyObject_IsInstance(obj, parameter_type)) {
      PyErr_SetString(PyExc_TypeError, "expected a Parameter");
      throw from_PyObject_failed{};
    }
    return
      { from_PyObject<int>(obj["element"])
      , from_PyObject<int>(obj["index"])
      , from_PyObject<int>(obj["channel"])
      };
  }

  static auto parse_type() { return std::tuple<PyObject&, PyObject*>{*parameter_type, nullptr}; }
  static constexpr auto format = "O!";
  static auto post_parse(PyObject&, PyObject* obj) -> Parameter { return from_py(obj); }
};

template <typename Rep, typename Period>
struct PyObject_conv<std::chrono::duration<Rep, Period>> {
  private:
    using convRep = PyObject_conv<Rep>;

  public:
    static auto to_py(std::chrono::duration<Rep, Period> dur, bool) -> owned_PyObject { return convRep::to_py(dur.count()); }
    static auto from_py(view_PyObject obj) -> std::chrono::duration<Rep, Period> { return std::chrono::duration<Rep, Period>{convRep::from_py(obj)}; }

    static auto parse_type() { return convRep::parse_type(); }
    static constexpr auto format = convRep::format;
    static constexpr auto post_parse = [](auto&& ...args) -> std::chrono::duration<Rep, Period> { return std::chrono::duration<Rep, Period>{convRep::post_parse(std::forward<decltype(args)>(args)...)}; };
};

template <typename ...Ts>
constexpr auto tuple_to_references(std::tuple<Ts...> & xs) noexcept -> std::tuple<Ts&...> {
  return std::apply(std::tie<Ts...>, xs);
}

template <typename F, typename ...Ts, std::size_t ...Is>
constexpr auto tuple_map_impl(F const & f, std::tuple<Ts...>& xs, std::index_sequence<Is...>) noexcept {
  return std::tuple{f(std::get<Is>(xs))...};
}

template <typename F, typename ...Ts>
constexpr auto tuple_map(F const & f, std::tuple<Ts...>& xs) noexcept {
  return tuple_map_impl(f, xs, std::index_sequence_for<Ts...>{});
}

template <typename ...Ts>
constexpr auto tuple_concat(std::tuple<Ts...> const & xss) noexcept {
  return std::apply(std::tuple_cat<Ts const &...>, xss);
}

template <typename Ret>
auto lift(auto (*f)() -> Ret, PyObject* pyArgs, bool releaseGIL) -> PyObject* {
  auto f2 = [&] {
    auto gil = releaseGIL ? std::make_optional<scoped_release_gil>() : std::nullopt;
    return (*f)();
  };
  if constexpr (std::is_void_v<Ret>) {
    f2();
    Py_RETURN_NONE;
  } else {
    return to_PyObject(f2(), false).release();
  }
}

template <typename Ret, typename ...Args, std::size_t ...Is>
auto lift_impl(auto (*f)(Args...) -> Ret, PyObject* pyArgs, bool releaseGIL, std::index_sequence<Is...>) -> PyObject* {
  auto args = std::tuple{PyObject_conv<Args>::parse_type()...};
  auto format = (std::string{PyObject_conv<Args>::format} + ... + ""s);
  if( std::apply
      ( [&](auto& ...args) { return PyArg_ParseTuple(pyArgs, format.c_str(), &args...); }
      , tuple_concat(tuple_map([] (auto& x) { return tuple_to_references(x); }, args))
      )
    ) {
    try {
      auto parsed_args = std::tuple{std::apply(PyObject_conv<Args>::post_parse, std::get<Is>(args))...};
      auto f2 = [&] {
        auto gil = releaseGIL ? std::make_optional<scoped_release_gil>() : std::nullopt;
        return (*f)(std::move(std::get<Is>(parsed_args))...);
      };
      if constexpr (std::is_void_v<Ret>) {
        f2();
        Py_RETURN_NONE;
      } else {
        return to_PyObject(f2(), false).release();
      }
    } catch (from_PyObject_failed const &) {
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

template <typename Ret, typename ...Args>
auto lift(auto (*f)(Args...) -> Ret, PyObject* pyArgs, bool releaseGIL) -> PyObject* {
  return lift_impl(f, pyArgs, releaseGIL, std::index_sequence_for<Args...>{});
}

template <typename T, typename Ret>
auto lift(auto (T::*f)() -> Ret, T& obj, PyObject* pyArgs, bool releaseGIL) -> PyObject* {
  auto f2 = [&] {
    auto gil = releaseGIL ? std::make_optional<scoped_release_gil>() : std::nullopt;
    return (obj.*f)();
  };
  if constexpr (std::is_void_v<Ret>) {
    f2();
    Py_RETURN_NONE;
  } else {
    return to_PyObject(f2(), false).release();
  }
}

template <typename T, typename Ret, typename ...Args, std::size_t ...Is>
auto lift_impl(auto (T::*f)(Args...) -> Ret, T& obj, PyObject* pyArgs, bool releaseGIL, std::index_sequence<Is...>) -> PyObject* {
  auto args = std::tuple{PyObject_conv<Args>::parse_type()...};
  auto format = (std::string{PyObject_conv<Args>::format} + ... + ""s);
  if( std::apply
      ( [&](auto& ...args) { return PyArg_ParseTuple(pyArgs, format.c_str(), &args...); }
      , tuple_concat(tuple_map([] (auto& x) { return tuple_to_references(x); }, args))
      )
    ) {
    try {
      auto parsed_args = std::tuple{std::apply(PyObject_conv<Args>::post_parse, std::get<Is>(args))...};
      auto f2 = [&] {
        auto gil = releaseGIL ? std::make_optional<scoped_release_gil>() : std::nullopt;
        return (obj.*f)(std::move(std::get<Is>(parsed_args))...);
      };
      if constexpr (std::is_void_v<Ret>) {
        f2();
        Py_RETURN_NONE;
      } else {
        return to_PyObject(f2(), false).release();
      }
    } catch (from_PyObject_failed const &) {
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

template <typename T, typename Ret, typename ...Args>
auto lift(auto (T::*f)(Args...) -> Ret, T& obj, PyObject* pyArgs, bool releaseGIL) -> PyObject* {
  return lift_impl(f, obj, pyArgs, releaseGIL, std::index_sequence_for<Args...>{});
}

template <auto f, auto x, bool releaseGIL>
struct member_t;

template <typename T, typename Ret, typename ...Args, typename PyT, auto (T::*f)(Args...) -> Ret, T PyT::*x, bool releaseGIL>
struct member_t<f, x, releaseGIL> {
  static auto run(PyObject* obj, PyObject* pyArgs) -> PyObject* {
    return lift(f, static_cast<PyT*>(obj)->*x, pyArgs, releaseGIL);
  }
};

template <auto f, auto x, bool releaseGIL>
auto member(PyObject* obj, PyObject* pyArgs) -> PyObject* {
  return member_t<f, x, releaseGIL>::run(obj, pyArgs);
}

template <auto f, bool releaseGIL>
struct staticmember_t;

template <typename Ret, typename ...Args, auto (*f)(Args...) -> Ret, bool releaseGIL>
struct staticmember_t<f, releaseGIL> {
  static auto run(PyObject* pyArgs) -> PyObject* {
    return lift(f, pyArgs, releaseGIL);
  }
};

template <auto f, bool releaseGIL>
auto staticmember(PyObject*, PyObject* pyArgs) -> PyObject* {
  return staticmember_t<f, releaseGIL>::run(pyArgs);
}

template <auto f, auto x, bool releaseGIL>
auto memberTimeout(PyObject* obj, PyObject* pyArgs) -> PyObject* {
  try {
    return member_t<f, x, releaseGIL>::run(obj, pyArgs);
  } catch (LS9::timeout_expired const &) {
    PyErr_SetNone(PyExc_TimeoutError);
    return nullptr;
  }
}

struct PyLS9 : PyObject {
  LS9 ls9;

  static auto py_new(PyTypeObject* subtype, PyObject* args, PyObject* kwds) -> PyObject* {
    // auto selfObject = owned_PyObject::steal_ref(subtype->tp_alloc(subtype, 0));
    auto selfObject = owned_PyObject::steal_ref(PyType_GenericAlloc(subtype, 0)); // TODO This won't work if subtype has a custom allocator
    auto self = selfObject.get<PyLS9>();
  
    char const * portName;
    static char* kwlist[] = {"portName", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &portName)) {
      PyErr_SetString(PyExc_TypeError, "requires a single string parameter");
      return nullptr;
    }
  
    if (self != nullptr) {
      try {
        new(&self->ls9) LS9{portName};
      } catch (std::exception const & e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
      }
    }
    return selfObject.release();
  }
  
  static void dealloc(PyLS9* self) {
    std::destroy_at(&self->ls9);
  }

  static auto addGlobalCallback(PyLS9* self, PyObject* callback) -> PyObject* {
    if (!PyCallable_Check(callback)) {
      PyErr_SetString(PyExc_TypeError, "Parameter must be callable");
      return nullptr;
    }
    self->ls9.addGlobalCallback
      ( [callback = owned_PyObject::take_ref(callback)] (auto&& ...args) {
          auto gil = scoped_gil{};
          callback(std::forward<decltype(args)>(args)...);
        }
      );
    Py_RETURN_NONE;
  }

  static auto addParamCallback(PyLS9* self, PyObject* args) -> PyObject* {
    PyObject * param;
    PyObject * callback;
    if (!PyArg_ParseTuple(args, "O!O", parameter_type, &param, &callback)) {
      PyErr_SetString(PyExc_TypeError, "requires two parameters");
      return nullptr;
    }
    if (!PyCallable_Check(callback)) {
      PyErr_SetString(PyExc_TypeError, "second parameter must be callable");
      return nullptr;
    }
    self->ls9.addParamCallback
      ( from_PyObject<Parameter>(param)
      , [callback = owned_PyObject::take_ref(callback)] (auto&& ...args) {
          auto gil = scoped_gil{};
          callback(std::forward<decltype(args)>(args)...);
        }
      );
    Py_RETURN_NONE;
  }
};

static PyMethodDef ls9_methods[] =
  { {"addGlobalCallback", reinterpret_cast<PyCFunction>(PyLS9::addGlobalCallback), METH_O, PyDoc_STR("Add a callback for all parameters")}
  , {"addParamCallback", reinterpret_cast<PyCFunction>(PyLS9::addParamCallback), METH_VARARGS, PyDoc_STR("Add a callback for a parameter")}
  , {"portNames", staticmember<&LS9::portNames, false>, METH_VARARGS | METH_STATIC, PyDoc_STR("get a list of all the port names")}
  , {"get", memberTimeout<&LS9::get, &PyLS9::ls9, true>, METH_VARARGS, PyDoc_STR("get the value of a parameter")}
  , {"set", member<&LS9::set, &PyLS9::ls9, false>, METH_VARARGS, PyDoc_STR("Set the value of a parameter")}
  , {"fade", memberTimeout<&LS9::fade, &PyLS9::ls9, true>, METH_VARARGS, PyDoc_STR("Fade a parameter")}
  , {"nextParamTouched", member<&LS9::nextParamTouched, &PyLS9::ls9, true>, METH_NOARGS, PyDoc_STR("Get the next parameter touched")}
  , {"getChannelName", memberTimeout<&LS9::getChannelName, &PyLS9::ls9, true>, METH_VARARGS, PyDoc_STR("Get the name of a channel")}
  , {nullptr}
  };

static PyType_Slot ls9_slots[] =
  { {Py_tp_dealloc, (void*)PyLS9::dealloc}
  , {Py_tp_doc, (void*)PyDoc_STR("The Yamaha LS9 Mixer")}
  , {Py_tp_methods, ls9_methods}
  , {Py_tp_new, (void*)PyLS9::py_new}
  , {0, nullptr}
  };
  
static auto ls9_spec = PyType_Spec
  { .name = "pyopenls9.LS9"
  , .basicsize = sizeof(PyLS9)
  , .itemsize = 0
  , .flags = Py_TPFLAGS_DEFAULT
  , .slots = ls9_slots
  };

static void load_python_types() {
  static auto const python_module_string = R"python(
from dataclasses import dataclass
from typing import Any

@dataclass
class Parameter:
  "Some parameter of the desk"
  element : int
  index : int
  channel : int
      )python";

  auto compiled_module = owned_PyObject::steal_ref(Py_CompileString(python_module_string, "pyopenls9/python.cpp", Py_file_input));
  if (!compiled_module) { return; }

  auto module_object = owned_PyObject::steal_ref(PyImport_ExecCodeModule("pyopenls9", compiled_module));

  parameter_type = module_object["Parameter"];

  ls9_type = PyType_FromSpec(&ls9_spec);
}

static auto moduleDef = PyModuleDef
  { .m_base = PyModuleDef_HEAD_INIT
  , .m_name = "pyopenls9"
  , .m_doc = PyDoc_STR("A library for controlling the Yamaha LS9 mixer")
  , .m_size = -1
  , .m_methods = nullptr
  };

PyMODINIT_FUNC PyInit_pyopenls9() {
  auto m = owned_PyObject::steal_ref(PyModule_Create(&moduleDef));
  if (!m) { return nullptr; }

  load_python_types();

  if (!parameter_type) { return nullptr; }
  Py_INCREF(parameter_type);
  if (PyModule_AddObject(m, "Parameter", parameter_type) < 0) {
    Py_DECREF(parameter_type);
    Py_DECREF(ls9_type);
    return nullptr;
  }

  if (!ls9_type) { return nullptr; }
  Py_INCREF(ls9_type);
  if (PyModule_AddObject(m, "LS9", ls9_type) < 0) {
    Py_DECREF(ls9_type);
    return nullptr;
  }

  return m.release();
}

#pragma GCC diagnostic pop

