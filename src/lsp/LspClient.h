#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QProcess;

namespace nebula {

enum class PositionEncoding { Utf8, Utf16, Utf32 };

struct LspPosition {
    int line = 0;
    int character = 0;
};

struct LspRange {
    LspPosition start;
    LspPosition end;
};

struct LspLocation {
    QString path;
    LspRange range;
};

struct LspDiagnostic {
    LspRange range;
    int severity = 0;
    QString message;
};

struct LspCompletion {
    QString label;
    QString text;
    int startOffset = 0;
    int endOffset = 0;
};

bool supportsClangDocument(const QString &path);
LspPosition lspPosition(const QString &text, int utf16Offset, PositionEncoding encoding);
int lspOffset(const QString &text, LspPosition position, PositionEncoding encoding);

class LspClient : public QObject {
    Q_OBJECT

public:
    explicit LspClient(QObject *parent = nullptr);
    ~LspClient() override;

    void setWorkspace(const QString &root);
    void setExecutable(const QString &program, const QStringList &arguments = {});
    void restart();
    PositionEncoding positionEncoding() const { return encoding_; }

    void openDocument(const QString &path, const QString &text);
    void invalidateDocument(const QString &path);
    void changeDocument(const QString &path, const QString &text);
    void closeDocument(const QString &path);
    void requestCompletion(const QString &path, int utf16Offset);
    void requestDefinition(const QString &path, int utf16Offset);
    void requestReferences(const QString &path, int utf16Offset);

signals:
    void completionReady(const QString &path, int utf16Offset, const QList<LspCompletion> &items);
    void diagnosticsReady(const QString &path, const QList<LspDiagnostic> &diagnostics);
    void locationsReady(const QString &title, const QList<LspLocation> &locations);
    void errorOccurred(const QString &message);

private:
    struct Document {
        QString text;
        int version = 0;
        bool unsynced = false;
    };

    struct Request {
        QString method;
        QString path;
        int version = 0;
        int offset = 0;
    };

    void ensureRunning();
    void stop();
    void fail(const QString &message);
    void sendOpen(const QString &path, const Document &document);
    void sendChange(const QString &path, const Document &document, const QString &previous);
    void send(QJsonObject message);
    void notify(const QString &method, const QJsonObject &params);
    void request(const Request &request, const QJsonObject &params);
    void requestPosition(const QString &method, const QString &path, int offset);
    void dispatchPosition(const Request &request);
    void readOutput();
    void receive(const QJsonObject &message);
    void initialized(const QJsonObject &result);
    void handleDiagnostics(const QJsonObject &params);
    void handleResult(const Request &request, const QJsonValue &result);

    QProcess *process_ = nullptr;
    QString root_;
    QString executable_;
    QStringList arguments_;
    QByteArray input_;
    QByteArray errors_;
    QHash<QString, Document> documents_;
    QHash<int, Request> pending_;
    QSet<QString> publishedDiagnostics_;
    QList<Request> queued_;
    int latestCompletionId_ = 0;
    int latestNavigationId_ = 0;
    int nextId_ = 1;
    int nextVersion_ = 1;
    bool ready_ = false;
    bool failed_ = false;
    int syncKind_ = 0;
    PositionEncoding encoding_ = PositionEncoding::Utf16;
};

}
