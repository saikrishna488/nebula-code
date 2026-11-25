#pragma once

#include "Commands.h"
#include "workspace/Workspace.h"
#include <QMainWindow>
#include <QPointer>
#include <QSplitter>

class QFileSystemModel;
class QTreeView;
class QDockWidget;
class QTabWidget;
class QTimer;
namespace nebula {
class EditorView;
class Sidebar;
class ActivityBar;
class LspClient;
class LspPanel;
class TerminalSession;
class DocumentTab : public QSplitter {
public:
  FileState file;
  EditorView *primary = nullptr;
  EditorView *secondary = nullptr;
  EditorView *active = nullptr;
  QTimer *sync = nullptr;
  bool languageService = false;
};

class Workbench final : public QMainWindow {
public:
  explicit Workbench(const QString &root);
  ~Workbench() override;
  static Workbench *openWorkspace(const QString &path);
  bool switchWorkspace(const QString &path);
  bool openFile(const QString &path);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

private:
  DocumentTab *current() const;
  DocumentTab *createTab(FileState file);
  void bindView(DocumentTab *tab, EditorView *editor);
  void toggleSplit(DocumentTab *tab);
  void updateTitle(DocumentTab *tab);
  bool save(DocumentTab *tab);
  bool canClose(DocumentTab *tab);
  void closeTab(int index);
  bool canCloseTabs();
  void restoreSession(bool restoreWindowGeometry);
  void rememberSession();
  void setupCommands();
  void applyTheme();
  void showSettings();
  void updateAppearance(const QString &theme, bool minimap,
                        const QJsonObject &overrides);
  QJsonObject currentTheme() const;
  void showFind();
  void toggleTerminal();
  void updateLanguage(DocumentTab *tab);
  enum class LanguageAction { Completion, Definition, References };
  void requestLanguage(LanguageAction operation);
  void report(const QString &message);
  QString root_;
  QString theme_;
  QJsonObject settings_;
  Commands commands_;
  QTabWidget *tabs_;
  QDockWidget *terminalDock_;
  Sidebar *sidebar_;
  ActivityBar *activity_;
  QDockWidget *languageDock_;
  QDockWidget *findDock_;
  TerminalSession *terminal_ = nullptr;

  LspClient *lsp_;
  LspPanel *language_;
  bool minimap_ = true;
};
} // namespace nebula
