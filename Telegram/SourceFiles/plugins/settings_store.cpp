#include "settings_store.h"
#include <QSettings>
#include <QStringList>
namespace Ayu::Plugins {
Grants loadGrants(const QString &file) {
    QSettings settings(file, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("grants"));
    Grants grants;
    for (const auto &id : settings.childKeys()) {
        auto &values = grants[id.toStdString()];
        for (const auto &v : settings.value(id).toStringList()) values.insert(v.toStdString());
    }
    return grants;
}
bool saveGrants(const QString &file, const Grants &grants) {
    QSettings settings(file, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("grants")); settings.remove(QString());
    for (const auto &[id, values] : grants) {
        QStringList list;
        for (const auto &value : values) list.push_back(QString::fromStdString(value));
        settings.setValue(QString::fromStdString(id), list);
    }
    settings.endGroup(); settings.sync(); return settings.status() == QSettings::NoError;
}
}
