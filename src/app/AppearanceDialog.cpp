#include "AppearanceDialog.h"

#include "Theme.h"
#include "workspace/Workspace.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontComboBox>
#include <QFormLayout>
#include <QIcon>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

namespace nebula {
namespace {
void showColor(QPushButton *button, const QColor &color) {
  QPixmap swatch(16, 16);
  swatch.fill(color);
  button->setIcon(QIcon(swatch));
  const auto format = color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb;
  button->setText(color.name(format));
}
} // namespace

AppearanceDialog::AppearanceDialog(const QString &theme, bool minimap,
                                   const QJsonObject &themeOverrides,
                                   QWidget *parent)
    : QDialog(parent), overrides_(themeOverrides) {
  setWindowTitle(tr("Appearance Settings"));

  auto *form = new QFormLayout(this);
  theme_ = new QComboBox(this);
  theme_->addItem(tr("Dark"), QStringLiteral("dark"));
  theme_->addItem(tr("Light"), QStringLiteral("light"));
  for (const auto &file :
       QDir(storagePath("themes")).entryInfoList({"*.json"}, QDir::Files)) {
    const QString name = file.completeBaseName();
    if (theme_->findData(name) == -1)
      theme_->addItem(name, name);
  }
  if (!theme.isEmpty() && theme_->findData(theme) == -1)
    theme_->addItem(theme, theme);
  theme_->setCurrentIndex(qMax(0, theme_->findData(theme)));
  form->addRow(tr("&Theme"), theme_);

  const auto addColor = [&](const QString &label, const QString &key,
                            const QString &title) {
    auto *button = new QPushButton(this);
    form->addRow(label, button);
    connect(button, &QPushButton::clicked, this,
            [this, key, title] { chooseColor(key, title); });
    return button;
  };
  panel_ =
      addColor(tr("Sidebar &background"), "panel", tr("Sidebar Background"));
  background_ = addColor(tr("Editor b&ackground"), "background",
                         tr("Editor Background"));
  foreground_ = addColor(tr("Editor &text color"), "foreground",
                        tr("Editor Text Color"));

  font_ = new QFontComboBox(this);
  form->addRow(tr("Editor &font"), font_);
  pointSize_ = new QSpinBox(this);
  pointSize_->setRange(6, 60);
  pointSize_->setSuffix(tr(" pt"));
  form->addRow(tr("Font &size"), pointSize_);
  minimap_ = new QCheckBox(tr("Show &minimap"), this);
  minimap_->setChecked(minimap);
  form->addRow(QString(), minimap_);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                           this);
  auto *reset = buttons->addButton(tr("Reset Theme Customizations"),
                                   QDialogButtonBox::ResetRole);
  form->addRow(buttons);

  connect(theme_, &QComboBox::currentIndexChanged, this,
          [this] { refreshControls(); });
  connect(font_, &QFontComboBox::currentFontChanged, this,
          [this](const QFont &font) { setOverride("font", font.family()); });
  connect(pointSize_, &QSpinBox::valueChanged, this,
          [this](int size) { setOverride("pointSize", size); });
  connect(reset, &QPushButton::clicked, this, [this] {
    overrides_.remove(themeName());
    refreshControls();
  });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  refreshControls();
}

QString AppearanceDialog::themeName() const {
  return theme_->currentData().toString();
}

bool AppearanceDialog::minimapVisible() const { return minimap_->isChecked(); }

QJsonObject AppearanceDialog::themeOverrides() const { return overrides_; }

void AppearanceDialog::refreshControls() {
  defaults_ = loadTheme(themeName());
  const auto theme = loadTheme(themeName(), overrides_);
  showColor(panel_, QColor(theme["panel"].toString()));
  showColor(background_, QColor(theme["background"].toString()));
  showColor(foreground_, QColor(theme["foreground"].toString()));
  const QSignalBlocker blockFont(font_);
  const QSignalBlocker blockSize(pointSize_);
  font_->setCurrentFont(QFont(theme["font"].toString()));
  pointSize_->setValue(theme["pointSize"].toInt());
}

void AppearanceDialog::setOverride(const QString &key, const QJsonValue &value) {
  const QString name = themeName();
  auto theme = overrides_.value(name).toObject();
  if (value == defaults_.value(key))
    theme.remove(key);
  else
    theme.insert(key, value);
  if (theme.isEmpty())
    overrides_.remove(name);
  else
    overrides_.insert(name, theme);
}

void AppearanceDialog::chooseColor(const QString &key, const QString &title) {
  const QColor current(loadTheme(themeName(), overrides_).value(key).toString());
  const QColor color = QColorDialog::getColor(current, this, title);
  if (color.isValid() && color != current) {
    setOverride(key, color.name());
    refreshControls();
  }
}
} // namespace nebula
