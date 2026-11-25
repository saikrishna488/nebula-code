#include "Commands.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QVBoxLayout>
namespace nebula {
Commands::Commands(QWidget *window, QJsonObject bindings)
    : window_(window), bindings_(bindings) {}
QAction *Commands::add(const QString &id, const QString &title,
                       QKeySequence shortcut, std::function<void()> run,
                       std::function<bool()> enabled) {
  auto *action = new QAction(title, window_);
  action->setObjectName(id);
  if (bindings_.value(id).isString()) {
    auto value = bindings_.value(id).toString();
    auto custom = QKeySequence::fromString(value, QKeySequence::PortableText);
    if (value.isEmpty() || (!custom.isEmpty() && !custom.toString().isEmpty()))
      shortcut = custom;
  }
  action->setShortcut(shortcut);
  action->setShortcutContext(Qt::WindowShortcut);
  window_->addAction(action);
  QObject::connect(action, &QAction::triggered, window_, [run, enabled] {
    if (!enabled || enabled())
      run();
  });
  entries_.insert(id, {action, enabled});
  return action;
}
void Commands::refresh() {
  for (const auto &entry : std::as_const(entries_))
    entry.action->setEnabled(!entry.enabled || entry.enabled());
}
void Commands::palette() {
  refresh();
  QPointer<QWidget> previousFocus = QApplication::focusWidget();
  QList<QAction *> available;
  for (const auto &entry : std::as_const(entries_))
    if (entry.action->isEnabled())
      available.append(entry.action);
  QDialog box(window_);
  box.setWindowTitle(QObject::tr("Command Palette"));
  box.resize(450, 350);
  QVBoxLayout layout(&box);
  QLineEdit query;
  query.setPlaceholderText(QObject::tr("Find a command"));
  QListWidget list;
  layout.addWidget(&query);
  layout.addWidget(&list);
  const auto populate = [&] {
    list.clear();
    for (auto *a : std::as_const(available)) {
      if (!a->text().contains(query.text(), Qt::CaseInsensitive))
        continue;
      auto *item = new QListWidgetItem(
          a->text() + "   " + a->shortcut().toString(QKeySequence::NativeText),
          &list);
      item->setData(Qt::UserRole, a->objectName());
    }
    list.sortItems();
    list.setCurrentRow(0);
  };
  QObject::connect(&query, &QLineEdit::textChanged, &box, populate);
  QObject::connect(&query, &QLineEdit::returnPressed, &box, &QDialog::accept);
  QObject::connect(&list, &QListWidget::itemActivated, &box, &QDialog::accept);
  populate();
  if (box.exec() && list.currentItem()) {
    auto *a = entries_.value(list.currentItem()->data(Qt::UserRole).toString())
                  .action;
    box.hide();
    if (previousFocus)
      previousFocus->setFocus();
    refresh();
    a->trigger();
  }
}
} // namespace nebula
