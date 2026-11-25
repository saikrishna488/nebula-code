#pragma once

#include <QDockWidget>
#include <functional>

class QLabel;
class QStackedWidget;
class QColor;
class QTreeView;

namespace nebula {
class GitPanel;

class Sidebar final : public QDockWidget {
public:
  Sidebar(const QString &root, std::function<void(const QString &)> openFile,
          std::function<void()> openFolder, QWidget *parent);
  void setWorkspace(const QString &root);
  void togglePage(bool sourceControl);
  bool showsSourceControl() const;
  void setColors(const QColor &panel, const QColor &foreground,
                 const QColor &accent);

private:
  QString root_;
  QLabel *heading_;
  QStackedWidget *pages_;
  QWidget *emptyExplorer_;
  QLabel *folderName_;
  QTreeView *tree_;
  GitPanel *git_;
};
} // namespace nebula
