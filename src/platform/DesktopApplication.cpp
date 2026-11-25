#include "DesktopApplication.h"
#include <QFileOpenEvent>

bool DesktopApplication::event(QEvent *event) {
  if (event->type() == QEvent::FileOpen && openPath) {
    const auto *request = static_cast<QFileOpenEvent *>(event);
    if (!request->file().isEmpty()) {
      openPath(request->file());
      return true;
    }
  }
  return QApplication::event(event);
}
