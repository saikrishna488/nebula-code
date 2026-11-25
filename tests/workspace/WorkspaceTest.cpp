#include "workspace/Workspace.h"
#include "app/Commands.h"
#include "app/Theme.h"
#include <QAction>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>
using namespace nebula;
class WorkspaceTest : public QObject {
  Q_OBJECT
private slots:
  void diskRoundTrip() {
    QTemporaryDir dir;
    QString error;
    const QString path = dir.filePath("main.cpp");
    FileState file{path, {}, {}, true};
    QByteArray first, second;
    QByteArray bytes = QStringLiteral("é\r\nline\n").toUtf8();
    QCOMPARE(saveFile(file, bytes, {}, first, error), SaveResult::Saved);
    QVERIFY(loadFile(path, file, error));
    QCOMPARE(file.bytes, bytes);
    QCOMPARE(file.diskHash, first);
    QVERIFY(file.bom);
    QCOMPARE(saveFile(file, "new", first, second, error), SaveResult::Saved);
    QCOMPARE(saveFile(file, "stale", first, second, error),
             SaveResult::ChangedOnDisk);
    QVERIFY(QFile::remove(path));
    QCOMPARE(saveFile(file, "stale", first, second, error),
             SaveResult::ChangedOnDisk);
    file.path = dir.filePath("missing/main.cpp");
    QCOMPARE(saveFile(file, bytes, {}, second, error), SaveResult::Failed);
  }
  void invalidInput() {
    QTemporaryDir dir;
    QFile f(dir.filePath("input"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("\xff");
    f.close();
    FileState state;
    QString error;
    QVERIFY(!loadFile(f.fileName(), state, error));
    QVERIFY(!error.isEmpty());
  }
  void settingsAndCommands() {
    QTemporaryDir dir;
    QString err, path = dir.filePath("sub/file.json");
    QVERIFY(writeJson(path, {{"split", true}}, err));
    QVERIFY(readJson(path)["split"].toBool());
    QWidget window;
    bool enabled = false;
    int executions = 0;
    Commands registry(&window, {{"edit.test", "Ctrl+Y"}});
    auto *action = registry.add(
        "edit.test", "Test", QKeySequence("Ctrl+T"), [&] { ++executions; },
        [&] { return enabled; });
    QCOMPARE(action->shortcut(), QKeySequence("Ctrl+Y"));
    registry.refresh();
    QVERIFY(!action->isEnabled());
    enabled = true;
    registry.refresh();
    action->trigger();
    QCOMPARE(executions, 1);
    QVERIFY(loadTheme("dark")["dark"].toBool());
    QVERIFY(!loadTheme("light")["dark"].toBool());
  }
};
QTEST_MAIN(WorkspaceTest)
#include "WorkspaceTest.moc"
