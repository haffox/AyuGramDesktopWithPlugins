#pragma once
#include <QObject>
#include <QString>
#include <cstdint>
#include <QPointer>
#include <QFileSystemWatcher>
#include <QTimer>
#include <functional>
#include <map>
#include <optional>

namespace Ayu::Plugins {
class CustomizationManager final : public QObject {
    Q_OBJECT
public:
    // nullopt restores host's current baseline; false hides, true shows.
    using VisibilityHook = std::function<void(std::optional<bool>)>;
    // nullopt restores; empty key hides; nonempty key is a HOST-WHITELISTED icon ID.
    using IconHook = std::function<void(std::optional<QString>)>;
    explicit CustomizationManager(QObject *parent = nullptr);
    void setBaseStyleSheet(QString qss); // call again when host theme changes
    bool setQss(QString owner, QString qss, int priority = 0);
    void watchQss(QString owner, QString absoluteFile, int priority = 0);
    void setVisible(QString owner, QString element, std::optional<bool> value, int priority = 0);
    void replaceIcon(QString owner, QString element, std::optional<QString> key, int priority = 0);
    void clearOwner(const QString &owner);
    void registerElement(QString id, QObject *lifetime, VisibilityHook visibility, IconHook icon = {});
    void unregisterElement(const QString &id);
Q_SIGNALS:
    void error(QString text);
private:
    struct Layer {
        int priority = 0;
        QString qss;
        std::map<QString, bool> visibility;
        std::map<QString, QString> icons;
    };
    struct Element {
        QPointer<QObject> lifetime;
        VisibilityHook visibility;
        IconHook icon;
        std::uint64_t token;
    };
    struct Watch { QString path; int priority; };
    void requireGui() const;
    void rebuild();
    void readFiles();
    void rebuildWatches();
    QString _baseQss;
    std::map<QString, Layer> _layers;
    std::map<QString, Element> _elements;
    std::map<QString, Watch> _files;
    std::uint64_t _nextToken = 0;
    QFileSystemWatcher _watcher;
    QTimer _debounce;
};
} // namespace Ayu::Plugins
