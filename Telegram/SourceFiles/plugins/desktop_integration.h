#pragma once

#include "data/data_msg_id.h"
#include <cstdint>
#include <optional>
#include <functional>
#include <QString>

class QObject;
class QWidget;
class HistoryItem;
namespace Api { struct MessageToSend; }
namespace Main { class Session; }
namespace Ui { class PopupMenu; }

namespace Ayu::Plugins {
void Initialize(QObject *application);
void Shutdown();
void SetSession(Main::Session *session);
void SetLocked(bool locked);
std::uint64_t PresentationEpoch(Main::Session *session);
// Returns true only if the intent was consumed; resume through ApiWrap::sendMessage.
bool InterceptOutgoing(Main::Session *session, Api::MessageToSend &message,
    std::optional<MsgId> localMessageId,
    std::function<void(Api::MessageToSend)> resume = {});
void InterceptIncoming(HistoryItem *item);
void AddMenuActions(Ui::PopupMenu *menu, HistoryItem *item);
void ShowSettings(QWidget *parent = nullptr);
} // namespace Ayu::Plugins
