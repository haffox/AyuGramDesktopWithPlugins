#pragma once
#include "python_runtime.h"
#include <QString>
namespace Ayu::Plugins {
// Use a separate, account-specific file owned by the application; not by plugins.
Grants loadGrants(const QString &absoluteIniFile);
bool saveGrants(const QString &absoluteIniFile, const Grants &grants);
}
