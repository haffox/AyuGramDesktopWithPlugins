#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Ayu::Plugins {
using Clock = std::chrono::steady_clock;
using Grants = std::map<std::string, std::set<std::string>>;
struct Message {
    // Opaque, account-scoped host IDs; never cast these to TDesktop PeerId.
    std::string peerId, messageId, text;
    bool outgoing = false;
    bool service = false;
    bool pluginOrigin = false;
};
enum class Event { Incoming, Outgoing, Command, Menu };
struct Request {
    std::uint64_t id = 0, epoch = 0, revision = 0;
    Event event = Event::Incoming;
    Message message;
    std::string owner, key, argument;
    std::uint64_t generation = 0; // mandatory for menu/command callbacks
    Clock::time_point deadline;
};
struct Effect {
    enum class Kind { Send, Toast } kind;
    std::string owner, peerId, text;
    std::uint64_t generation = 0;
};
struct Result {
    Request request;
    bool drop = false;
    std::vector<Effect> effects;
};
struct MenuItem { std::string id, label; };
struct PluginInfo {
    std::string id, error;
    bool enabled = false, loaded = false, quarantined = false;
    std::uint64_t generation = 0;
    std::vector<std::string> commands;
    std::vector<MenuItem> menus;
};
struct Snapshot {
    std::uint64_t revision = 0;
    std::vector<PluginInfo> plugins;
};
struct RuntimeConfig {
    std::filesystem::path plugins;
    std::wstring pythonHome;
    // Explicit absolute paths: stdlib (or zip), and extension-module directory.
    std::vector<std::wstring> pythonPaths;
    Grants grants; // presence means explicitly enabled; empty means no powers
    std::uint64_t revision = 1;
    std::size_t queueLimit = 128;
    bool enableExteraCompat = false; // also explicitly add trusted compat dir to pythonPaths
};
struct RuntimeSinks {
    // Called on Python worker, never directly touch Qt/UI here.
    std::function<void(bool, std::string)> status;
    std::function<void(Snapshot)> snapshot;
    std::function<void(Result)> result;
    std::function<void(std::string, std::string)> log;
};
class PythonRuntime final {
public:
    PythonRuntime(RuntimeConfig config, RuntimeSinks sinks);
    ~PythonRuntime();
    PythonRuntime(const PythonRuntime &) = delete;
    PythonRuntime &operator=(const PythonRuntime &) = delete;
    void start(); // one interpreter, one lifetime per process
    bool dispatch(Request request);
    bool configure(Grants grants, std::uint64_t revision);
    void stop(); // joins; CANNOT interrupt a hung in-process plugin
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace Ayu::Plugins
