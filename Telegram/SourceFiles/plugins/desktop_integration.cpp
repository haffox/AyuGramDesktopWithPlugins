#include "plugins/desktop_integration.h"

#include "plugins/plugin_manager.h"
#include "plugins/plugin_settings.h"
#include "plugins/customization_manager.h"
#include "plugins/settings_store.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_chat_participant_status.h"
#include <utility>
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <filesystem>

namespace Ayu::Plugins {
namespace {
std::filesystem::path NativePath(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
std::string Utf8(const QString &text) { return text.toUtf8().toStdString(); }
QString FromUtf8(const std::string &text) {
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

// An encoded PeerId is an opaque *account-scoped* token, not an Android dialog ID.
Message SnapshotOf(HistoryItem *item) {
    Message result;
    result.peerId = std::to_string(item->history()->peer->id.value);
    result.messageId = std::to_string(item->id.bare);
    result.text = Utf8(item->originalText().text);
    result.outgoing = item->out();
    result.service = item->isService();
    return result;
}

class DesktopHost final : public QObject, public HostBridge {
public:
    explicit DesktopHost(QObject *parent) : QObject(parent) {
        const auto root = QCoreApplication::applicationDirPath();
        _profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/ayu-plugins");
        QDir().mkpath(_profile + QStringLiteral("/plugins"));
        RuntimeConfig config;
        config.plugins = NativePath(_profile + QStringLiteral("/plugins"));
        config.pythonHome = (root + QStringLiteral("/python")).toStdWString();
        config.pythonPaths = {
            (root + QStringLiteral("/python/python312.zip")).toStdWString(),
            (root + QStringLiteral("/python")).toStdWString(),
            (root + QStringLiteral("/plugin-sdk")).toStdWString(),
        };
        config.enableExteraCompat = true;
        _customization = std::make_unique<CustomizationManager>();
        _manager = std::make_unique<PluginManager>(std::move(config), *this);
        connect(_manager.get(), &PluginManager::grantsChanged, this, [=] {
            if (const auto session = _session.get(); session && !_locked) {
                if (!saveGrants(grantsFile(session), _manager->grants())) {
                    showToast(QStringLiteral("Не удалось сохранить разрешения плагинов."));
                }
            }
        });
        // Start only once per process. Switches invalidate revisions, not CPython.
        _manager->start(_epoch);
    }
    ~DesktopHost() override {
        _sessionLifetime.destroy();
        if (_settings) delete _settings.data();
        _manager.reset(); // joins worker BEFORE the QObject/host is destroyed
        _customization.reset();
    }
    QString grantsFile(Main::Session *session) const {
        return _profile + QStringLiteral("/account-%1.ini").arg(session->uniqueId());
    }
    void setSession(Main::Session *session) {
        if (_session.get() == session) return;
        if (_settings) delete _settings.data();
        _sessionLifetime.destroy();
        _session = session ? base::make_weak(session) : base::weak_ptr<Main::Session>();
        activate();
        if (session) {
            session->data().itemRemoved() | rpl::on_next([=](not_null<const HistoryItem*> item) {
                _presentations.erase(item->fullId());
            }, _sessionLifetime);
        }
    }
    void setLocked(bool locked) {
        if (_locked == locked) return;
        _locked = locked;
        if (_settings) delete _settings.data();
        activate();
    }
    void activate() {
        ++_epoch;
        const auto session = _session.get();
        _manager->activateAccount(_epoch,
            session && !_locked ? loadGrants(grantsFile(session)) : Grants());
        // Existing presentation overrides are tagged with the previous epoch.
        // Refresh visible old-account views on unlock/switch; canonical data stays intact.
        const auto presentations = std::exchange(_presentations, {});
        for (const auto &[id, weak] : presentations) {
            if (const auto owner = weak.get()) {
                if (const auto item = owner->data().message(id)) {
                    owner->data().requestItemTextRefresh(item);
                    owner->data().requestItemViewRefresh(item);
                }
            }
        }
    }
    bool accountIsCurrent(std::uint64_t epoch) const override {
        return !_locked && _session.get() && epoch == _epoch;
    }
    std::uint64_t epochFor(Main::Session *session) const {
        return !_locked && session && session == _session.get() ? _epoch : 0;
    }
    bool sendMessage(std::uint64_t epoch, const std::string &peerToken,
            const std::string &plainText, const std::string &) override {
        if (!accountIsCurrent(epoch)) return false;
        bool ok = false;
        const auto value = FromUtf8(peerToken).toULongLong(&ok);
        if (!ok || !value || plainText.empty()) return false;
        const auto session = _session.get();
        // Never create arbitrary peers from plugin-supplied integers.
        const auto peer = session->data().peerLoaded(PeerId(PeerIdHelper(value)));
        if (!peer || !Data::CanSendTexts(peer) || peer->slowmodeSecondsLeft() > 0
                || peer->starsPerMessageChecked() > 0) return false;
        auto message = Api::MessageToSend(Api::SendAction(session->data().history(peer->id)));
        message.action.clearDraft = false;
        message.textWithTags.text = FromUtf8(plainText);
        message.ayuPluginOrigin = true;
        message.ayuPluginProcessed = true;
        session->api().sendMessage(std::move(message));
        return true; // accepted into normal native pipeline, NOT delivery confirmation
    }
    void showToast(const QString &text) override { Ui::Toast::Show(text); }
    bool outgoing(Main::Session *session, Api::MessageToSend &message,
            std::optional<MsgId> localId, std::function<void(Api::MessageToSend)> resume) {
        if (message.ayuPluginProcessed || message.ayuPluginOrigin) return false;
        const auto text = message.textWithTags.text;
        const auto command = text.startsWith(QStringLiteral("/plugin:"));
        if (!epochFor(session)) {
            if (command) showToast(QStringLiteral("Команды плагинов недоступны в этом аккаунте."));
            return command; // reserved local namespace never reaches MTProto
        }
        Message dto;
        const auto peerId = message.action.history->peer->id;
        dto.peerId = std::to_string(peerId.value);
        dto.text = Utf8(text);
        dto.outgoing = true;
        if (command) {
            const auto body = text.mid(8).trimmed();
            const auto space = body.indexOf(QChar(' '));
            const auto name = space < 0 ? body : body.left(space);
            const auto slash = name.indexOf(QChar('/'));
            if (slash <= 0 || !_manager->runCommand(Utf8(name.left(slash)),
                    Utf8(name.mid(slash + 1)),
                    space < 0 ? std::string() : Utf8(body.mid(space + 1)), dto)) {
                showToast(QStringLiteral("Локальная команда плагина не найдена."));
            }
            return true;
        }
        if (!_manager->ready()) return false;
        const auto weak = base::make_weak(session);
        const auto epoch = _epoch;
        auto intent = std::move(message);
        intent.ayuPluginProcessed = true;
        _manager->interceptOutgoing(std::move(dto), this,
            [this, weak, epoch, peerId, localId, text, intent = std::move(intent), resume = std::move(resume)](
                    Message result, bool cancel) mutable {
                const auto session = weak.get();
                if (!session || !accountIsCurrent(epoch) || cancel) return;
                // Resolve target again: never use current UI chat or captured History*.
                intent.action.history = session->data().history(peerId);
                const auto changed = FromUtf8(result.text);
                if (changed != text) {
                    intent.textWithTags.text = changed;
                    intent.textWithTags.tags.clear(); // UTF-16 entity offsets are stale
                    intent.webPage = {}; // old link preview must not follow new text
                }
                const auto peer = intent.action.history->peer;
                const auto reply = intent.action.replyTo.messageId
                    ? session->data().message(intent.action.replyTo.messageId) : nullptr;
                const auto topicId = intent.action.replyTo.topicRootId
                    ? intent.action.replyTo.topicRootId
                    : reply ? reply->topicRootId() : Data::ForumTopic::kGeneralId;
                const auto topic = peer->forumTopicFor(topicId);
                if (!(topic ? Data::CanSendTexts(topic) : Data::CanSendTexts(peer))) {
                    showToast(QStringLiteral("Отправка отменена: нет права отправлять текст."));
                    return;
                }
                if (!intent.action.options.scheduled && peer->slowmodeSecondsLeft() > 0) {
                    showToast(QStringLiteral("Отправка отменена: slow mode, повторите позже."));
                    return;
                }
                // All splitting, rights, dice/ephemeral, pricing, reply and RPC logic
                // stays in the actual ApiWrap implementation, and runs exactly once.
                if (resume) resume(std::move(intent));
                else session->api().sendMessage(std::move(intent), localId);
            });
        return true;
    }
    void incoming(HistoryItem *item) {
        if (!item || item->out() || !item->isRegular() || item->ayuPluginObserved()
                || !_manager->ready()) return;
        const auto session = &item->history()->session();
        if (!epochFor(session)) return;
        const auto token = ++_nextMessageToken;
        item->ayuBeginPluginPresentation(_epoch, token);
        const auto id = item->fullId();
        const auto weak = base::make_weak(session);
        const auto epoch = _epoch;
        _presentations.insert_or_assign(id, weak);
        _manager->interceptIncoming(SnapshotOf(item), this,
            [this, weak, epoch, token, id](Message result, bool drop) {
                const auto session = weak.get();
                if (!session || !accountIsCurrent(epoch)) return;
                const auto item = session->data().message(id);
                if (!item || !item->ayuFinishPluginPresentation(token, FromUtf8(result.text), drop)) return;
                session->data().requestItemTextRefresh(item);
                session->data().requestItemViewRefresh(item);
                session->data().requestItemRepaint(item);
            });
        session->data().requestItemViewRefresh(item);
    }
    void addMenu(Ui::PopupMenu *menu, HistoryItem *item) {
        if (!item || !epochFor(&item->history()->session())) return;
        const auto snapshot = SnapshotOf(item);
        const auto epoch = _epoch;
        for (const auto &plugin : _manager->snapshot().plugins) {
            if (!plugin.loaded) continue;
            for (const auto &entry : plugin.menus) {
                menu->addAction(FromUtf8(entry.label), [weak = QPointer<DesktopHost>(this), epoch,
                        owner = plugin.id, key = entry.id,
                        generation = plugin.generation, snapshot] {
                    if (weak && weak->accountIsCurrent(epoch)) weak->_manager->runMenu(owner, key, generation, snapshot);
                });
            }
        }
    }
    void settings(QWidget *parent) {
        if (!_session.get() || _locked) return;
        if (!_settings) _settings = new PluginSettings(*_manager, *_customization, parent);
        _settings->show(); _settings->raise(); _settings->activateWindow();
    }
private:
    QString _profile;
    base::weak_ptr<Main::Session> _session;
    std::unique_ptr<CustomizationManager> _customization;
    std::unique_ptr<PluginManager> _manager;
    QPointer<PluginSettings> _settings;
    // Only IDs + weak sessions, never HistoryItem* across asynchronous boundaries.
    std::map<FullMsgId, base::weak_ptr<Main::Session>> _presentations;
    rpl::lifetime _sessionLifetime;
    std::uint64_t _epoch = 1, _nextMessageToken = 0;
    bool _locked = false;
};
DesktopHost *Host = nullptr;
} // namespace
void Initialize(QObject *application) { if (!Host) Host = new DesktopHost(application); }
void Shutdown() { delete std::exchange(Host, nullptr); }
void SetSession(Main::Session *session) { if (Host) Host->setSession(session); }
void SetLocked(bool locked) { if (Host) Host->setLocked(locked); }
std::uint64_t PresentationEpoch(Main::Session *session) { return Host ? Host->epochFor(session) : 0; }
bool InterceptOutgoing(Main::Session *session, Api::MessageToSend &message,
        std::optional<MsgId> localMessageId, std::function<void(Api::MessageToSend)> resume) {
    // Consume local commands even if initialization failed or is shutting down.
    if (!Host) return message.textWithTags.text.startsWith(QStringLiteral("/plugin:"));
    return Host->outgoing(session, message, localMessageId, std::move(resume));
}
void InterceptIncoming(HistoryItem *item) { if (Host) Host->incoming(item); }
void AddMenuActions(Ui::PopupMenu *menu, HistoryItem *item) { if (Host) Host->addMenu(menu, item); }
void ShowSettings(QWidget *parent) { if (Host) Host->settings(parent); }
} // namespace Ayu::Plugins
