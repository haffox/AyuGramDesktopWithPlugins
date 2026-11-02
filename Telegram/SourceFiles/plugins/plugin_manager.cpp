#include "plugin_manager.h"
#include <QCoreApplication>
#include <QMetaObject>
#include <QScopedValueRollback>
#include <QThread>
#include <algorithm>
#include <utility>

namespace Ayu::Plugins {
namespace {
constexpr std::size_t LaneLimit = 128;
constexpr std::size_t SendsPerMinute = 5; // conservative host-side sample policy
QString q(const std::string &s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
}
PluginManager::PluginManager(RuntimeConfig config, HostBridge &host, QObject *parent)
: QObject(parent), _host(host), _grants(config.grants), _revision(config.revision) {
    requireGui();
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, [this] { finish(std::nullopt); });
    // The manager remains alive until stop()/join() finishes. Worker callbacks only
    // post queued, context-bound functors; they never read manager/UI state.
    RuntimeSinks sinks;
    sinks.status = [this](bool up, std::string error) {
        QMetaObject::invokeMethod(this, [this, up, e = std::move(error)] {
            if (_stopping) return;
            _runtimeUp = up;
            _ready = up && _snapshot.revision == _revision;
            if (!up) flush();
            if (!e.empty()) report("runtime", e);
            Q_EMIT changed();
        }, Qt::QueuedConnection);
    };
    sinks.snapshot = [this](Snapshot s) {
        QMetaObject::invokeMethod(this, [this, s = std::move(s)]() mutable {
            if (!_stopping) acceptSnapshot(std::move(s));
        }, Qt::QueuedConnection);
    };
    sinks.result = [this](Result r) {
        QMetaObject::invokeMethod(this, [this, r = std::move(r)]() mutable {
            if (_stopping || !_active || _active->request.id != r.request.id) return;
            finish(std::move(r));
        }, Qt::QueuedConnection);
    };
    sinks.log = [this](std::string owner, std::string text) {
        QMetaObject::invokeMethod(this, [this, o = std::move(owner), t = std::move(text)] {
            if (!_stopping) report(o, t);
        }, Qt::QueuedConnection);
    };
    _runtime = std::make_unique<PythonRuntime>(std::move(config), std::move(sinks));
}
PluginManager::~PluginManager() {
    requireGui(); _stopping = true; _ready = false; flush();
    _runtime->stop(); // may block on malicious Python/native code; no safe forced kill
}
void PluginManager::requireGui() const {
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}
void PluginManager::start(std::uint64_t epoch) {
    requireGui(); _epoch = epoch; _runtime->start();
}
void PluginManager::activateAccount(std::uint64_t epoch, Grants grants) {
    requireGui(); _epoch = epoch; _grants = std::move(grants); _sendTimes.clear(); configure();
}
void PluginManager::reload() { requireGui(); configure(); }
void PluginManager::setEnabled(const QString &id, bool enabled, const QSet<QString> &grants) {
    requireGui();
    const auto key = id.toStdString();
    if (!enabled) _grants.erase(key);
    else {
        std::set<std::string> approved;
        for (const auto &g : grants) approved.insert(g.toStdString());
        _grants[key] = std::move(approved);
    }
    Q_EMIT grantsChanged(); configure();
}
void PluginManager::configure() {
    _ready = false; ++_revision; flush();
    for (auto &p : _snapshot.plugins) { p.loaded = false; p.commands.clear(); p.menus.clear(); }
    if (!_runtime->configure(_grants, _revision)) report("runtime", "Reload queue unavailable; plugins bypassed, retry Reload");
    Q_EMIT changed();
}
void PluginManager::acceptSnapshot(Snapshot s) {
    if (s.revision != _revision) return;
    _snapshot = std::move(s); _ready = _runtimeUp;
    Q_EMIT changed();
}
void PluginManager::report(const std::string &owner, const std::string &text) {
    const auto value = q(text.substr(0, 16384));
    _logs.push_back(QStringLiteral("[") + q(owner) + QStringLiteral("] ") + value);
    while (_logs.size() > 500) _logs.removeFirst();
    Q_EMIT logLine(q(owner), value);
}
std::optional<std::uint64_t> PluginManager::generationFor(const std::string &owner) const {
    if (!_ready) return std::nullopt;
    for (const auto &p : _snapshot.plugins) if (p.id == owner && p.loaded) return p.generation;
    return std::nullopt;
}
void PluginManager::interceptIncoming(Message m, QObject *context, Completion done) {
    requireGui(); Request r; r.event = Event::Incoming; r.message = std::move(m);
    r.deadline = Clock::now() + std::chrono::milliseconds(100);
    enqueue({std::move(r), context, std::move(done)});
}
void PluginManager::interceptOutgoing(Message m, QObject *context, Completion done) {
    requireGui(); Request r; r.event = Event::Outgoing; r.message = std::move(m);
    r.deadline = Clock::now() + std::chrono::milliseconds(250);
    enqueue({std::move(r), context, std::move(done)});
}
bool PluginManager::runCommand(const std::string &owner, const std::string &key,
        const std::string &arguments, Message context) {
    requireGui(); auto gen = generationFor(owner); if (!gen) return false;
    bool found = false;
    for (const auto &p : _snapshot.plugins) if (p.id == owner)
        found = std::find(p.commands.begin(), p.commands.end(), key) != p.commands.end();
    if (!found) return false;
    Request r; r.event = Event::Command; r.owner = owner; r.key = key;
    r.argument = arguments; r.generation = *gen; r.message = std::move(context);
    r.deadline = Clock::now() + std::chrono::seconds(1);
    enqueue({std::move(r), {}, {}}); return true;
}
bool PluginManager::runMenu(const std::string &owner, const std::string &key,
        std::uint64_t generation, Message context) {
    requireGui(); auto gen = generationFor(owner);
    if (!gen || *gen != generation) return false;
    Request r; r.event = Event::Menu; r.owner = owner; r.key = key;
    r.generation = generation; r.message = std::move(context);
    r.deadline = Clock::now() + std::chrono::seconds(1);
    enqueue({std::move(r), {}, {}}); return true;
}
void PluginManager::deliver(Pending p, Message message, bool drop) {
    if (!p.done || !p.context) return;
    // Always queued: deterministic continuation order, no callback reentrancy here.
    // Captured continuation must additionally validate its account/session lifetime.
    auto receiver = p.context.data();
    QMetaObject::invokeMethod(receiver,
        [done = std::move(p.done), message = std::move(message), drop]() mutable {
            done(std::move(message), drop);
        }, Qt::QueuedConnection);
}
void PluginManager::flush() {
    _timer.stop();
    if (_active) {
        auto p = std::move(*_active); _active.reset();
        auto original = p.request.message; deliver(std::move(p), std::move(original), false);
    }
    while (!_waiting.empty()) {
        auto p = std::move(_waiting.front()); _waiting.pop_front();
        auto original = p.request.message; deliver(std::move(p), std::move(original), false);
    }
}
void PluginManager::enqueue(Pending p) {
    p.request.id = ++_nextId; p.request.epoch = _epoch; p.request.revision = _revision;
    if (!_ready || !_epoch || !_host.accountIsCurrent(_epoch) || Clock::now() < _bypassUntil) {
        auto original = p.request.message; deliver(std::move(p), std::move(original), false); return;
    }
    if (_waiting.size() + std::size_t(_active.has_value()) >= LaneLimit) {
        // Drain originals IN ORDER before bypassing new events; late replies discarded.
        _bypassUntil = Clock::now() + std::chrono::seconds(1); flush();
        report("runtime", "Hook lane overloaded; ordered fail-open bypass for 1 second");
        auto original = p.request.message; deliver(std::move(p), std::move(original), false); return;
    }
    _waiting.push_back(std::move(p)); pump();
}
void PluginManager::pump() {
    if (_pumping || _finishing || _stopping) return;
    QScopedValueRollback<bool> guard(_pumping, true);
    while (!_active && !_waiting.empty()) {
        auto p = std::move(_waiting.front()); _waiting.pop_front();
        if (!_ready || p.request.revision != _revision || p.request.epoch != _epoch
                || Clock::now() >= p.request.deadline || (p.done && !p.context)) {
            auto original = p.request.message; deliver(std::move(p), std::move(original), false); continue;
        }
        _active = std::move(p);
        if (!_runtime->dispatch(_active->request)) { finish(std::nullopt); continue; }
        auto remain = std::chrono::ceil<std::chrono::milliseconds>(_active->request.deadline - Clock::now()).count();
        _timer.start(int(std::max<std::int64_t>(1, remain)));
    }
}
void PluginManager::finish(std::optional<Result> result) {
    if (!_active) return;
    {
        QScopedValueRollback<bool> guard(_finishing, true);
        _timer.stop(); auto p = std::move(*_active); _active.reset();
        const auto valid = result && result->request.epoch == _epoch
            && result->request.revision == _revision && _ready
            && _host.accountIsCurrent(_epoch) && Clock::now() < p.request.deadline;
        if (valid) {
            deliver(std::move(p), result->request.message, result->drop);
            commit(*result);
        } else {
            auto original = p.request.message; deliver(std::move(p), std::move(original), false);
        }
    }
    pump();
}
void PluginManager::commit(const Result &r) {
    for (const auto &e : r.effects) {
        if (_stopping || !_ready || r.request.epoch != _epoch || r.request.revision != _revision
                || Clock::now() >= r.request.deadline || !_host.accountIsCurrent(_epoch)) break;
        auto gen = generationFor(e.owner);
        auto grants = _grants.find(e.owner);
        if (!gen || *gen != e.generation || grants == _grants.end()) continue;
        if (e.kind == Effect::Kind::Toast) {
            if (grants->second.contains("toast")) _host.showToast(q(e.text));
            continue;
        }
        if (!grants->second.contains("send.current")) continue;
        if (e.peerId != r.request.message.peerId && !grants->second.contains("send.any")) continue;
        auto &times = _sendTimes[e.owner]; const auto now = Clock::now();
        while (!times.empty() && now - times.front() >= std::chrono::minutes(1)) times.pop_front();
        if (times.size() >= SendsPerMinute) { report(e.owner, "Host send rate limit reached"); continue; }
        times.push_back(now);
        if (!_host.sendMessage(_epoch, e.peerId, e.text, e.owner)) report(e.owner, "Host rejected send request");
    }
}
} // namespace Ayu::Plugins
