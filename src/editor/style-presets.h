#pragma once

#include "layer-model.h"
#include "title-rich-text.h"

#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QPixmap>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

class QLineEdit;
class QComboBox;
class QListWidget;
class QToolButton;

namespace obsbgs {

enum class StylePresetKind {
    Text,
    Gradient,
};

struct StylePreset {
    QString id;
    QString name;
    QString category;
    StylePresetKind kind = StylePresetKind::Text;
    QJsonObject payload;
};

class StylePresetLibrary {
public:
    StylePresetLibrary();

    bool load();
    bool save() const;
    bool importFromFile(const QString &path, QString *error = nullptr);
    bool exportToFile(const QString &path, StylePresetKind kind, QString *error = nullptr) const;

    QList<StylePreset> presets(StylePresetKind kind) const;
    bool findById(const QString &id, StylePreset *out = nullptr) const;
    QString displayNameForId(const QString &id) const;
    QStringList categories(StylePresetKind kind) const;
    void upsert(const StylePreset &preset);
    bool remove(const QString &id);

    static StylePreset makeTextPreset(const Layer &layer, const QString &name, const QString &category);
    static StylePreset makeGradientPreset(const Layer &layer, const QString &name, const QString &category);
    static bool applyTextPreset(const StylePreset &preset, Layer &layer);
    static bool applyGradientPreset(const StylePreset &preset, Layer &layer);

    /* Inline application support: these convert a saved preset to the rich-text
     * character format used by the on-canvas editor. The returned mask uses the
     * canonical RichTextCharFormatMask values, so the Styles dock and the
     * editor/core cannot silently drift to different bit layouts. */
    static bool textPresetToCharFormat(const StylePreset &preset, RichTextCharFormat &format);
    static bool gradientPresetToCharFormat(const StylePreset &preset, RichTextCharFormat &format);
    static uint32_t textPresetCharMask();
    static uint32_t gradientPresetCharMask();
    static QPixmap thumbnail(const StylePreset &preset, const QSize &size);

private:
    QString storagePath() const;
    void ensureDefaults();
    QList<StylePreset> presets_;
};

class StylePresetPanel : public QWidget {
    Q_OBJECT
public:
    explicit StylePresetPanel(StylePresetKind kind, QWidget *parent = nullptr);

    void setCreatePresetCallback(std::function<StylePreset(const QString &, const QString &)> callback);
    void setApplyPresetCallback(std::function<void(const StylePreset &)> callback);
    void reload();

private slots:
    void refreshList();
    void addCurrentAsPreset();
    void applySelectedPreset();
    void deleteSelectedPreset();
    void importPresets();
    void exportPresets();

private:
    StylePreset *selectedPreset();
    const StylePreset *selectedPreset() const;
    void rebuildCategoryFilter();

    StylePresetKind kind_;
    StylePresetLibrary library_;
    QLineEdit *search_ = nullptr;
    QComboBox *category_filter_ = nullptr;
    QListWidget *list_ = nullptr;
    QToolButton *add_button_ = nullptr;
    QToolButton *apply_button_ = nullptr;
    QToolButton *delete_button_ = nullptr;
    QToolButton *import_button_ = nullptr;
    QToolButton *export_button_ = nullptr;
    std::function<StylePreset(const QString &, const QString &)> create_callback_;
    std::function<void(const StylePreset &)> apply_callback_;
};

} // namespace obsbgs
