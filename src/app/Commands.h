#pragma once
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <functional>
class QAction;
class QWidget;
namespace nebula {
class Commands {
public:
  explicit Commands(QWidget *window, QJsonObject bindings);
  QAction *add(const QString &id, const QString &title, QKeySequence shortcut,
               std::function<void()> run, std::function<bool()> enabled = {});
  void refresh();
  void palette();

private:
  QWidget *window_;
  QJsonObject bindings_;
  struct Entry {
    QAction *action;
    std::function<bool()> enabled;
  };
  QHash<QString, Entry> entries_;
};
} // namespace nebula
