#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <iostream>
#include <string>

namespace {

QByteArray frame(QJsonObject message)
{
    message.insert("jsonrpc", "2.0");
    const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    return "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

void send(const QByteArray &framed)
{
    std::cout.write(framed.constData(), framed.size());
    std::cout.flush();
}

QJsonObject response(const QJsonObject &request, const QJsonValue &result)
{
    return {{"id", request.value("id")}, {"result", result}};
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::string line;
    QJsonObject lastDocument;
    while (std::getline(std::cin, line)) {
        qsizetype size = 0;
        if (line.starts_with("Content-Length: "))
            size = std::stoll(line.substr(16));
        while (line != "\r" && std::getline(std::cin, line)) {
        }
        if (size <= 0)
            return 1;
        QByteArray payload(size, Qt::Uninitialized);
        if (!std::cin.read(payload.data(), size))
            return 1;
        const auto request = QJsonDocument::fromJson(payload).object();
        const auto method = request.value("method").toString();
        const auto params = request.value("params").toObject();
        if (method == "initialize") {
            const auto acknowledgement = frame(response(request, QJsonObject{
                {"capabilities", QJsonObject{{"positionEncoding", "utf-8"}, {"textDocumentSync", 2}}}
            }));
            send(acknowledgement.left(7));
            QThread::msleep(20);
            send(acknowledgement.mid(7));
        } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
            if (method == "textDocument/didChange") {
                const auto change = params.value("contentChanges").toArray().at(0).toObject();
                const auto range = change.value("range").toObject();
                const auto start = range.value("start").toObject().value("character").toInt();
                const auto end = range.value("end").toObject().value("character").toInt();
                if (start != 3 || end != 7 || change.value("text").toString() != "🙃Z")
                    return 2;
            }
            lastDocument = params.value("textDocument").toObject();
            const int version = lastDocument.value("version").toInt();
            const QJsonObject position{{"line", 0}, {"character", 0}};
            const auto diagnostics = [&](int version, const QString &message) {
                return frame({{"method", "textDocument/publishDiagnostics"}, {"params", QJsonObject{
                    {"uri", lastDocument.value("uri")}, {"version", version}, {"diagnostics", QJsonArray{
                        QJsonObject{{"range", QJsonObject{{"start", position}, {"end", position}}},
                                    {"severity", 2}, {"message", message}}
                    }}
                }}});
            };
            send(diagnostics(version - 1, "Obsolete") + diagnostics(version, "Current"));
        } else if (method == "textDocument/completion") {
            const int character = params.value("position").toObject().value("character").toInt();
            const QString label = QString::number(character);
            send(frame(response(request, QJsonArray{QJsonObject{{"label", label}}})));
        } else if (method == "textDocument/definition") {
            QThread::msleep(100);
            const auto position = params.value("position").toObject();
            send(frame(response(request, QJsonObject{
                {"uri", lastDocument.value("uri")},
                {"range", QJsonObject{{"start", position}, {"end", position}}}
            })));
        }
    }
    return 0;
}
