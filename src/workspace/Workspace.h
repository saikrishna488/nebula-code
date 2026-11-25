#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace nebula {
struct FileState {
  QString path;
  QByteArray bytes;
  QByteArray diskHash;
  bool bom = false;
};
bool loadFile(const QString &path, FileState &file, QString &error);
// An empty expected hash means the destination must be new.
enum class SaveResult { Saved, ChangedOnDisk, Failed };
SaveResult saveFile(const FileState &file, const QByteArray &bytes,
                    const QByteArray &expectedHash, QByteArray &newHash,
                    QString &error);
QString storagePath(const QString &name);
QJsonObject readJson(const QString &path);
bool writeJson(const QString &path, const QJsonObject &data, QString &error);
QString sessionPath(const QString &root);
QString normalizedPath(const QString &path);
} // namespace nebula
