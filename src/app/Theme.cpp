#include "Theme.h"
#include "workspace/Workspace.h"
#include <QColor>
#include <QFileInfo>

namespace nebula {
namespace {
void applyOverrides(QJsonObject &theme, const QJsonObject &overrides) {
  for (auto it = overrides.begin(); it != overrides.end(); ++it) {
    if (!theme.contains(it.key()) || it.key() == "version")
      continue;
    if (it.key() == "font") {
      if (!it.value().toString().isEmpty())
        theme[it.key()] = it.value();
    } else if (it.key() == "pointSize") {
      if (it.value().toInt() >= 6 && it.value().toInt() <= 60)
        theme[it.key()] = it.value();
    } else if (it.key() == "dark") {
      if (it.value().isBool())
        theme[it.key()] = it.value();
    } else if (QColor(it.value().toString()).isValid())
      theme[it.key()] = it.value();
  }
}
} // namespace

QJsonObject loadTheme(const QString &name, const QJsonObject &themeOverrides) {
  const QString base = name == "light" ? "light" : "dark";
  auto theme = readJson(":/nebula/themes/" + base + ".json");
  if (!name.isEmpty() && name == QFileInfo(name).fileName())
    applyOverrides(theme, readJson(storagePath("themes/" + name + ".json")));
  applyOverrides(theme, themeOverrides.value(name).toObject());
  return theme;
}
} // namespace nebula
