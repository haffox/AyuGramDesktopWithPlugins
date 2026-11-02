#define PY_SSIZE_T_CLEAN
#include <Python.h>
#ifdef Py_GIL_DISABLED
#error This runtime requires a GIL-enabled CPython build.
#endif
#include "python_runtime.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Ayu::Plugins {
namespace {
constexpr std::size_t MaxText = 64 * 1024;
constexpr std::size_t MaxSource = 1024 * 1024;
constexpr std::size_t MaxEffects = 16;
constexpr unsigned QuarantineAfter = 3;
// All Ref objects live/die on the Python thread, with the GIL held.
class Ref final {
public:
    explicit Ref(PyObject *p = nullptr) : _p(p) {}
    ~Ref() { Py_XDECREF(_p); }
    Ref(Ref &&o) noexcept : _p(std::exchange(o._p, nullptr)) {}
    Ref &operator=(Ref &&o) noexcept {
        if (this != &o) { Py_XDECREF(_p); _p = std::exchange(o._p, nullptr); }
        return *this;
    }
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    PyObject *get() const { return _p; }
    PyObject *release() { return std::exchange(_p, nullptr); }
private:
    PyObject *_p;
};
std::string pythonError() {
    Ref exception(PyErr_GetRaisedException());
    if (!exception.get()) return "Python operation failed without an exception";
    std::string result = "Python exception";
    Ref plain(PyObject_Str(exception.get()));
    if (plain.get()) {
        if (auto s = PyUnicode_AsUTF8(plain.get())) result = s;
    }
    PyErr_Clear();
    Ref traceback(PyImport_ImportModule("traceback"));
    if (traceback.get()) {
        Ref lines(PyObject_CallMethod(traceback.get(), "format_exception", "O", exception.get()));
        Ref sep(PyUnicode_FromString(""));
        if (lines.get() && sep.get()) {
            Ref joined(PyUnicode_Join(sep.get(), lines.get()));
            if (joined.get()) {
                if (auto s = PyUnicode_AsUTF8(joined.get())) result = s;
            }
        }
    }
    PyErr_Clear();
    if (result.size() > 16384) result.resize(16384);
    return result;
}
void checked(bool ok) {
    if (!ok) throw std::runtime_error(pythonError());
}
Ref require(PyObject *p) { checked(p != nullptr); return Ref(p); }
std::string utf8(PyObject *o) {
    if (!PyUnicode_Check(o)) throw std::runtime_error("Expected str");
    Py_ssize_t n = 0;
    auto s = PyUnicode_AsUTF8AndSize(o, &n);
    checked(s != nullptr);
    if (n < 0 || std::size_t(n) > MaxText) throw std::runtime_error("Text exceeds host limit");
    return {s, std::size_t(n)};
}
Ref unicode(const std::string &s) {
    return require(PyUnicode_DecodeUTF8(s.data(), Py_ssize_t(s.size()), "strict"));
}
void put(PyObject *dict, const char *key, const std::string &value) {
    auto v = unicode(value);
    checked(PyDict_SetItemString(dict, key, v.get()) == 0);
}
Ref messageDict(const Message &m) {
    auto d = require(PyDict_New());
    put(d.get(), "peer_id", m.peerId); put(d.get(), "message_id", m.messageId);
    put(d.get(), "text", m.text);
    checked(PyDict_SetItemString(d.get(), "outgoing", m.outgoing ? Py_True : Py_False) == 0);
    checked(PyDict_SetItemString(d.get(), "service", m.service ? Py_True : Py_False) == 0);
    checked(PyDict_SetItemString(d.get(), "plugin_origin", m.pluginOrigin ? Py_True : Py_False) == 0);
    return d;
}
std::string pathUtf8(const std::filesystem::path &p) {
    const auto u = p.u8string();
    return {reinterpret_cast<const char *>(u.data()), u.size()};
}
class Gil final {
public:
    Gil() : _state(PyGILState_Ensure()) {}
    ~Gil() { PyGILState_Release(_state); }
    Gil(const Gil &) = delete;
    Gil &operator=(const Gil &) = delete;
private:
    PyGILState_STATE _state;
};
struct MenuRegistration { std::string label; Ref callback; };
struct Plugin {
    std::string id, moduleName, error;
    bool enabled = false, loaded = false, quarantined = false;
    std::uint64_t generation = 0;
    unsigned failures = 0;
    std::set<std::string> grants;
    std::function<void(std::string)> report;
    Clock::time_point logWindow{};
    unsigned logCount = 0;
    Ref module, incoming, outgoing;
    std::map<std::string, Ref> commands;
    std::map<std::string, MenuRegistration> menus;
};
enum class Phase { Register, Callback };
struct Invocation {
    Plugin &plugin;
    Phase phase;
    const Request *request = nullptr;
    std::vector<Effect> *effects = nullptr;
};
thread_local Invocation *active = nullptr;
class Scope final {
public:
    explicit Scope(Invocation &i) : _old(std::exchange(active, &i)) {}
    ~Scope() { active = _old; }
private:
    Invocation *_old;
};
Invocation &context(const char *permission, Phase phase) {
    if (!active) throw std::runtime_error("API is available only inside a host callback");
    if (active->phase != phase) throw std::runtime_error("API unavailable in this phase");
    if (!active->plugin.grants.contains(permission))
        throw std::runtime_error(std::string("Permission denied: ") + permission);
    if (phase == Phase::Callback && Clock::now() >= active->request->deadline)
        throw std::runtime_error("Callback deadline expired");
    return *active;
}
bool identifier(const std::string &s) {
    return std::regex_match(s, std::regex("[a-z][a-z0-9_-]{0,63}"));
}
template <typename F> PyObject *boundary(F &&f) noexcept {
    try { return f(); }
    catch (const std::exception &e) { PyErr_SetString(PyExc_RuntimeError, e.what()); }
    catch (...) { PyErr_SetString(PyExc_RuntimeError, "Unknown host error"); }
    return nullptr;
}
PyObject *apiSend(PyObject *, PyObject *args) {
    return boundary([&]() -> PyObject * {
        PyObject *peer = nullptr, *text = nullptr;
        if (!PyArg_ParseTuple(args, "UU:send_message", &peer, &text)) return nullptr;
        auto &i = context("send.current", Phase::Callback);
        auto peerId = utf8(peer), value = utf8(text);
        if (peerId.empty() || value.empty()) throw std::runtime_error("Empty peer/text");
        if (peerId != i.request->message.peerId && !i.plugin.grants.contains("send.any"))
            throw std::runtime_error("Sending to a different peer requires send.any");
        if (i.effects->size() >= MaxEffects) throw std::runtime_error("Too many effects");
        i.effects->push_back({Effect::Kind::Send, i.plugin.id, peerId, value, i.plugin.generation});
        Py_RETURN_NONE;
    });
}
PyObject *apiToast(PyObject *, PyObject *args) {
    return boundary([&]() -> PyObject * {
        PyObject *text = nullptr;
        if (!PyArg_ParseTuple(args, "U:show_toast", &text)) return nullptr;
        auto &i = context("toast", Phase::Callback);
        if (i.effects->size() >= MaxEffects) throw std::runtime_error("Too many effects");
        i.effects->push_back({Effect::Kind::Toast, i.plugin.id, {}, utf8(text), i.plugin.generation});
        Py_RETURN_NONE;
    });
}
PyObject *apiCommand(PyObject *, PyObject *args) {
    return boundary([&]() -> PyObject * {
        PyObject *command = nullptr, *callback = nullptr;
        if (!PyArg_ParseTuple(args, "UO:register_command", &command, &callback)) return nullptr;
        auto &i = context("commands", Phase::Register);
        const auto name = utf8(command);
        if (!identifier(name) || !PyCallable_Check(callback))
            throw std::runtime_error("Invalid command/callback");
        if (i.plugin.commands.size() >= 32 || i.plugin.commands.contains(name))
            throw std::runtime_error("Duplicate command or registration limit");
        i.plugin.commands.emplace(name, Ref(Py_NewRef(callback)));
        Py_RETURN_NONE;
    });
}
PyObject *apiMenu(PyObject *, PyObject *args) {
    return boundary([&]() -> PyObject * {
        PyObject *id = nullptr, *label = nullptr, *callback = nullptr;
        if (!PyArg_ParseTuple(args, "UUO:add_context_menu_item", &id, &label, &callback)) return nullptr;
        auto &i = context("menus", Phase::Register);
        auto key = utf8(id), title = utf8(label);
        if (!identifier(key) || title.empty() || title.size() > 256 || !PyCallable_Check(callback))
            throw std::runtime_error("Invalid menu registration");
        if (i.plugin.menus.size() >= 32 || i.plugin.menus.contains(key))
            throw std::runtime_error("Duplicate menu id or registration limit");
        i.plugin.menus.emplace(key, MenuRegistration{std::move(title), Ref(Py_NewRef(callback))});
        Py_RETURN_NONE;
    });
}
PyObject *apiAccount(PyObject *, PyObject *) {
    return boundary([&]() -> PyObject * {
        if (!active || !active->request) throw std::runtime_error("No event account in registration phase");
        return PyLong_FromUnsignedLongLong(active->request->epoch);
    });
}
PyObject *apiLog(PyObject *, PyObject *args) {
    return boundary([&]() -> PyObject * {
        PyObject *level = nullptr, *text = nullptr;
        if (!PyArg_ParseTuple(args, "UU:log", &level, &text)) return nullptr;
        if (!active) throw std::runtime_error("Logging requires a host callback");
        auto &plugin = active->plugin;
        const auto now = Clock::now();
        if (now - plugin.logWindow >= std::chrono::seconds(1)) {
            plugin.logWindow = now; plugin.logCount = 0;
        }
        if (plugin.logCount >= 50) {
            if (plugin.logCount == 50) plugin.report("[WARNING] SDK logs rate-limited");
            plugin.logCount = 51; Py_RETURN_NONE;
        }
        ++plugin.logCount;
        auto value = utf8(text);
        if (value.size() > 16384) value.resize(16384);
        active->plugin.report("[" + utf8(level).substr(0, 32) + "] " + value);
        Py_RETURN_NONE;
    });
}
PyMethodDef methods[] = {
    {"get_account_epoch", apiAccount, METH_NOARGS, "Opaque desktop account token, not Android account index."},
    {"log", apiLog, METH_VARARGS, "Write to the host plugin console."},
    {"send_message", apiSend, METH_VARARGS, "Stage a send in the current account."},
    {"show_toast", apiToast, METH_VARARGS, "Stage a toast."},
    {"register_command", apiCommand, METH_VARARGS, "Register a local command."},
    {"add_context_menu_item", apiMenu, METH_VARARGS, "Register a message-menu item."},
    {nullptr, nullptr, 0, nullptr},
};
PyModuleDef moduleDef = {
    PyModuleDef_HEAD_INIT, "ayugram_api", "Capability-checked host API", -1, methods,
    nullptr, nullptr, nullptr, nullptr,
};
PyMODINIT_FUNC PyInit_ayugram_api() {
    Ref m(PyModule_Create(&moduleDef));
    if (!m.get()) return nullptr;
    if (PyModule_AddObjectRef(m.get(), "DROP", Py_False) < 0) return nullptr;
    if (PyModule_AddIntConstant(m.get(), "API_VERSION", 1) < 0) return nullptr;
    return m.release();
}
Ref optionalCallable(PyObject *module, const char *name) {
    Ref result(PyObject_GetAttrString(module, name));
    if (!result.get()) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); return Ref(); }
        throw std::runtime_error(pythonError());
    }
    if (!PyCallable_Check(result.get())) throw std::runtime_error(std::string(name) + " is not callable");
    return result;
}
void removeModule(Plugin &p) {
    if (p.module.get()) {
        try {
            auto unload = optionalCallable(p.module.get(), "_ayu_extera_unload");
            if (unload.get()) {
                // Delete first: an error/recursive cleanup cannot invoke it twice.
                checked(PyObject_DelAttrString(p.module.get(), "_ayu_extera_unload") == 0);
                Invocation call{p, Phase::Register}; Scope scope(call);
                auto ignored = require(PyObject_CallNoArgs(unload.get()));
            }
        } catch (const std::exception &e) {
            if (p.report) p.report(std::string("Unload error: ") + e.what());
        }
    }
    p.menus.clear(); p.commands.clear(); p.incoming = Ref(); p.outgoing = Ref();
    if (!p.moduleName.empty()) {
        if (PyDict_DelItemString(PyImport_GetModuleDict(), p.moduleName.c_str()) < 0) PyErr_Clear();
    }
    p.module = Ref(); p.loaded = false;
}
struct State {
    RuntimeConfig config;
    RuntimeSinks &sinks;
    std::map<std::string, Plugin> plugins;
    std::uint64_t nextGeneration = 0;
    State(RuntimeConfig c, RuntimeSinks &s) : config(std::move(c)), sinks(s) {}
    ~State() { for (auto &[id, p] : plugins) removeModule(p); }
    void log(const std::string &id, const std::string &text) { sinks.log(id, text); }
    Snapshot snapshot() const {
        Snapshot s; s.revision = config.revision;
        for (const auto &[id, p] : plugins) {
            PluginInfo row{id, p.error, p.enabled, p.loaded, p.quarantined, p.generation, {}, {}};
            if (p.loaded) {
                for (const auto &[name, cb] : p.commands) row.commands.push_back(name);
                for (const auto &[name, m] : p.menus) row.menus.push_back({name, m.label});
            }
            s.plugins.push_back(std::move(row));
        }
        return s;
    }
    void publish() { sinks.snapshot(snapshot()); }
    void failure(Plugin &p, std::string error) {
        p.error = std::move(error); ++p.failures; log(p.id, p.error);
        if (p.failures >= QuarantineAfter) {
            p.quarantined = true; removeModule(p);
        }
        publish();
    }
    void load(Plugin &p, const std::filesystem::path &file) {
        try {
            const auto size = std::filesystem::file_size(file);
            if (size > MaxSource) throw std::runtime_error("Plugin source exceeds 1 MiB");
            std::ifstream input(file, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open plugin");
            std::string source((std::istreambuf_iterator<char>(input)), {});
            if (input.bad() || source.size() > MaxSource) throw std::runtime_error("Cannot read plugin");
            p.moduleName = "_ayu_plugin_" + p.id + "_" + std::to_string(p.generation);
            p.module = require(PyModule_New(p.moduleName.c_str()));
            auto globals = PyModule_GetDict(p.module.get()); // borrowed
            auto filename = unicode(pathUtf8(file));
            checked(PyDict_SetItemString(globals, "__file__", filename.get()) == 0);
            checked(PyDict_SetItemString(globals, "__package__", Py_None) == 0);
            checked(PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) == 0);
            checked(PyDict_SetItemString(PyImport_GetModuleDict(), p.moduleName.c_str(), p.module.get()) == 0);
            auto builtins = require(PyImport_ImportModule("builtins"));
            auto bytes = require(PyBytes_FromStringAndSize(source.data(), Py_ssize_t(source.size())));
            auto mode = unicode("exec");
            auto code = require(PyObject_CallMethod(builtins.get(), "compile", "OOO", bytes.get(), filename.get(), mode.get()));
            Invocation call{p, Phase::Register}; Scope scope(call);
            auto ignored = require(PyEval_EvalCode(code.get(), globals, globals));
            if (config.enableExteraCompat) {
                auto bridge = require(PyImport_ImportModule("_ayugram_extera_bridge"));
                auto owner = unicode(p.id);
                auto attached = require(PyObject_CallMethod(bridge.get(), "attach", "OO", p.module.get(), owner.get()));
            }
            p.incoming = optionalCallable(p.module.get(), "on_message_received");
            p.outgoing = optionalCallable(p.module.get(), "on_message_send");
            auto ui = optionalCallable(p.module.get(), "on_ui_init");
            if (ui.get()) { auto unused = require(PyObject_CallNoArgs(ui.get())); }
            p.loaded = true;
        } catch (const std::exception &e) {
            p.error = e.what(); removeModule(p); log(p.id, p.error);
        }
    }
    void rescan(Grants grants, std::uint64_t revision) {
        for (auto &[id, p] : plugins) removeModule(p);
        plugins.clear(); config.grants = std::move(grants); config.revision = revision;
        std::filesystem::create_directories(config.plugins);
        const auto root = std::filesystem::canonical(config.plugins);
        // Deterministic ASCII IDs; skip symlinks and non-.py files. This is NOT a sandbox.
        std::map<std::string, std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(root)) {
            const auto extension = entry.path().extension();
            if (entry.is_symlink() || !entry.is_regular_file()
                || (extension != ".py" && !(config.enableExteraCompat && extension == ".plugin"))) continue;
            const auto id = pathUtf8(entry.path().stem());
            if (identifier(id)) files.emplace(id, entry.path());
        }
        for (const auto &[id, file] : files) {
            auto [it, inserted] = plugins.try_emplace(id);
            auto &p = it->second; p.id = id; p.generation = ++nextGeneration;
            p.report = [this, id](std::string text) { log(id, text); };
            if (auto grant = config.grants.find(id); grant != config.grants.end()) {
                p.enabled = true; p.grants = grant->second; load(p, file);
            }
        }
        publish();
    }
    Result execute(Request request) {
        Result result{std::move(request), false, {}};
        auto &r = result.request;
        if (r.revision != config.revision || Clock::now() >= r.deadline) return result;
        if (r.message.pluginOrigin && r.event == Event::Outgoing) return result;
        if (r.message.text.size() > MaxText) return result;
        if (r.event == Event::Menu || r.event == Event::Command) {
            auto it = plugins.find(r.owner);
            if (it == plugins.end()) return result;
            auto &p = it->second;
            if (!p.loaded || p.generation != r.generation) return result;
            PyObject *callback = nullptr;
            if (r.event == Event::Command) {
                auto cb = p.commands.find(r.key);
                if (cb != p.commands.end()) callback = cb->second.get();
            } else {
                auto cb = p.menus.find(r.key);
                if (cb != p.menus.end()) callback = cb->second.callback.get();
            }
            if (!callback) return result;
            try {
                Invocation call{p, Phase::Callback, &r, &result.effects}; Scope scope(call);
                auto m = messageDict(r.message);
                if (r.event == Event::Command) put(m.get(), "arguments", r.argument);
                auto ignored = require(PyObject_CallOneArg(callback, m.get()));
                p.failures = 0;
            } catch (const std::exception &e) { result.effects.clear(); failure(p, e.what()); }
            return result;
        }
        // Alphabetical plugin order, transactional change/effects per plugin.
        for (auto &[id, p] : plugins) {
            if (Clock::now() >= r.deadline) break;
            if (!p.loaded) continue;
            auto hook = r.event == Event::Incoming ? p.incoming.get() : p.outgoing.get();
            if (!hook) continue;
            std::vector<Effect> effects;
            try {
                Invocation call{p, Phase::Callback, &r, &effects}; Scope scope(call);
                Ref returned;
                if (r.event == Event::Incoming) {
                    auto m = messageDict(r.message);
                    returned = require(PyObject_CallOneArg(hook, m.get()));
                } else {
                    auto text = unicode(r.message.text), peer = unicode(r.message.peerId);
                    returned = require(PyObject_CallFunctionObjArgs(hook, text.get(), peer.get(), nullptr));
                }
                std::string next = r.message.text;
                bool drop = false;
                if (returned.get() == Py_False) drop = true;
                else if (returned.get() != Py_None) next = utf8(returned.get());
                if (result.effects.size() + effects.size() > 64)
                    throw std::runtime_error("Pipeline effect limit exceeded");
                r.message.text = std::move(next); result.drop = drop;
                result.effects.insert(result.effects.end(), effects.begin(), effects.end());
                p.failures = 0;
                if (drop) break;
            } catch (const std::exception &e) { failure(p, e.what()); }
        }
        return result;
    }
};
std::atomic_flag interpreterUsed = ATOMIC_FLAG_INIT;
} // namespace

struct PythonRuntime::Impl {
    RuntimeConfig config;
    RuntimeSinks sinks;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void(State &)>> queue;
    bool stopping = false, started = false;
    std::thread worker;
    Impl(RuntimeConfig c, RuntimeSinks s) : config(std::move(c)), sinks(std::move(s)) {}
    bool post(std::function<void(State &)> task) {
        std::lock_guard lock(mutex);
        if (stopping || !started || queue.size() >= config.queueLimit) return false;
        queue.push_back(std::move(task)); cv.notify_one(); return true;
    }
    void run() noexcept {
        bool initialized = false;
        PyThreadState *saved = nullptr;
        std::unique_ptr<State> state;
        try {
            if (interpreterUsed.test_and_set()) throw std::runtime_error("Python runtime is process-singleton; restart client to reinitialize");
            if (Py_IsInitialized()) throw std::runtime_error("An external Python interpreter already exists");
            if (PyImport_AppendInittab("ayugram_api", &PyInit_ayugram_api) == -1)
                throw std::runtime_error("Cannot register ayugram_api");
            struct Config {
                PyConfig value;
                Config() { PyConfig_InitIsolatedConfig(&value); }
                ~Config() { PyConfig_Clear(&value); }
            } cfg;
            auto check = [](PyStatus status) {
                if (PyStatus_Exception(status)) throw std::runtime_error(status.err_msg ? status.err_msg : "CPython configuration failed");
            };
            cfg.value.site_import = 0; cfg.value.write_bytecode = 0;
            cfg.value.install_signal_handlers = 0; cfg.value.parse_argv = 0;
            cfg.value.safe_path = 1;
            if (!config.pythonHome.empty()) check(PyConfig_SetString(&cfg.value, &cfg.value.home, config.pythonHome.c_str()));
            if (config.pythonPaths.empty()) throw std::runtime_error("Explicit Python library paths are required");
            cfg.value.module_search_paths_set = 1;
            for (const auto &path : config.pythonPaths) {
                if (!std::filesystem::path(path).is_absolute()) throw std::runtime_error("Python search paths must be absolute");
                check(PyWideStringList_Append(&cfg.value.module_search_paths, path.c_str()));
            }
            check(Py_InitializeFromConfig(&cfg.value)); initialized = true;
            state = std::make_unique<State>(config, sinks);
            state->rescan(config.grants, config.revision);
            sinks.status(true, {});
            saved = PyEval_SaveThread(); // owner thread releases GIL while idle
            for (;;) {
                std::function<void(State &)> task;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping) break;
                    task = std::move(queue.front()); queue.pop_front();
                }
                Gil gil; // never acquire this on the GUI thread
                task(*state);
            }
            PyEval_RestoreThread(saved); saved = nullptr;
            state.reset(); // ALL Python references gone before finalization
            const auto rc = Py_FinalizeEx(); initialized = false;
            sinks.status(false, rc == 0 ? std::string() : "Py_FinalizeEx returned an error");
        } catch (const std::exception &e) {
            const auto error = std::string(e.what());
            if (saved) { PyEval_RestoreThread(saved); saved = nullptr; }
            state.reset();
            if (initialized) Py_FinalizeEx();
            try { sinks.status(false, error); } catch (...) {}
        } catch (...) {
            if (saved) PyEval_RestoreThread(saved);
            state.reset();
            if (initialized) Py_FinalizeEx();
            try { sinks.status(false, "Unhandled runtime failure"); } catch (...) {}
        }
        std::lock_guard lock(mutex); stopping = true; queue.clear();
    }
};
PythonRuntime::PythonRuntime(RuntimeConfig config, RuntimeSinks sinks)
: _impl(std::make_unique<Impl>(std::move(config), std::move(sinks))) {}
PythonRuntime::~PythonRuntime() { stop(); }
void PythonRuntime::start() {
    std::lock_guard lock(_impl->mutex);
    if (_impl->started || _impl->stopping) throw std::logic_error("Runtime already started/stopped");
    _impl->started = true;
    _impl->worker = std::thread([this] { _impl->run(); });
}
bool PythonRuntime::dispatch(Request request) {
    return _impl->post([r = std::move(request)](State &s) mutable {
        auto original = r;
        try { s.sinks.result(s.execute(std::move(r))); }
        catch (const std::exception &e) {
            s.log("runtime", e.what()); s.sinks.result(Result{std::move(original), false, {}});
        }
    });
}
bool PythonRuntime::configure(Grants grants, std::uint64_t revision) {
    return _impl->post([g = std::move(grants), revision](State &s) mutable {
        try { s.rescan(std::move(g), revision); }
        catch (const std::exception &e) { s.log("runtime", e.what()); s.publish(); }
    });
}
void PythonRuntime::stop() {
    { std::lock_guard lock(_impl->mutex); _impl->stopping = true; _impl->cv.notify_one(); }
    if (_impl->worker.joinable()) _impl->worker.join();
}
} // namespace Ayu::Plugins
