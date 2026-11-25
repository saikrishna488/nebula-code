#include "Workbench.h"
#include "platform/DesktopApplication.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QStyleFactory>

int main(int argc, char **argv) {
  DesktopApplication app(argc, argv);
  app.setOrganizationName("NebulaCode");
  app.setApplicationName("Nebula Code");
  app.setStyle(QStyleFactory::create("Fusion"));
  QString root, file;
  const auto args = app.arguments();
  if (args.size() > 1) {
    QFileInfo input(args.at(1));
    if (input.isDir())
      root = input.absoluteFilePath();
    else {
      file = input.absoluteFilePath();
      root = input.absolutePath();
    }
  } else {
    for (const auto &v :
         nebula::readJson(nebula::storagePath("recent.json"))["folders"]
             .toArray()) {
      if (QFileInfo(v.toString()).isDir()) {
        root = v.toString();
        break;
      }
    }
  }
  QPointer<nebula::Workbench> lastWindow =
      nebula::Workbench::openWorkspace(root);
  if (!file.isEmpty())
    lastWindow->openFile(file);
  app.openPath = [&lastWindow](const QString &path) {
    const QFileInfo item(path);
    if (item.isDir()) {
      auto *current =
          dynamic_cast<nebula::Workbench *>(QApplication::activeWindow());
      if (!current)
        current = lastWindow.data();
      if (current) {
        current->switchWorkspace(path);
        return;
      }
    }
    lastWindow = nebula::Workbench::openWorkspace(
        item.isDir() ? item.absoluteFilePath() : item.absolutePath());
    if (!item.isDir())
      lastWindow->openFile(path);
  };
  return app.exec();
}
