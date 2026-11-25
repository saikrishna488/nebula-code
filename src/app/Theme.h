#pragma once
#include <QJsonObject>
#include <QString>
namespace nebula {
QJsonObject loadTheme(const QString &name,
                      const QJsonObject &themeOverrides = {});
}
