#pragma once
#include "python_runtime.h"
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <deque>
#include <optional>

namespace Ayu::Plugins {
// Implement these methods against the actual AyuGram commit. All run on GUI thread.
class HostBridge {
public:
    virtual ~HostBridge() = default;
    virtual bool accountIsCurrent(std::uint64_t epoch) const = 0;
    // Resolve peer in THIS account; check permissions/limits; use normal send pipeline.
    // Mark origin=Plugin and preserve this marker through asynchronous send stages.
    virtual bool sendMessage(std::uint64_t epoch, const std::string &peer,
        const std::string &plainText, const std::string &pluginOwner) = 0;
    virtual void showToast(const QString &text) = 0;
};

class PluginManager final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(Message, bool drop)>;
    explicit PluginManager(RuntimeConfig config, HostBridge &host, QObject *parent = nullptr);
    ~PluginManager() override;
    void start(std::uint64_t accountEpoch);
    // Epoch must be monotonically changed on switch/logout/lock invalidation.
    void activateAccount(std::uint64_t epoch, Grants accountGrants);
    void reload();
    void setEnabled(const QString &id, bool enabled, const QSet<QString> &grants);
    const Grants &grants() const { return _grants; }
    const Snapshot &snapshot() const { return _snapshot; }
    bool ready() const { return _ready; }
    std::uint64_t accountEpoch() const { return _epoch; }
    QStringList logHistory() const { return _logs; }
    void interceptIncoming(Message m, QObject *context, Completion done);
    void interceptOutgoing(Message m, QObject *context, Completion done);
    bool runCommand(const std::string &owner, const std::string &key,
        const std::string &arguments, Message context);
    bool runMenu(const std::string &owner, const std::string &key,
        std::uint64_t generation, Message context);
Q_SIGNALS:
    void changed();
    void grantsChanged(); // adapter persists grants per account, NEVER auto-grant
    void logLine(QString owner, QString text);
private:
    struct Pending {
        Request request;
        QPointer<QObject> context;
        Completion done;
    };
    void requireGui() const;
    void configure();
    void acceptSnapshot(Snapshot snapshot);
    void enqueue(Pending pending);
    void pump();
    void finish(std::optional<Result> result);
    void flush();
    void deliver(Pending pending, Message message, bool drop);
    void commit(const Result &result);
    void report(const std::string &owner, const std::string &text);
    std::optional<std::uint64_t> generationFor(const std::string &owner) const;
    HostBridge &_host;
    Grants _grants;
    Snapshot _snapshot;
    QStringList _logs;
    std::unique_ptr<PythonRuntime> _runtime;
    QTimer _timer;
    std::deque<Pending> _waiting;
    std::optional<Pending> _active;
    std::map<std::string, std::deque<Clock::time_point>> _sendTimes;
    std::uint64_t _epoch = 0, _revision = 1, _nextId = 0;
    bool _runtimeUp = false, _ready = false, _stopping = false;
    bool _pumping = false, _finishing = false;
    Clock::time_point _bypassUntil{};
};
} // namespace Ayu::Plugins
