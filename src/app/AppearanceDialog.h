#pragma once

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QPushButton;
class QSpinBox;

namespace nebula {

class AppearanceDialog : public QDialog {
public:
  AppearanceDialog(const QString &theme, bool minimap,
                   const QJsonObject &themeOverrides, QWidget *parent);

  QString themeName() const;
  bool minimapVisible() const;
  QJsonObject themeOverrides() const;

private:
  void refreshControls();
  void setOverride(const QString &key, const QJsonValue &value);
  void chooseColor(const QString &key, const QString &title);

  QComboBox *theme_ = nullptr;
  QCheckBox *minimap_ = nullptr;
  QPushButton *panel_ = nullptr;
  QPushButton *background_ = nullptr;
  QPushButton *foreground_ = nullptr;
  QFontComboBox *font_ = nullptr;
  QSpinBox *pointSize_ = nullptr;
  QJsonObject overrides_;
  QJsonObject defaults_;
};

} // namespace nebula
