#pragma once
#include <QApplication>
#include <functional>

// Finder and other desktop file managers deliver paths to an existing process.
class DesktopApplication final : public QApplication {
public:
  using QApplication::QApplication;
  std::function<void(const QString &)> openPath;

protected:
  bool event(QEvent *event) override;
};
