#include "Workspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>

namespace nebula {
static QByteArray digest(const QByteArray &data) {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString normalizedPath(const QString &path) {
  QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool loadFile(const QString &path, FileState &file, QString &error) {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    error = input.errorString();
    return false;
  }
  QByteArray data = input.readAll();
  if (input.error() != QFileDevice::NoError) {
    error = input.errorString();
    return false;
  }
  if (data.contains('\0') || !data.isValidUtf8()) {
    error = QObject::tr("This file contains binary data or invalid UTF-8.");
    return false;
  }
  FileState result{normalizedPath(path), data, digest(data),
                   data.startsWith("\xef\xbb\xbf")};
  if (result.bom)
    result.bytes.remove(0, 3);
  file = std::move(result);
  return true;
}

SaveResult saveFile(const FileState &file, const QByteArray &bytes,
                    const QByteArray &expectedHash, QByteArray &newHash,
                    QString &error) {
  QFile current(file.path);
  if (current.exists()) {
    if (!current.open(QIODevice::ReadOnly)) {
      error = current.errorString();
      return SaveResult::Failed;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&current)) {
      error = current.errorString();
      return SaveResult::Failed;
    }
    if (expectedHash != hash.result())
      return SaveResult::ChangedOnDisk;
  } else if (!expectedHash.isEmpty())
    return SaveResult::ChangedOnDisk;
  current.close();
  QSaveFile out(file.path);
  const QByteArray data = file.bom ? QByteArray("\xef\xbb\xbf") + bytes : bytes;
  if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() ||
      !out.commit()) {
    error = out.errorString();
    return SaveResult::Failed;
  }
  newHash = digest(data);
  return SaveResult::Saved;
}

QString storagePath(const QString &name) {
  const QString portable = qEnvironmentVariable("NEBULA_CONFIG_DIR");
  const QString root =
      portable.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
          : portable;
  return QDir(root).filePath(name);
}
QString sessionPath(const QString &root) {
  return storagePath("sessions/" +
                     QString::fromLatin1(digest(root.toUtf8()).toHex()) +
                     ".json");
}
QJsonObject readJson(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  auto obj = QJsonDocument::fromJson(f.readAll()).object();
  return obj.value("version").toInt() == 1 ? obj : QJsonObject{};
}
bool writeJson(const QString &path, const QJsonObject &data, QString &error) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    error = "Cannot create settings directory";
    return false;
  }
  QJsonObject object = data;
  object.insert("version", 1);
  QByteArray encoded = QJsonDocument(object).toJson();
  QSaveFile out(path);
  if (!out.open(QIODevice::WriteOnly) || out.write(encoded) != encoded.size() ||
      !out.commit()) {
    error = out.errorString();
    return false;
  }
  return true;
}
} // namespace nebula
