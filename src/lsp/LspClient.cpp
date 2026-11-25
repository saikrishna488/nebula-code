#include "LspClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace nebula {

namespace {

const auto CompletionMethod = QStringLiteral("textDocument/completion");
const auto DefinitionMethod = QStringLiteral("textDocument/definition");
const auto ReferencesMethod = QStringLiteral("textDocument/references");

QString fileUri(const QString &path)
{
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

QString localPath(const QString &uri)
{
    const auto url = QUrl::fromEncoded(uri.toUtf8());
    return url.isLocalFile() ? url.toLocalFile() : QString{};
}

QString languageId(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "c")
        return "c";
    static const QSet<QString> cpp = {"h", "hh", "hpp", "hxx", "cc", "cpp", "cxx", "ipp", "tpp",
                                      "inl", "ixx", "cppm", "c++", "h++"};
    return cpp.contains(suffix) ? QStringLiteral("cpp") : QString{};
}

bool newline(QChar character)
{
    return character == u'\r' || character == u'\n';
}

int nextLine(const QString &text, int offset)
{
    return text.at(offset) == u'\r' && offset + 1 < text.size() && text.at(offset + 1) == u'\n'
        ? offset + 2 : offset + 1;
}

int codePointLength(const QString &text, int offset)
{
    return text.at(offset).isHighSurrogate() && offset + 1 < text.size()
        && text.at(offset + 1).isLowSurrogate() ? 2 : 1;
}

bool positionBoundary(const QString &text, int offset)
{
    if (offset == 0 || offset == text.size())
        return true;
    const auto before = text.at(offset - 1);
    const auto after = text.at(offset);
    return !(before == u'\r' && after == u'\n')
        && !(before.isHighSurrogate() && after.isLowSurrogate());
}

int positionUnits(QChar character, int length, PositionEncoding encoding)
{
    if (encoding == PositionEncoding::Utf16)
        return length;
    if (encoding == PositionEncoding::Utf32)
        return 1;
    if (length == 2)
        return 4;
    if (character.unicode() < 0x80)
        return 1;
    return character.unicode() < 0x800 ? 2 : 3;
}

QJsonObject positionJson(LspPosition position)
{
    return {{"line", position.line}, {"character", position.character}};
}

LspPosition decodePosition(const QJsonObject &position)
{
    return {position.value("line").toInt(), position.value("character").toInt()};
}

LspRange decodeRange(const QJsonObject &range)
{
    return {decodePosition(range.value("start").toObject()),
            decodePosition(range.value("end").toObject())};
}

bool identifierCharacter(QChar character)
{
    return character.isLetterOrNumber() || character.isMark() || character == u'_';
}

}

bool supportsClangDocument(const QString &path)
{
    return !languageId(path).isEmpty();
}

LspPosition lspPosition(const QString &text, int utf16Offset, PositionEncoding encoding)
{
    const int limit = std::clamp(utf16Offset, 0, int(text.size()));
    LspPosition position;
    for (int offset = 0; offset < limit;) {
        const auto character = text.at(offset);
        if (newline(character)) {
            offset = nextLine(text, offset);
            if (offset <= limit) {
                ++position.line;
                position.character = 0;
            }
            continue;
        }
        const int length = codePointLength(text, offset);
        offset += length;
        if (offset <= limit)
            position.character += positionUnits(character, length, encoding);
    }
    return position;
}

int lspOffset(const QString &text, LspPosition position, PositionEncoding encoding)
{
    int offset = 0;
    for (int line = 0; line < position.line && offset < text.size(); ++line) {
        while (offset < text.size() && !newline(text.at(offset)))
            ++offset;
        if (offset < text.size())
            offset = nextLine(text, offset);
    }
    int units = 0;
    while (units < position.character && offset < text.size() && !newline(text.at(offset))) {
        const int length = codePointLength(text, offset);
        units += positionUnits(text.at(offset), length, encoding);
        if (units > position.character)
            break;
        offset += length;
    }
    return offset;
}

LspClient::LspClient(QObject *parent) : QObject(parent)
{
}

LspClient::~LspClient()
{
    const QSignalBlocker blockSignals(this);
    auto *process = process_;
    stop();
    if (process && process->state() != QProcess::NotRunning)
        process->waitForFinished(500);
}

void LspClient::setWorkspace(const QString &root)
{
    if (root_ == root)
        return;
    stop();
    documents_.clear();
    root_ = root;
}

void LspClient::setExecutable(const QString &program, const QStringList &arguments)
{
    if (executable_ == program && arguments_ == arguments)
        return;
    executable_ = program;
    arguments_ = arguments;
    if (process_ || failed_)
        restart();
}

void LspClient::restart()
{
    stop();
    if (!documents_.isEmpty())
        ensureRunning();
}

void LspClient::openDocument(const QString &path, const QString &text)
{
    if (!supportsClangDocument(path))
        return;
    if (documents_.contains(path)) {
        changeDocument(path, text);
        return;
    }
    const auto document = documents_.insert(path, {text, nextVersion_++});
    if (ready_)
        sendOpen(path, document.value());
    else
        ensureRunning();
}

void LspClient::invalidateDocument(const QString &path)
{
    auto document = documents_.find(path);
    if (document == documents_.end())
        return;
    document->unsynced = true;
}

void LspClient::changeDocument(const QString &path, const QString &text)
{
    auto document = documents_.find(path);
    if (document == documents_.end())
        return;
    document->unsynced = false;
    if (document->text == text)
        return;
    const QString previous = std::exchange(document->text, text);
    document->version = nextVersion_++;
    if (ready_)
        sendChange(path, document.value(), previous);
    if (publishedDiagnostics_.remove(path))
        emit diagnosticsReady(path, {});
}

void LspClient::closeDocument(const QString &path)
{
    if (!documents_.remove(path))
        return;
    if (ready_)
        notify("textDocument/didClose", {{"textDocument", QJsonObject{{"uri", fileUri(path)}}}});
    if (publishedDiagnostics_.remove(path))
        emit diagnosticsReady(path, {});
}

void LspClient::requestCompletion(const QString &path, int utf16Offset)
{
    requestPosition(CompletionMethod, path, utf16Offset);
}

void LspClient::requestDefinition(const QString &path, int utf16Offset)
{
    requestPosition(DefinitionMethod, path, utf16Offset);
}

void LspClient::requestReferences(const QString &path, int utf16Offset)
{
    requestPosition(ReferencesMethod, path, utf16Offset);
}

void LspClient::ensureRunning()
{
    if (process_ || failed_)
        return;
    const QString program = executable_.isEmpty() ? QStandardPaths::findExecutable("clangd") : executable_;
    if (program.isEmpty()) {
        failed_ = true;
        emit errorOccurred(tr("clangd was not found. Set the executable in settings or install clangd."));
        return;
    }

    auto *process = new QProcess(this);
    process_ = process;
    if (!root_.isEmpty())
        process->setWorkingDirectory(root_);
    connect(process, &QProcess::readyReadStandardOutput, this, &LspClient::readOutput);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        errors_ = (errors_ + process->readAllStandardError()).right(4096);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("Could not start clangd: %1").arg(process->errorString()));
    });
    connect(process, &QProcess::finished, this, [this](int code) {
        QString message = tr("clangd stopped (exit code %1).").arg(code);
        if (!errors_.trimmed().isEmpty())
            message += u'\n' + QString::fromUtf8(errors_.trimmed());
        fail(message);
    });
    connect(process, &QProcess::started, this, [this, process] {
        const QJsonArray encodings = {"utf-16", "utf-8", "utf-32"};
        const QJsonObject capabilities = {
            {"general", QJsonObject{{"positionEncodings", encodings}}},
            {"offsetEncoding", QJsonArray{"utf-16", "utf-8"}},
            {"textDocument", QJsonObject{
                {"publishDiagnostics", QJsonObject{{"versionSupport", true}}},
                {"completion", QJsonObject{{"completionItem", QJsonObject{
                    {"snippetSupport", false}, {"insertReplaceSupport", true}}}}}
            }}
        };
        QJsonObject params{
            {"processId", double(QCoreApplication::applicationPid())},
            {"clientInfo", QJsonObject{{"name", "Nebula"}}},
            {"capabilities", capabilities}
        };
        if (!root_.isEmpty()) {
            const QString uri = fileUri(root_);
            params.insert("rootUri", uri);
            params.insert("workspaceFolders", QJsonArray{QJsonObject{
                {"uri", uri}, {"name", QDir(root_).dirName()}}});
        } else {
            params.insert("rootUri", QJsonValue::Null);
        }
        request({"initialize", {}, 0, 0}, params);
        QTimer::singleShot(10000, process, [this, process] {
            if (process_ == process && !ready_)
                fail(tr("clangd did not respond to initialization."));
        });
    });
    process->start(program, arguments_);
}

void LspClient::stop()
{
    if (auto *process = std::exchange(process_, nullptr)) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning)
            process->kill();
        process->deleteLater();
    }
    ready_ = false;
    failed_ = false;
    syncKind_ = 0;
    encoding_ = PositionEncoding::Utf16;
    input_.clear();
    errors_.clear();
    pending_.clear();
    queued_.clear();
    latestCompletionId_ = 0;
    latestNavigationId_ = 0;
    const auto paths = std::exchange(publishedDiagnostics_, {});
    for (const auto &path : paths)
        emit diagnosticsReady(path, {});
}

void LspClient::fail(const QString &message)
{
    stop();
    failed_ = true;
    emit errorOccurred(message);
}

void LspClient::sendOpen(const QString &path, const Document &document)
{
    notify("textDocument/didOpen", {{"textDocument", QJsonObject{
        {"uri", fileUri(path)}, {"languageId", languageId(path)},
        {"version", document.version}, {"text", document.text}
    }}});
}

void LspClient::sendChange(const QString &path, const Document &document, const QString &previous)
{
    if (syncKind_ == 0)
        return;
    QJsonObject change;
    if (syncKind_ == 2) {
        const int limit = std::min(previous.size(), document.text.size());
        int prefix = 0;
        while (prefix < limit && previous.at(prefix) == document.text.at(prefix))
            ++prefix;
        while (!positionBoundary(previous, prefix) || !positionBoundary(document.text, prefix))
            --prefix;
        int oldEnd = previous.size();
        int newEnd = document.text.size();
        while (oldEnd > prefix && newEnd > prefix
               && previous.at(oldEnd - 1) == document.text.at(newEnd - 1)) {
            --oldEnd;
            --newEnd;
        }
        while (!positionBoundary(previous, oldEnd) || !positionBoundary(document.text, newEnd)) {
            ++oldEnd;
            ++newEnd;
        }
        change = {
            {"range", QJsonObject{
                {"start", positionJson(lspPosition(previous, prefix, encoding_))},
                {"end", positionJson(lspPosition(previous, oldEnd, encoding_))}
            }},
            {"text", document.text.mid(prefix, newEnd - prefix)}
        };
    } else {
        change.insert("text", document.text);
    }
    notify("textDocument/didChange", {
        {"textDocument", QJsonObject{{"uri", fileUri(path)}, {"version", document.version}}},
        {"contentChanges", QJsonArray{change}}
    });
}

void LspClient::send(QJsonObject message)
{
    if (!process_ || process_->state() != QProcess::Running)
        return;
    message.insert("jsonrpc", "2.0");
    const QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    process_->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
}

void LspClient::notify(const QString &method, const QJsonObject &params)
{
    send({{"method", method}, {"params", params}});
}

void LspClient::request(const Request &request, const QJsonObject &params)
{
    const int id = nextId_++;
    if (!request.path.isEmpty()) {
        int &latest = request.method == CompletionMethod ? latestCompletionId_ : latestNavigationId_;
        if (pending_.remove(latest))
            notify("$/cancelRequest", {{"id", latest}});
        latest = id;
    }
    pending_.insert(id, request);
    send({{"id", id}, {"method", request.method}, {"params", params}});
}

void LspClient::requestPosition(const QString &method, const QString &path, int offset)
{
    const auto document = documents_.constFind(path);
    if (document == documents_.constEnd()) {
        emit errorOccurred(tr("Language features are available for open C and C++ files."));
        return;
    }
    if (failed_) {
        emit errorOccurred(tr("clangd is unavailable. Use Restart clangd to try again."));
        return;
    }
    const Request request{method, path, document->version, offset};
    if (ready_) {
        dispatchPosition(request);
    } else {
        queued_.append(request);
        ensureRunning();
    }
}

void LspClient::dispatchPosition(const Request &request)
{
    const auto document = documents_.constFind(request.path);
    if (document == documents_.constEnd() || document->unsynced || document->version != request.version)
        return;
    QJsonObject params{
        {"textDocument", QJsonObject{{"uri", fileUri(request.path)}}},
        {"position", positionJson(lspPosition(document->text, request.offset, encoding_))}
    };
    if (request.method == ReferencesMethod)
        params.insert("context", QJsonObject{{"includeDeclaration", true}});
    this->request(request, params);
}

void LspClient::readOutput()
{
    constexpr qsizetype headerLimit = 8192;
    constexpr qsizetype messageLimit = 32 * 1024 * 1024;
    input_ += process_->readAllStandardOutput();
    while (process_) {
        const auto headerEnd = input_.indexOf("\r\n\r\n");
        if (headerEnd < 0 && input_.size() <= headerLimit)
            return;
        if (headerEnd < 0 || headerEnd > headerLimit) {
            fail(tr("clangd sent an invalid JSON-RPC header."));
            return;
        }
        qsizetype length = -1;
        for (const auto &line : input_.left(headerEnd).split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon < 0 || line.left(colon).trimmed().toLower() != "content-length")
                continue;
            bool ok = false;
            const auto value = line.mid(colon + 1).trimmed().toLongLong(&ok);
            if (length >= 0 || !ok || value <= 0 || value > messageLimit) {
                length = -1;
                break;
            }
            length = value;
        }
        if (length < 0) {
            fail(tr("clangd sent an invalid JSON-RPC content length."));
            return;
        }
        const auto bodyStart = headerEnd + 4;
        if (input_.size() - bodyStart < length)
            return;
        const auto message = QJsonDocument::fromJson(input_.mid(bodyStart, length));
        input_.remove(0, bodyStart + length);
        if (!message.isObject()) {
            fail(tr("clangd sent invalid JSON-RPC content."));
            return;
        }
        receive(message.object());
    }
}

void LspClient::receive(const QJsonObject &message)
{
    const auto method = message.value("method").toString();
    if (!method.isEmpty()) {
        const auto params = message.value("params").toObject();
        if (method == "textDocument/publishDiagnostics") {
            handleDiagnostics(params);
        } else if (message.contains("id")) {
            send({{"id", message.value("id")}, {"error", QJsonObject{
                {"code", -32601}, {"message", "Method not found"}
            }}});
        } else if (method == "window/showMessage" && params.value("type").toInt() <= 2) {
            emit errorOccurred(params.value("message").toString());
        }
        return;
    }
    const int id = message.value("id").toInt();
    const auto found = pending_.find(id);
    if (found == pending_.end())
        return;
    const auto request = found.value();
    pending_.erase(found);
    if (!request.path.isEmpty()) {
        const auto document = documents_.constFind(request.path);
        if (document == documents_.constEnd() || document->unsynced || document->version != request.version)
            return;
    }
    if (message.contains("error")) {
        const auto error = message.value("error").toObject();
        const auto detail = error.value("message").toString();
        if (request.method == "initialize")
            fail(tr("clangd initialization failed: %1").arg(detail));
        else if (error.value("code").toInt() != -32800 && error.value("code").toInt() != -32801)
            emit errorOccurred(tr("clangd %1 failed: %2").arg(request.method.section(u'/', -1), detail));
        return;
    }
    if (request.method == "initialize")
        initialized(message.value("result").toObject());
    else
        handleResult(request, message.value("result"));
}

void LspClient::initialized(const QJsonObject &result)
{
    const QJsonObject capabilities = result.value("capabilities").toObject();
    QString encoding = capabilities.value("positionEncoding").toString();
    if (encoding.isEmpty())
        encoding = result.value("offsetEncoding").toString();
    if (encoding == "utf-8")
        encoding_ = PositionEncoding::Utf8;
    else if (encoding == "utf-32")
        encoding_ = PositionEncoding::Utf32;
    const auto sync = capabilities.value("textDocumentSync");
    syncKind_ = sync.isObject() ? sync.toObject().value("change").toInt() : sync.toInt();

    ready_ = true;
    notify("initialized", {});
    for (auto document = documents_.cbegin(); document != documents_.cend(); ++document)
        sendOpen(document.key(), document.value());
    for (const auto &request : std::exchange(queued_, {}))
        dispatchPosition(request);
}

void LspClient::handleDiagnostics(const QJsonObject &params)
{
    const auto path = localPath(params.value("uri").toString());
    const auto document = documents_.constFind(path);
    if (document == documents_.constEnd() || document->unsynced)
        return;
    const auto version = params.value("version");
    if (version.isDouble() && version.toInt() != document->version)
        return;
    QList<LspDiagnostic> items;
    for (const auto &value : params.value("diagnostics").toArray()) {
        const auto item = value.toObject();
        items.append(LspDiagnostic{decodeRange(item.value("range").toObject()),
                                   item.value("severity").toInt(), item.value("message").toString()});
    }
    if (items.isEmpty())
        publishedDiagnostics_.remove(path);
    else
        publishedDiagnostics_.insert(path);
    emit diagnosticsReady(path, items);
}

void LspClient::handleResult(const Request &request, const QJsonValue &result)
{
    if (request.method == CompletionMethod) {
        const auto values = result.isArray() ? result.toArray() : result.toObject().value("items").toArray();
        const auto &text = documents_.constFind(request.path)->text;
        const int offset = std::clamp(request.offset, 0, int(text.size()));
        int wordStart = offset;
        while (wordStart > 0 && identifierCharacter(text.at(wordStart - 1)))
            --wordStart;
        QList<LspCompletion> items;
        for (const auto &value : values) {
            const auto item = value.toObject();
            const QString label = item.value("label").toString();
            if (label.isEmpty() || item.value("insertTextFormat").toInt() == 2)
                continue;
            const auto edit = item.value("textEdit").toObject();
            if (edit.isEmpty()) {
                items.append(LspCompletion{label, item.value("insertText").toString(label), wordStart, offset});
                continue;
            }
            const auto range = decodeRange(edit.value(edit.contains("replace") ? "replace" : "range").toObject());
            const int start = lspOffset(text, range.start, encoding_);
            const int end = lspOffset(text, range.end, encoding_);
            if (start <= end)
                items.append(LspCompletion{label, edit.value("newText").toString(), start, end});
        }
        emit completionReady(request.path, request.offset, items);
        return;
    }

    QJsonArray values = result.toArray();
    if (result.isObject())
        values.append(result);
    QList<LspLocation> locations;
    for (const auto &value : values) {
        const auto item = value.toObject();
        const bool link = item.contains("targetUri");
        const auto path = localPath(item.value(link ? "targetUri" : "uri").toString());
        const auto range = decodeRange(item.value(link ? "targetSelectionRange" : "range").toObject());
        if (!path.isEmpty())
            locations.append(LspLocation{path, range});
    }
    const auto title = request.method == DefinitionMethod ? tr("Definitions") : tr("References");
    emit locationsReady(title, locations);
}

}
