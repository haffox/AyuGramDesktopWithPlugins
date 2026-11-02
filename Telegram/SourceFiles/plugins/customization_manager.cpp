#include "customization_manager.h"
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringDecoder>
#include <QThread>
#include <algorithm>
#include <vector>

namespace Ayu::Plugins {
namespace { constexpr qsizetype MaxQss = 256 * 1024; }
CustomizationManager::CustomizationManager(QObject *parent) : QObject(parent) {
    requireGui(); _baseQss = qApp->styleSheet();
    _debounce.setSingleShot(true); _debounce.setInterval(100);
    connect(&_watcher, &QFileSystemWatcher::fileChanged, this, [this] { _debounce.start(); });
    connect(&_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { _debounce.start(); });
    connect(&_debounce, &QTimer::timeout, this, &CustomizationManager::readFiles);
}
void CustomizationManager::requireGui() const {
    Q_ASSERT(qApp && QThread::currentThread() == qApp->thread());
}
void CustomizationManager::setBaseStyleSheet(QString qss) {
    requireGui(); _baseQss = std::move(qss); rebuild();
}
bool CustomizationManager::setQss(QString owner, QString qss, int priority) {
    requireGui();
    if (qss.toUtf8().size() > MaxQss) { Q_EMIT error(QStringLiteral("QSS exceeds 256 KiB")); return false; }
    auto &layer = _layers[owner]; layer.qss = std::move(qss); layer.priority = priority; rebuild(); return true;
}
void CustomizationManager::watchQss(QString owner, QString file, int priority) {
    requireGui(); _files[owner] = {QFileInfo(file).absoluteFilePath(), priority}; readFiles();
}
void CustomizationManager::readFiles() {
    for (const auto &[owner, w] : _files) {
        QFile input(w.path);
        if (!input.open(QIODevice::ReadOnly) || input.size() > MaxQss) {
            Q_EMIT error(QStringLiteral("Cannot read QSS: ") + w.path); continue;
        }
        const auto bytes = input.read(MaxQss + 1);
        if (input.error() != QFileDevice::NoError || bytes.size() > MaxQss) {
            Q_EMIT error(QStringLiteral("QSS read/size error: ") + w.path); continue;
        }
        QStringDecoder decoder(QStringDecoder::Utf8);
        QString text = decoder(bytes);
        if (decoder.hasError()) { Q_EMIT error(QStringLiteral("QSS must be UTF-8: ") + w.path); continue; }
        auto &layer = _layers[owner]; layer.qss = std::move(text); layer.priority = w.priority;
    }
    rebuildWatches(); rebuild();
}
void CustomizationManager::rebuildWatches() {
    if (!_watcher.files().empty()) _watcher.removePaths(_watcher.files());
    if (!_watcher.directories().empty()) _watcher.removePaths(_watcher.directories());
    QSet<QString> paths;
    for (const auto &[owner, w] : _files) {
        const QFileInfo info(w.path);
        if (info.exists()) paths.insert(info.absoluteFilePath());
        paths.insert(info.absolutePath()); // catches atomic-save replacement
    }
    if (!paths.empty()) _watcher.addPaths(paths.values());
}
void CustomizationManager::setVisible(QString owner, QString id, std::optional<bool> value, int priority) {
    requireGui(); auto &layer = _layers[owner]; layer.priority = priority;
    if (value) layer.visibility[id] = *value; else layer.visibility.erase(id); rebuild();
}
void CustomizationManager::replaceIcon(QString owner, QString id, std::optional<QString> key, int priority) {
    requireGui(); auto &layer = _layers[owner]; layer.priority = priority;
    if (key) layer.icons[id] = *key; else layer.icons.erase(id); rebuild();
}
void CustomizationManager::clearOwner(const QString &owner) {
    requireGui(); _layers.erase(owner); _files.erase(owner); rebuildWatches(); rebuild();
}
void CustomizationManager::registerElement(QString id, QObject *lifetime, VisibilityHook visibility, IconHook icon) {
    requireGui(); Q_ASSERT(lifetime && lifetime->thread() == qApp->thread());
    const auto token = ++_nextToken;
    _elements[id] = {lifetime, std::move(visibility), std::move(icon), token};
    connect(lifetime, &QObject::destroyed, this, [this, id, token] {
        const auto i = _elements.find(id);
        if (i != _elements.end() && i->second.token == token) _elements.erase(i);
    });
    rebuild();
}
void CustomizationManager::unregisterElement(const QString &id) { requireGui(); _elements.erase(id); }
void CustomizationManager::rebuild() {
    // Last layer wins: (priority, owner ID), deterministic; user layer uses priority 100.
    std::vector<std::pair<QString, const Layer *>> layers;
    for (const auto &[owner, layer] : _layers) layers.emplace_back(owner, &layer);
    std::sort(layers.begin(), layers.end(), [](const auto &a, const auto &b) {
        if (a.second->priority != b.second->priority) return a.second->priority < b.second->priority;
        return a.first < b.first;
    });
    auto qss = _baseQss;
    for (const auto &[owner, layer] : layers) qss += QStringLiteral("\n") + layer->qss;
    if (qApp->styleSheet() != qss) qApp->setStyleSheet(qss);
    for (auto &[id, element] : _elements) {
        if (!element.lifetime) continue;
        std::optional<bool> visible;
        std::optional<QString> icon;
        for (const auto &[owner, layer] : layers) {
            if (auto it = layer->visibility.find(id); it != layer->visibility.end()) visible = it->second;
            if (auto it = layer->icons.find(id); it != layer->icons.end()) icon = it->second;
        }
        // Host callbacks must not reenter or mutate the customization registry.
        if (element.visibility) element.visibility(visible);
        if (element.icon) element.icon(icon);
    }
}
} // namespace Ayu::Plugins
