#include "git/GitRepository.h"
#include "lsp/LspClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace nebula;

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

QByteArray runProcess(const QString &program, const QString &directory, const QStringList &arguments,
                      int *exitCode)
{
    QProcess process;
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted() || !process.waitForFinished()) {
        *exitCode = -1;
        return process.errorString().toUtf8();
    }
    *exitCode = process.exitCode();
    return process.readAll();
}

}

class ServiceTests : public QObject {
    Q_OBJECT

private slots:
    void positionMapping();
    void fakeLanguageServer();
    void clangdFixture();
    void gitRepository();
};

void ServiceTests::positionMapping()
{
    const QString text = QString::fromUtf8("aé🙂\r\nZ字\rB\n");
    QCOMPARE(lspPosition(text, 4, PositionEncoding::Utf8).character, 7);
    QCOMPARE(lspPosition(text, 4, PositionEncoding::Utf16).character, 4);
    QCOMPARE(lspPosition(text, 4, PositionEncoding::Utf32).character, 3);
    QCOMPARE(lspPosition(text, 5, PositionEncoding::Utf8).line, 0);
    QCOMPARE(lspPosition(text, 6, PositionEncoding::Utf8).line, 1);
    QCOMPARE(lspPosition(text, 9, PositionEncoding::Utf8).line, 2);
    QCOMPARE(lspOffset(text, {0, 7}, PositionEncoding::Utf8), 4);
    QCOMPARE(lspOffset(text, {0, 4}, PositionEncoding::Utf8), 2);
    QCOMPARE(lspOffset(text, {1, 2}, PositionEncoding::Utf16), 8);
    QCOMPARE(lspOffset(text, {1, 99}, PositionEncoding::Utf32), 8);
    QCOMPARE(lspOffset(text, {99, 0}, PositionEncoding::Utf16), text.size());
}

void ServiceTests::fakeLanguageServer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("file #é.cpp");
    LspClient client;
    client.setWorkspace(directory.path());
    client.setExecutable(QStringLiteral(NEBULA_LSP_FIXTURE));

    QStringList errors;
    QStringList diagnosticMessages;
    QList<LspCompletion> completions;
    QList<LspLocation> locations;
    int locationNotifications = 0;
    connect(&client, &LspClient::errorOccurred, this, [&](const QString &error) { errors.append(error); });
    connect(&client, &LspClient::diagnosticsReady, this, [&](const QString &file, const auto &items) {
        QCOMPARE(file, path);
        for (const auto &item : items)
            diagnosticMessages.append(item.message);
    });
    connect(&client, &LspClient::completionReady, this, [&](const QString &file, int, const auto &items) {
        QCOMPARE(file, path);
        completions = items;
    });
    connect(&client, &LspClient::locationsReady, this, [&](const QString &, const auto &items) {
        locations = items;
        ++locationNotifications;
    });

    const QString text = QString::fromUtf8("aé🙂");
    client.openDocument(path, text);
    client.requestCompletion(path, text.size());
    QTRY_COMPARE(completions.size(), 1);
    QCOMPARE(completions.first().label, "7");
    QCOMPARE(client.positionEncoding(), PositionEncoding::Utf8);
    QTRY_COMPARE(diagnosticMessages, QStringList{"Current"});

    client.requestDefinition(path, 0);
    client.invalidateDocument(path);
    QTest::qWait(150);
    QCOMPARE(locationNotifications, 0);
    client.changeDocument(path, QString::fromUtf8("aé🙃Z"));
    QTRY_COMPARE(diagnosticMessages, (QStringList{"Current", "Current"}));
    QVERIFY(locations.isEmpty());
    client.requestDefinition(path, 0);
    client.requestDefinition(path, 1);
    QTRY_COMPARE(locationNotifications, 1);
    QCOMPARE(locations.size(), 1);
    QCOMPARE(locations.first().path, path);
    QCOMPARE(locations.first().range.start.character, 1);

    completions.clear();
    client.restart();
    client.requestCompletion(path, text.size() + 1);
    QTRY_COMPARE(completions.size(), 1);
    QCOMPARE(completions.first().label, "8");
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
}

void ServiceTests::clangdFixture()
{
    const QString clangd = QStandardPaths::findExecutable("clangd");
    const QString cmake = QStandardPaths::findExecutable("cmake");
    if (clangd.isEmpty() || cmake.isEmpty())
        QSKIP("clangd and CMake are required for the compiled-language fixture.");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString fixture = QStringLiteral(NEBULA_CLANGD_FIXTURE);
    for (const QString &file : {"CMakeLists.txt", "math.h", "math.cpp", "caller.cpp"})
        QVERIFY(QFile::copy(QDir(fixture).filePath(file), QDir(root).filePath(file)));

    int code = 0;
    const auto output = runProcess(cmake, root, {"-S", ".", "-B", "build", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"}, &code);
    QVERIFY2(code == 0, output.constData());
    QVERIFY(QFileInfo::exists(QDir(root).filePath("build/compile_commands.json")));

    LspClient client;
    client.setWorkspace(root);
    client.setExecutable(clangd, {"--compile-commands-dir=" + QDir(root).filePath("build")});
    QStringList errors;
    QList<LspDiagnostic> diagnostics;
    QList<LspCompletion> completions;
    QList<LspLocation> locations;
    QString locationTitle;
    const QString callerPath = QDir(root).filePath("caller.cpp");
    const QString implementationPath = QDir(root).filePath("math.cpp");
    const auto caller = readFile(callerPath);
    const auto implementation = readFile(implementationPath);
    QVERIFY(!caller.isEmpty());
    QVERIFY(!implementation.isEmpty());
    connect(&client, &LspClient::errorOccurred, this, [&](const QString &error) { errors.append(error); });
    connect(&client, &LspClient::diagnosticsReady, this, [&](const QString &file, const auto &items) {
        if (file == callerPath)
            diagnostics = items;
    });
    connect(&client, &LspClient::completionReady, this, [&](const QString &, int, const auto &items) {
        completions = items;
    });
    connect(&client, &LspClient::locationsReady, this, [&](const QString &title, const auto &items) {
        locationTitle = title;
        locations = items;
    });
    client.openDocument(implementationPath, implementation);
    client.openDocument(callerPath, caller);

    const int symbol = caller.indexOf("nebula_add");
    QVERIFY(symbol >= 0);
    client.requestDefinition(callerPath, symbol + 2);
    QTRY_VERIFY_WITH_TIMEOUT(!locationTitle.isEmpty() || !errors.isEmpty(), 10000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
    QVERIFY(std::any_of(locations.cbegin(), locations.cend(), [&](const LspLocation &location) {
        return location.path == implementationPath || location.path == QDir(root).filePath("math.h");
    }));

    locationTitle.clear();
    locations.clear();
    client.requestReferences(implementationPath, implementation.indexOf("nebula_add") + 2);
    QTRY_VERIFY_WITH_TIMEOUT(!locationTitle.isEmpty(), 10000);
    QVERIFY(std::any_of(locations.cbegin(), locations.cend(), [&](const LspLocation &location) {
        return location.path == callerPath;
    }));

    const QString incomplete = QString(caller).replace("nebula_add(1, 2)", "nebula_a");
    client.changeDocument(callerPath, incomplete);
    client.requestCompletion(callerPath, symbol + 8);
    QTRY_VERIFY_WITH_TIMEOUT(!completions.isEmpty(), 10000);
    QVERIFY(std::any_of(completions.cbegin(), completions.cend(), [](const LspCompletion &completion) {
        return completion.label.contains("nebula_add") && completion.text.contains("nebula_add");
    }));
    QTRY_VERIFY_WITH_TIMEOUT(!diagnostics.isEmpty(), 10000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
}

void ServiceTests::gitRepository()
{
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is required for the local repository fixture.");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto gitCommand = [&](const QStringList &arguments) {
        int code = 0;
        const auto output = runProcess(git, directory.path(), arguments, &code);
        if (code != 0)
            qWarning().noquote() << output;
        return code;
    };
    QCOMPARE(gitCommand({"init", "-q"}), 0);
    QCOMPARE(gitCommand({"config", "user.email", "nebula@example.invalid"}), 0);
    QCOMPARE(gitCommand({"config", "user.name", "Nebula tests"}), 0);
    QCOMPARE(gitCommand({"config", "commit.gpgsign", "false"}), 0);
    QCOMPARE(gitCommand({"config", "core.hooksPath", ".git/hooks"}), 0);
    QVERIFY(writeFile(directory.filePath("before.txt"), "initial\n"));
    QVERIFY(writeFile(directory.filePath("modified.txt"), "tracked\n"));
    QCOMPARE(gitCommand({"add", "--", "before.txt", "modified.txt"}), 0);
    QCOMPARE(gitCommand({"commit", "-qm", "Initial commit"}), 0);
    QCOMPARE(gitCommand({"mv", "before.txt", "after space.txt"}), 0);
    QVERIFY(writeFile(directory.filePath("modified.txt"), "changed\n"));
    QVERIFY(writeFile(directory.filePath("-new[1]é.txt"), "untracked content\n"));

    GitRepository repository;
    QList<GitEntry> entries;
    QString diff;
    QStringList errors;
    bool committed = false;
    connect(&repository, &GitRepository::statusReady, this, [&](const auto &status) { entries = status; });
    connect(&repository, &GitRepository::diffReady, this, [&](const QString &, const QString &changed) { diff = changed; });
    connect(&repository, &GitRepository::errorOccurred, this, [&](const QString &error) { errors.append(error); });
    connect(&repository, &GitRepository::commitSucceeded, this, [&] { committed = true; });
    const auto entry = [&](const QString &path) {
        for (const auto &entry : entries)
            if (entry.path == path)
                return entry;
        return GitEntry{};
    };

    repository.setWorkspace(directory.path());
    QTRY_VERIFY(!repository.busy());
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
    QCOMPARE(entries.size(), 3);
    const auto renamed = entry("after space.txt");
    QCOMPARE(renamed.originalPath, "before.txt");
    QVERIFY(renamed.staged());
    QVERIFY(entry("-new[1]é.txt").untracked());

    repository.showDiff(renamed, true);
    repository.showDiff(entry("modified.txt"), false);
    QTRY_VERIFY(!repository.busy());
    QVERIFY(diff.contains("+changed"));
    repository.showDiff(renamed, true);
    QTRY_VERIFY(!repository.busy());
    QVERIFY(diff.contains("rename from before.txt"));
    repository.showDiff(entry("-new[1]é.txt"), false);
    QTRY_VERIFY(!repository.busy());
    QVERIFY(diff.contains("+untracked content"));

    QVERIFY(writeFile(directory.filePath("after space.txt"), "initial\nupdated\n"));
    repository.refresh();
    QTRY_VERIFY(!repository.busy());
    const auto modifiedRename = entry("after space.txt");
    QVERIFY(modifiedRename.staged() && modifiedRename.unstaged());
    repository.stage(modifiedRename);
    QTRY_VERIFY(!repository.busy());
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
    QVERIFY(!entry("after space.txt").unstaged());

    repository.unstage(renamed);
    QTRY_VERIFY(!repository.busy());
    QVERIFY(entry("after space.txt").untracked());
    QVERIFY(entry("before.txt").unstaged());
    repository.stage(entry("before.txt"));
    repository.stage(entry("after space.txt"));
    repository.stage(entry("modified.txt"));
    repository.stage(entry("-new[1]é.txt"));
    QTRY_VERIFY(!repository.busy());
    QVERIFY(entry("after space.txt").staged());
    QVERIFY(entry("modified.txt").staged());
    QVERIFY(entry("-new[1]é.txt").staged());
    repository.commit("Commit from Nebula");
    QTRY_VERIFY(!repository.busy());
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
    QVERIFY(committed);
    QVERIFY(entries.isEmpty());
}

QTEST_GUILESS_MAIN(ServiceTests)
#include "service_tests.moc"
