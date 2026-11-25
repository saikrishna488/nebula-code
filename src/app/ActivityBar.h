#pragma once

#include <QColor>
#include <QHash>
#include <QToolBar>

class QAction;

namespace nebula {

class ActivityBar : public QToolBar {
public:
  explicit ActivityBar(QWidget *parent = nullptr);

  void bind(const QString &id, QAction *action);
  void setActiveView(const QString &id);
  void setColors(QColor background, QColor foreground, QColor accent);

private:
  class Button;
  QHash<QString, Button *> buttons_;
};

} // namespace nebula
