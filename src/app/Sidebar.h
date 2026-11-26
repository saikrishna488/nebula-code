#pragma once

#include <QDockWidget>
#include <functional>

class QLabel;
class QStackedWidget;
class QColor;
class QToolButton;
class QTreeView;

namespace nebula {
class GitPanel;

class Sidebar final : public QDockWidget {
  Q_OBJECT

public:
  Sidebar(const QString &root, std::function<void(const QString &)> openFile,
          std::function<void()> openFolder, QWidget *parent);
  void setWorkspace(const QString &root);
  void togglePage(bool sourceControl);
  bool showsSourceControl() const;
  void setColors(const QColor &panel, const QColor &foreground,
                 const QColor &accent);

signals:
  void pathRenamed(const QString &oldPath, const QString &newPath);
  void pathDeleted(const QString &path);

private:
  QString selectedDirectory() const;
  void createEntry(const QString &parentDir, bool directory);
  void renameEntry(const QString &path);
  void deleteEntry(const QString &path);
  void showContextMenu(const QPoint &pos);
  void updateActionIcons(const QColor &foreground);

  QString root_;
  std::function<void(const QString &)> openFile_;
  QLabel *heading_;
  QStackedWidget *pages_;
  QWidget *emptyExplorer_;
  QWidget *folderHeader_;
  QLabel *folderName_;
  QToolButton *newFileButton_;
  QToolButton *newFolderButton_;
  QTreeView *tree_;
  GitPanel *git_;
};
} // namespace nebula
