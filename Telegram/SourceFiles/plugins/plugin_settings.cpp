#include "plugin_settings.h"
#include "plugin_manager.h"
#include "customization_manager.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QVBoxLayout>
#include <optional>

namespace Ayu::Plugins {
namespace {
std::optional<QSet<QString>> requestGrants(QWidget *parent) {
    QDialog dialog;
    QObject::connect(parent, &QObject::destroyed, &dialog, &QDialog::reject);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setWindowTitle(QStringLiteral("Разрешения плагина"));
    auto layout = new QVBoxLayout(&dialog);
    auto warning = new QLabel(QStringLiteral(
        "Плагин выполняет произвольный Python-код внутри клиента.\n"
        "Эти разрешения ограничивают только SDK, а не доступ к файлам или сети.\n"
        "Включайте только проверенные плагины."), &dialog);
    warning->setWordWrap(true); layout->addWidget(warning);
    const std::pair<const char *, const char *> permissions[] = {
        {"send.current", "Отправлять сообщения в текущий чат"},
        {"send.any", "Отправлять в другие чаты (также нужен send.current)"},
        {"toast", "Показывать уведомления"},
        {"commands", "Регистрировать локальные команды"},
        {"menus", "Добавлять пункты контекстного меню"},
    };
    std::vector<std::pair<QString, QCheckBox *>> boxes;
    for (const auto &[id, label] : permissions) {
        auto box = new QCheckBox(QString::fromUtf8(label), &dialog);
        layout->addWidget(box); boxes.emplace_back(QString::fromLatin1(id), box);
    }
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    QSet<QString> result;
    for (const auto &[id, box] : boxes) if (box->isChecked()) result.insert(id);
    return result;
}
}
PluginSettings::PluginSettings(PluginManager &manager, CustomizationManager &customization, QWidget *parent)
: QDialog(parent), _manager(manager), _list(new QListWidget(this)), _console(new QPlainTextEdit(this)) {
    setWindowTitle(QStringLiteral("Плагины")); setObjectName(QStringLiteral("AyuPluginSettings"));
    resize(760, 560);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel(QStringLiteral("Embedded Python: только доверенные плагины; песочницы нет."), this);
    note->setWordWrap(true); layout->addWidget(note); layout->addWidget(_list);
    auto buttons = new QHBoxLayout();
    auto reload = new QPushButton(QStringLiteral("Перезагрузить плагины"), this);
    auto qss = new QPushButton(QStringLiteral("Выбрать QSS…"), this);
    auto reset = new QPushButton(QStringLiteral("Сбросить кастомизацию"), this);
    buttons->addWidget(reload); buttons->addWidget(qss); buttons->addWidget(reset); layout->addLayout(buttons);
    _console->setReadOnly(true); _console->document()->setMaximumBlockCount(500);
    layout->addWidget(new QLabel(QStringLiteral("Консоль ошибок"), this)); layout->addWidget(_console);
    for (const auto &line : manager.logHistory()) _console->appendPlainText(line);
    connect(reload, &QPushButton::clicked, &manager, &PluginManager::reload);
    connect(qss, &QPushButton::clicked, this, [this, &customization] {
        const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("Выбрать QSS"), {}, QStringLiteral("QSS (*.qss)"));
        if (!file.isEmpty()) customization.watchQss(QStringLiteral("user"), file, 100);
    });
    connect(reset, &QPushButton::clicked, &customization, [&customization] { customization.clearOwner(QStringLiteral("user")); });
    connect(&customization, &CustomizationManager::error, this, [this](const QString &text) { _console->appendPlainText(text); });
    connect(&manager, &PluginManager::logLine, this, [this](const QString &owner, const QString &text) {
        _console->appendPlainText(QStringLiteral("[") + owner + QStringLiteral("] ") + text);
    });
    connect(&manager, &PluginManager::changed, this, &PluginSettings::refresh, Qt::QueuedConnection);
    connect(_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        // Copy before nested modal event loop: refresh may destroy the QListWidgetItem.
        const auto id = item->data(Qt::UserRole).toString();
        const auto enabled = item->checkState() == Qt::Checked;
        if (!enabled) { _manager.setEnabled(id, false, {}); return; }
        const auto epoch = _manager.accountEpoch();
        const auto alive = QPointer<PluginSettings>(this);
        auto grants = requestGrants(this);
        if (!alive) return;
        if (_manager.accountEpoch() != epoch) { refresh(); return; }
        if (grants) _manager.setEnabled(id, true, *grants);
        else refresh();
    });
    refresh();
}
void PluginSettings::refresh() {
    QSignalBlocker blocker(_list); _list->clear();
    for (const auto &p : _manager.snapshot().plugins) {
        auto status = p.quarantined ? QStringLiteral("карантин")
            : p.loaded ? QStringLiteral("загружен") : p.enabled ? QStringLiteral("ошибка / загрузка") : QStringLiteral("выключен");
        const auto id = QString::fromStdString(p.id);
        auto item = new QListWidgetItem(id + QStringLiteral(" — ") + status, _list);
        item->setData(Qt::UserRole, id); item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(_manager.grants().contains(p.id) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(QString::fromUtf8(p.error.data(), qsizetype(p.error.size())));
    }
}
} // namespace Ayu::Plugins
