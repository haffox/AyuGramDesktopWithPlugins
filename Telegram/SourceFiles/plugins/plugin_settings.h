#pragma once
#include <QDialog>
class QListWidget;
class QPlainTextEdit;
namespace Ayu::Plugins {
class PluginManager;
class CustomizationManager;
// Working QWidget dialog; host may render the same model using native TDesktop settings widgets.
class PluginSettings final : public QDialog {
    Q_OBJECT
public:
    PluginSettings(PluginManager &manager, CustomizationManager &customization, QWidget *parent = nullptr);
private:
    void refresh();
    PluginManager &_manager;
    QListWidget *_list;
    QPlainTextEdit *_console;
};
} // namespace Ayu::Plugins
