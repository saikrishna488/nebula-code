#include "app/Workbench.h"
#include "editor/EditorView.h"

#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <array>
#include <cstdio>
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

constexpr int MiB = 1024 * 1024;
constexpr int TypingSamples = 50;

class BenchApplication : public QApplication {
public:
    using QApplication::QApplication;

    QElapsedTimer clock;
    int edits = 0;
    bool stayedActive = false;

    void armPaint(QWidget *viewport, int requiredEdits = 0)
    {
        viewport_ = viewport;
        requiredEdits_ = requiredEdits;
        paintNs_ = -1;
    }

    bool notify(QObject *receiver, QEvent *event) override
    {
        const bool painting = paintNs_ < 0 && receiver == viewport_
            && edits >= requiredEdits_ && event->type() == QEvent::Paint;
        const bool handled = QApplication::notify(receiver, event);
        if (painting && paintNs_ < 0) {
            paintNs_ = clock.nsecsElapsed();
            if (paintWaiter_)
                paintWaiter_->quit();
        }
        return handled;
    }

    qint64 waitForPaint()
    {
        if (paintNs_ >= 0)
            return paintNs_;
        QEventLoop loop;
        paintWaiter_ = &loop;
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
        paintWaiter_ = nullptr;
        if (paintNs_ < 0)
            qFatal("The active editor viewport did not repaint");
        return paintNs_;
    }

private:
    QWidget *viewport_ = nullptr;
    QEventLoop *paintWaiter_ = nullptr;
    int requiredEdits_ = 0;
    qint64 paintNs_ = -1;
};

struct ProcessSample {
    QString name;
    double rssMiB;
    quint64 cpuNs;
    quint64 started;
};

using Processes = QHash<pid_t, ProcessSample>;

bool sampleProcess(pid_t pid, ProcessSample &sample)
{
    rusage_info_v0 info{};
    if (proc_pid_rusage(pid, RUSAGE_INFO_V0, reinterpret_cast<rusage_info_t *>(&info)) != 0)
        return false;
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    proc_pidpath(pid, path.data(), path.size());
    sample = {QFileInfo(QString::fromUtf8(path.data())).fileName(),
              double(info.ri_resident_size) / MiB, info.ri_user_time + info.ri_system_time,
              info.ri_proc_start_abstime};
    return true;
}

ProcessSample sampleEditor()
{
    ProcessSample editor;
    if (!sampleProcess(getpid(), editor))
        qFatal("Cannot sample the editor process");
    return editor;
}

void sampleChildren(pid_t parent, Processes &children)
{
    std::array<pid_t, 128> pids{};
    const int count = proc_listchildpids(parent, pids.data(), sizeof(pids));
    if (count < 0 || count >= int(pids.size()))
        qFatal("Cannot enumerate the benchmark's child processes");
    for (int i = 0; i < count; ++i) {
        const auto pid = pids.at(i);
        ProcessSample sample;
        if (pid > 0 && sampleProcess(pid, sample)) {
            children.insert(pid, sample);
            sampleChildren(pid, children);
        }
    }
}

QJsonArray childResults(const Processes &start, const Processes &end, qint64 elapsedNs)
{
    QJsonArray children;
    for (auto it = start.cbegin(); it != start.cend(); ++it) {
        QJsonObject sample{{"name", it->name}, {"rss_mib", it->rssMiB}};
        const auto after = end.constFind(it.key());
        if (after != end.cend() && it->started == after->started) {
            sample.insert("rss_end_mib", after->rssMiB);
            sample.insert("cpu_percent", (after->cpuNs - it->cpuNs) * 100.0 / elapsedNs);
        }
        children.append(sample);
    }
    return children;
}

QString fixture(const QTemporaryDir &dir, const QString &name, int size)
{
    QFile file(dir.filePath(name));
    if (!file.open(QIODevice::WriteOnly))
        qFatal("Cannot open fixture for writing");
    QByteArray text = "// Nebula performance fixture\n";
    text.reserve(size);
    for (int index = 0; ; ++index) {
        const QByteArray function = "static int nebula_value_" + QByteArray::number(index)
            + "(int x) { return (x & 255) * 3 + 1; }\n";
        if (text.size() + function.size() > size)
            break;
        text += function;
    }
    text += QByteArray(size - text.size(), ' ');
    if (file.write(text) != size)
        qFatal("Cannot write complete fixture");
    return file.fileName();
}

nebula::DocumentTab *currentTab(nebula::Workbench &window)
{
    return static_cast<nebula::DocumentTab *>(
        qobject_cast<QTabWidget *>(window.centralWidget())->currentWidget());
}

QAbstractScrollArea *editorArea(nebula::Workbench &window)
{
    return currentTab(window)->active->findChild<QAbstractScrollArea *>();
}

void verifyTerminalPrompt(const QDockWidget &terminal, const QTemporaryDir &dir)
{
    auto *focus = QApplication::focusWidget();
    if (!focus || !terminal.isAncestorOf(focus))
        qFatal("The terminal did not have keyboard focus");
    const auto marker = dir.filePath(".nebula-bench-ready");
    QTest::keyClicks(focus, "echo ready > .nebula-bench-ready");
    QTest::keyClick(focus, Qt::Key_Return);
    QElapsedTimer timeout;
    timeout.start();
    while (!QFileInfo::exists(marker) && timeout.elapsed() < 3000)
        QTest::qWait(20);
    QFile file(marker);
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != "ready\n")
        qFatal("The terminal shell did not run a command in the workspace");
}

QJsonObject idleResults(BenchApplication &app, nebula::Workbench &window,
                        const QTemporaryDir &dir, int seconds, bool verifyPrompt)
{
    for (int i = 0; i < 10; ++i) {
        if (!window.openFile(fixture(dir, QString("source%1.cpp").arg(i), 4096)))
            qFatal("Cannot open source tab");
    }
    const auto *tabs = qobject_cast<QTabWidget *>(window.centralWidget());
    const auto *terminal = window.findChild<QDockWidget *>("Terminal");
    if (tabs->count() != 10 || !terminal)
        qFatal("Expected exactly ten source tabs and a terminal");
    window.findChild<QAction *>("view.terminal")->trigger();
    QTest::qWait(3000);
    if (!terminal->isVisible())
        qFatal("The terminal session did not stay open");
    if (verifyPrompt) {
        verifyTerminalPrompt(*terminal, dir);
        QTest::qWait(250);
    }

    app.stayedActive = app.platformName() == "cocoa" && window.isActiveWindow();
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window,
                     [&](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
            app.stayedActive = false;
    });
    Processes beforeChildren;
    sampleChildren(getpid(), beforeChildren);
    const auto start = sampleEditor();
    const auto startNs = app.clock.nsecsElapsed();

    // Let the ordinary GUI event loop sleep between events throughout this sample.
    QEventLoop loop;
    QTimer::singleShot(seconds * 1000, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();

    const auto elapsed = app.clock.nsecsElapsed() - startNs;
    const auto after = sampleEditor();
    Processes afterChildren;
    sampleChildren(getpid(), afterChildren);
    return {{"loaded_rss_mib", start.rssMiB},
            {"loaded_rss_end_mib", after.rssMiB},
            {"idle_elapsed_seconds", elapsed / 1e9},
            {"idle_cpu_percent", (after.cpuNs - start.cpuNs) * 100.0 / elapsed},
            {"stayed_foreground", app.stayedActive && window.isActiveWindow()},
            {"terminal_open_at_end", terminal->isVisible()},
            {"terminal_prompt_verified", verifyPrompt},
            {"children", childResults(beforeChildren, afterChildren, elapsed)}};
}

QJsonObject fileResults(BenchApplication &app, nebula::Workbench &window,
                        const QTemporaryDir &dir, const QString &mode)
{
    const bool large = mode == "--large";
    const int size = (large ? 16 : 1) * MiB;
    const auto path = fixture(dir, "fixture.cpp", size);
    const auto start = app.clock.nsecsElapsed();
    if (!window.openFile(path))
        qFatal("Cannot open C++ fixture");
    auto *area = editorArea(window);
    app.armPaint(area->viewport());
    const auto painted = app.waitForPaint();
    auto *editor = currentTab(window)->active;
    if (editor->textUtf8().size() != size)
        qFatal("The current editor does not contain the fixture");

    QJsonObject result{{"open_ms", (painted - start) / 1e6}};
    if (mode == "--open")
        return result;

    editor->setCursorPosition(3);
    QObject::connect(editor, &nebula::EditorView::textChanged, &window, [&] { ++app.edits; });
    QTest::qWait(300);
    QJsonArray latency;
    const int samples = large ? 10 : TypingSamples;
    for (int index = 0; index < samples; ++index) {
        app.armPaint(area->viewport(), app.edits + 1);
        const auto keyStart = app.clock.nsecsElapsed();
        QTest::keyClick(area, Qt::Key_X, Qt::NoModifier, 0);
        latency.append((app.waitForPaint() - keyStart) / 1e6);
        QTest::qWait(40);
    }
    if (!editor->textUtf8().startsWith("// " + QByteArray(samples, 'x')))
        qFatal("The active editor did not receive each simulated key");
    result.insert("typing_ms", latency);
    return result;
}

}

int main(int argc, char **argv)
{
    BenchApplication app(argc, argv);
    app.clock.start();
    app.setStyle("Fusion");
    app.setOrganizationName("NebulaBenchmark");
    app.setApplicationName("NebulaBenchmark");
    const auto mode = app.arguments().value(1);
    if (mode != "--launch" && mode != "--empty" && mode != "--open" && mode != "--typing"
        && mode != "--large" && mode != "--idle")
        qFatal("Specify --launch, --empty, --open, --typing, --large, or --idle");

    QTemporaryDir dir;
    if (!dir.isValid())
        qFatal("Cannot create temporary workspace");
    nebula::Workbench window(mode == "--launch" ? QString{} : dir.path());
    app.armPaint(editorArea(window)->viewport());
    window.show();
    if (app.platformName() == "cocoa") {
        window.raise();
        window.activateWindow();
    }
    app.waitForPaint();

    if (mode == "--launch") {
        std::puts("ready");
        std::fflush(stdout);
        return 0;
    }

    QJsonObject result;
    if (mode == "--empty") {
        QTest::qWait(3000);
        result.insert("empty_rss_mib", sampleEditor().rssMiB);
    } else if (mode == "--idle") {
        bool valid = false;
        const int seconds = app.arguments().value(2, "60").toInt(&valid);
        if (!valid || seconds <= 0 || seconds > 600)
            qFatal("--idle expects a sample duration between 1 and 600 seconds");
        result = idleResults(app, window, dir, seconds, app.arguments().contains("--verify-prompt"));
    } else {
        result = fileResults(app, window, dir, mode);
    }
    result.insert("platform", app.platformName());
    result.insert("scale", window.devicePixelRatioF());
    result.insert("active_window", window.isActiveWindow());
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return 0;
}
