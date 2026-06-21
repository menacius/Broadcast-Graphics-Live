#include "title-editor-internal.h"
#include "title-logger.h"
#include "style-presets.h"

#include <QClipboard>
#include <QScopedValueRollback>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>
#include <QScreen>
#include <QWindow>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>

#include <cmath>

namespace {
constexpr double kMaxPlaybackUiHz = 60.0;

double editor_playback_ui_frame_duration()
{
    return std::max(obs_frame_duration(), 1.0 / kMaxPlaybackUiHz);
}

int editor_playback_ui_timer_interval_ms()
{
    return std::max(1, static_cast<int>(std::lround(editor_playback_ui_frame_duration() * 1000.0)));
}

QString editor_recent_color_hex(const QColor &color)
{
    if (!color.isValid())
        return QString();
    if (color.alpha() < 255) {
        return QStringLiteral("#%1%2%3%4")
            .arg(color.red(), 2, 16, QLatin1Char('0'))
            .arg(color.green(), 2, 16, QLatin1Char('0'))
            .arg(color.blue(), 2, 16, QLatin1Char('0'))
            .arg(color.alpha(), 2, 16, QLatin1Char('0'))
            .toUpper();
    }
    return color.name(QColor::HexRgb).toUpper();
}

bool editor_parse_recent_color_hex(const QString &hex, QColor &color)
{
    QString s = hex.trimmed();
    if (s.startsWith(QLatin1Char('#')))
        s.remove(0, 1);

    bool ok = false;
    if (s.size() == 6) {
        const int r = s.mid(0, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int g = s.mid(2, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int b = s.mid(4, 2).toInt(&ok, 16);
        if (!ok) return false;
        color = QColor(r, g, b, 255);
        return color.isValid();
    }

    if (s.size() == 8) {
        const int r = s.mid(0, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int g = s.mid(2, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int b = s.mid(4, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int a = s.mid(6, 2).toInt(&ok, 16);
        if (!ok) return false;
        color = QColor(r, g, b, a);
        return color.isValid();
    }

    return false;
}

QString editor_color_swatch_style(const QColor &color, bool enabled = true)
{
    const QPalette pal = qApp->palette();
    const QString border = pal.color(QPalette::Mid).name(QColor::HexRgb);
    const QString highlight = pal.color(QPalette::Highlight).name(QColor::HexRgb);
    if (!enabled || !color.isValid()) {
        const QString bg = pal.color(QPalette::Window).name(QColor::HexRgb);
        return QStringLiteral("QToolButton{background:%1;border:1px dashed %2;border-radius:3px;}"
                              "QToolButton:hover{border:1px dashed %2;}")
            .arg(bg, border);
    }

    const QString bg = color.name(color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
    return QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:3px;}"
                          "QToolButton:hover{border:2px solid %3;}")
        .arg(bg, border, highlight);
}

QString editor_slugify(QString text)
{
    text = text.trimmed().toLower();
    QString slug;
    bool previous_dash = false;
    for (const QChar ch : text) {
        const bool valid = ch.isLetterOrNumber();
        if (valid) {
            slug.append(ch);
            previous_dash = false;
        } else if (!previous_dash && !slug.isEmpty()) {
            slug.append(QLatin1Char('-'));
            previous_dash = true;
        }
    }
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    return slug.isEmpty() ? QStringLiteral("library") : slug;
}

QString editor_color_libraries_path()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/OBS Graphics Studio Pro");
    QDir dir(base);
    dir.mkpath(QStringLiteral("palettes"));
    return dir.filePath(QStringLiteral("palettes/user-color-libraries.palette.json"));
}

QString obsgs_load_selected_color_library_slug()
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Color"));
    return settings.value(QStringLiteral("selectedColorLibrarySlug")).toString();
}

void obsgs_save_selected_color_library_slug(const QString &slug)
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Color"));
    settings.setValue(QStringLiteral("selectedColorLibrarySlug"), slug);
    settings.sync();
}

class LongPressToolButton final : public QToolButton {
public:
    explicit LongPressToolButton(QWidget *parent = nullptr) : QToolButton(parent)
    {
        long_press_timer_.setSingleShot(true);
        long_press_timer_.setInterval(250);
        QObject::connect(&long_press_timer_, &QTimer::timeout, this, [this]() {
            long_press_triggered_ = true;
            if (menu())
                menu()->popup(mapToGlobal(QPoint(0, height())));
        });
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            long_press_triggered_ = false;
            long_press_timer_.start();
            setDown(true);
            event->accept();
            return;
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const bool was_long_press = long_press_triggered_;
            long_press_timer_.stop();
            setDown(false);
            event->accept();
            if (!was_long_press && rect().contains(event->position().toPoint()))
                click();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

private:
    QTimer long_press_timer_;
    bool long_press_triggered_ = false;
};

const Layer *editor_layer_by_id_for_parenting(const std::shared_ptr<Title> &title, const std::string &id)
{
    if (!title || id.empty()) return nullptr;
    for (const auto &candidate : title->layers) {
        if (candidate && candidate->id == id) return candidate.get();
    }
    return nullptr;
}

QTransform editor_layer_world_transform_for_parenting(const std::shared_ptr<Title> &title,
                                                      const Layer &layer,
                                                      double playhead,
                                                      int depth = 0)
{
    QTransform xf;
    if (depth > 64) return xf;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = editor_layer_by_id_for_parenting(title, layer.parent_id))
            xf = editor_layer_world_transform_for_parenting(title, *parent, playhead, depth + 1);
    }
    const double lt = std::max(0.0, playhead - layer.in_time);
    xf.translate(layer.position.evaluate(lt).x, layer.position.evaluate(lt).y);
    xf.rotate(layer.rotation.evaluate(lt));
    xf.scale(layer.scale.evaluate(lt).x, layer.scale.evaluate(lt).y);
    return xf;
}

bool editor_parenting_would_cycle(const std::shared_ptr<Title> &title,
                                  const std::string &layer_id,
                                  const std::string &parent_id)
{
    std::string cursor = parent_id;
    int guard = 0;
    while (!cursor.empty() && guard++ < 64) {
        if (cursor == layer_id) return true;
        const Layer *parent = editor_layer_by_id_for_parenting(title, cursor);
        if (!parent) break;
        cursor = parent->parent_id;
    }
    return false;
}

// Editor-session-only defaults. These intentionally live only in process memory:
// closing/reopening the editor window within the same OBS run keeps the user's
// last foreground/background and new-layer style, but restarting OBS resets them
// to the compiled/project defaults instead of reading/writing QSettings.
bool g_editor_session_sidebar_colors_initialized = false;
QColor g_editor_session_foreground_color;
QColor g_editor_session_background_color;
bool g_editor_session_new_layer_defaults_initialized = false;
Layer g_editor_session_new_layer_style;

} // namespace

TitleEditor::TitleEditor(QWidget *parent)
    : QMainWindow(parent, Qt::Window)
{
    setWindowTitle(obsgs_tr("OBSTitles.EditorWindowTitle"));
    resize(1280, 760);
    setMinimumSize(900, 600);

    /* Dark background */
    QPalette pal = palette();
    pal.setColor(QPalette::Window,     C_BG_DARK);
    pal.setColor(QPalette::WindowText, C_TEXT);
    pal.setColor(QPalette::Base,       C_BG_MID);
    pal.setColor(QPalette::AlternateBase, C_BG_LIGHT);
    pal.setColor(QPalette::Text,       C_TEXT);
    pal.setColor(QPalette::Button,     C_BG_LIGHT);
    pal.setColor(QPalette::ButtonText, C_TEXT);
    pal.setColor(QPalette::Highlight,  C_ACCENT);
    setPalette(pal);
    setAutoFillBackground(true);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks |
                   QMainWindow::GroupedDragging);

    build_ui();
    load_sidebar_default_colors();
    load_new_layer_defaults();
    update_sidebar_color_swatches(nullptr);

    play_timer_ = new QTimer(this);
    play_timer_->setTimerType(Qt::PreciseTimer);
    // Transport is always driven by the project/OBS frame rate. It must not
    // change when the editor is moved between monitors with different refresh rates.
    play_timer_->setInterval(editor_playback_ui_timer_interval_ms());
    connect(play_timer_, &QTimer::timeout, this, &TitleEditor::tick);

    gui_refresh_timer_ = new QTimer(this);
    gui_refresh_timer_->setTimerType(Qt::PreciseTimer);
    connect(gui_refresh_timer_, &QTimer::timeout, this, [this]() {
        // Never enqueue paints while Qt is reparenting/rebuilding the dock tree.
        // Updating child widgets during QMainWindow layout transitions can race
        // with Qt's internal dock layout items and lead to access violations.
        if (dock_layout_transition_ || !isVisible() || isMinimized() || !isActiveWindow())
            return;

        // High-refresh presentation is reserved for interactive UI/canvas work.
        // Playback frames continue to advance only from play_timer_ at project FPS.
        const bool pointer_drag = QApplication::mouseButtons() != Qt::NoButton;
        if (!pointer_drag && active_text_edit_layer_id_.empty())
            return;

        if (canvas_)
            canvas_->update();
        if (timeline_ && pointer_drag && timeline_->underMouse())
            timeline_->update();
    });

    layout_settle_timer_ = new QTimer(this);
    layout_settle_timer_->setSingleShot(true);
    layout_settle_timer_->setInterval(90);
    connect(layout_settle_timer_, &QTimer::timeout, this, [this]() {
        dock_layout_transition_ = false;
        update_display_refresh_pacing();
        if (canvas_ && isVisible() && !isMinimized())
            canvas_->update();
    });

    cache_invalidation_timer_ = new QTimer(this);
    cache_invalidation_timer_->setSingleShot(true);
    cache_invalidation_timer_->setInterval(180);
    connect(cache_invalidation_timer_, &QTimer::timeout, this, [this]() {
        CacheManager::instance().setInteractiveBypass(false);
        if (!title_)
            return;
        CacheManager::instance().invalidateAll(title_);
        CacheManager::instance().reprioritize(title_, playhead_);
        if (timeline_) timeline_->update();
    });

    clock_timer_ = new QTimer(this);
    // Dynamic text does not need to wake the editor/OBS render path at 30 Hz.
    // A 10 Hz UI refresh is smooth for clocks while avoiding constant canvas invalidation.
    clock_timer_->setInterval(100);
    connect(clock_timer_, &QTimer::timeout, this, [this]() {
        update_title_bar();
        if (canvas_ && title_has_dynamic_text_layer(title_) && isVisible() && !isMinimized())
            canvas_->update();
    });
    clock_timer_->start();

    qApp->installEventFilter(this);
    QTimer::singleShot(0, this, &TitleEditor::update_display_refresh_pacing);
}

TitleEditor::~TitleEditor()
{
    CacheManager::instance().setEditorPrerenderFocus(QString(), false);
    save_sidebar_default_colors();
    save_new_layer_defaults();
    save_editor_layout();
    if (qApp)
        qApp->removeEventFilter(this);
}




QWidget *TitleEditor::create_effects_panel()
{
    effects_panel_ = new EffectsPanel(this);
    connect(effects_panel_, &EffectsPanel::property_changed,
            this, [this](bool push_undo_snapshot) {
                if (updating_layer_panels_)
                    return;
                // Every EffectsPanel notification represents a real visual-model edit.
                // Bypass the selection-only visual hash guard so enable/disable and
                // stack changes always invalidate prerender/frame caches immediately.
                force_next_title_visual_update();
                on_title_modified(push_undo_snapshot);
                if (layers_) layers_->refresh();
            });
    return effects_panel_;
}

QWidget *TitleEditor::create_prerender_panel()
{
    prerender_panel_ = new PrerenderDock(this);
    connect(prerender_panel_, &PrerenderDock::cacheWorkAreaRequested, this, [this]() {
        if (timeline_) timeline_->update();
    });
    connect(prerender_panel_, &PrerenderDock::cacheEntireTimelineRequested, this, [this]() {
        if (timeline_) timeline_->update();
    });
    return prerender_panel_;
}

QWidget *TitleEditor::create_styles_panel()
{
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("OBSGraphicsStudioProStylesTabs"));
    tabs->setDocumentMode(true);

    auto *text_styles = new obsgsp::StylePresetPanel(obsgsp::StylePresetKind::Text, tabs);
    text_styles->setCreatePresetCallback([this](const QString &name, const QString &category) {
        Layer fallback;
        if (title_ && !sel_layer_id_.empty()) {
            if (auto layer = title_->find_layer(sel_layer_id_)) {
                if (active_text_edit_layer_id_ == layer->id) {
                    RichTextCharFormatSummary summary = summarize_rich_text_char_format(*layer, true);
                    if (summary.valid) {
                        Layer inline_style = *layer;
                        inline_style.font_family = summary.format.font_family;
                        inline_style.font_style = summary.format.font_style;
                        inline_style.font_size = summary.format.font_size;
                        inline_style.font_bold = summary.format.bold;
                        inline_style.font_italic = summary.format.italic;
                        inline_style.text_underline = summary.format.underline;
                        inline_style.text_strikethrough = summary.format.strikethrough;
                        inline_style.char_tracking = summary.format.tracking;
                        inline_style.char_scale_x = summary.format.scale_x;
                        inline_style.char_scale_y = summary.format.scale_y;
                        inline_style.baseline_shift = summary.format.baseline_shift;
                        inline_style.fill_type = summary.format.fill.type;
                        inline_style.text_color = summary.format.fill.color;
                        inline_style.fill_color = summary.format.fill.color;
                        inline_style.gradient_type = summary.format.fill.gradient_type;
                        inline_style.gradient_start_color = summary.format.fill.gradient_start_color;
                        inline_style.gradient_end_color = summary.format.fill.gradient_end_color;
                        inline_style.gradient_start_pos = summary.format.fill.gradient_start_pos;
                        inline_style.gradient_end_pos = summary.format.fill.gradient_end_pos;
                        inline_style.gradient_start_opacity = summary.format.fill.gradient_start_opacity;
                        inline_style.gradient_end_opacity = summary.format.fill.gradient_end_opacity;
                        inline_style.gradient_opacity = summary.format.fill.gradient_opacity;
                        inline_style.gradient_angle = summary.format.fill.gradient_angle;
                        inline_style.gradient_center_x = summary.format.fill.gradient_center_x;
                        inline_style.gradient_center_y = summary.format.fill.gradient_center_y;
                        inline_style.gradient_scale = summary.format.fill.gradient_scale;
                        inline_style.gradient_focal_x = summary.format.fill.gradient_focal_x;
                        inline_style.gradient_focal_y = summary.format.fill.gradient_focal_y;
                        return obsgsp::StylePresetLibrary::makeTextPreset(inline_style, name, category);
                    }
                }
                return obsgsp::StylePresetLibrary::makeTextPreset(*layer, name, category);
            }
        }
        return obsgsp::StylePresetLibrary::makeTextPreset(fallback, name, category);
    });
    text_styles->setApplyPresetCallback([this](const obsgsp::StylePreset &preset) {
        if (!title_ || sel_layer_id_.empty()) return;
        if (auto layer = title_->find_layer(sel_layer_id_)) {
            if (layer->type != LayerType::Text && layer->type != LayerType::Clock && layer->type != LayerType::Ticker)
                return;
            push_undo_snapshot();
            const bool apply_inline = active_text_edit_layer_id_ == layer->id;
            if (apply_inline) {
                RichTextCharFormat fmt = layer_char_format_for_editor(*layer);
                if (!obsgsp::StylePresetLibrary::textPresetToCharFormat(preset, fmt))
                    return;
                const uint32_t mask = obsgsp::StylePresetLibrary::textPresetCharMask();
                apply_rich_text_format_to_layer_range(*layer, fmt, mask, true);
                if (canvas_) canvas_->apply_active_text_char_format(layer->id, fmt, mask);
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
                return;
            }
            if (obsgsp::StylePresetLibrary::applyTextPreset(preset, *layer)) {
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
            }
        }
    });

    auto *gradient_styles = new obsgsp::StylePresetPanel(obsgsp::StylePresetKind::Gradient, tabs);
    gradient_styles->setCreatePresetCallback([this](const QString &name, const QString &category) {
        Layer fallback;
        fallback.fill_type = 1;
        if (title_ && !sel_layer_id_.empty()) {
            if (auto layer = title_->find_layer(sel_layer_id_)) {
                if (active_text_edit_layer_id_ == layer->id &&
                    (layer->type == LayerType::Text || layer->type == LayerType::Clock || layer->type == LayerType::Ticker)) {
                    RichTextCharFormatSummary summary = summarize_rich_text_char_format(*layer, true);
                    if (summary.valid) {
                        Layer inline_gradient = *layer;
                        inline_gradient.fill_type = summary.format.fill.type;
                        inline_gradient.text_color = summary.format.fill.color;
                        inline_gradient.fill_color = summary.format.fill.color;
                        inline_gradient.gradient_type = summary.format.fill.gradient_type;
                        inline_gradient.gradient_start_color = summary.format.fill.gradient_start_color;
                        inline_gradient.gradient_end_color = summary.format.fill.gradient_end_color;
                        inline_gradient.gradient_start_pos = summary.format.fill.gradient_start_pos;
                        inline_gradient.gradient_end_pos = summary.format.fill.gradient_end_pos;
                        inline_gradient.gradient_start_opacity = summary.format.fill.gradient_start_opacity;
                        inline_gradient.gradient_end_opacity = summary.format.fill.gradient_end_opacity;
                        inline_gradient.gradient_opacity = summary.format.fill.gradient_opacity;
                        inline_gradient.gradient_angle = summary.format.fill.gradient_angle;
                        inline_gradient.gradient_center_x = summary.format.fill.gradient_center_x;
                        inline_gradient.gradient_center_y = summary.format.fill.gradient_center_y;
                        inline_gradient.gradient_scale = summary.format.fill.gradient_scale;
                        inline_gradient.gradient_focal_x = summary.format.fill.gradient_focal_x;
                        inline_gradient.gradient_focal_y = summary.format.fill.gradient_focal_y;
                        return obsgsp::StylePresetLibrary::makeGradientPreset(inline_gradient, name, category);
                    }
                }
                return obsgsp::StylePresetLibrary::makeGradientPreset(*layer, name, category);
            }
        }
        return obsgsp::StylePresetLibrary::makeGradientPreset(fallback, name, category);
    });
    gradient_styles->setApplyPresetCallback([this](const obsgsp::StylePreset &preset) {
        if (!title_ || sel_layer_id_.empty()) return;
        if (auto layer = title_->find_layer(sel_layer_id_)) {
            push_undo_snapshot();
            const bool apply_inline = active_text_edit_layer_id_ == layer->id &&
                                      (layer->type == LayerType::Text || layer->type == LayerType::Clock || layer->type == LayerType::Ticker);
            if (apply_inline) {
                RichTextCharFormat fmt = layer_char_format_for_editor(*layer);
                if (!obsgsp::StylePresetLibrary::gradientPresetToCharFormat(preset, fmt))
                    return;
                const uint32_t mask = obsgsp::StylePresetLibrary::gradientPresetCharMask();
                apply_rich_text_format_to_layer_range(*layer, fmt, mask, true);
                if (canvas_) canvas_->apply_active_text_char_format(layer->id, fmt, mask);
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                update_sidebar_color_swatches(layer);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
                return;
            }
            if (obsgsp::StylePresetLibrary::applyGradientPreset(preset, *layer)) {
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                update_sidebar_color_swatches(layer);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
            }
        }
    });

    auto make_placeholder_tab = [](const QString &title, const QString &description) {
        auto *tab = new QWidget;
        auto *layout = new QVBoxLayout(tab);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(6);
        auto *heading = new QLabel(title, tab);
        QFont heading_font = heading->font();
        heading_font.setBold(true);
        heading->setFont(heading_font);
        heading->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(qApp->palette().color(QPalette::WindowText).name(QColor::HexRgb)));
        auto *body = new QLabel(description, tab);
        body->setWordWrap(true);
        body->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg((qApp->palette().color(QPalette::WindowText).lightness() < 128 ? qApp->palette().color(QPalette::WindowText).lighter(135) : qApp->palette().color(QPalette::WindowText).darker(135)).name(QColor::HexRgb)));
        layout->addWidget(heading);
        layout->addWidget(body);
        layout->addStretch(1);
        return tab;
    };

    tabs->addTab(text_styles, obsgs_tr("OBSTitles.Text"));
    tabs->addTab(gradient_styles, obsgs_tr("OBSTitles.Gradient"));
    tabs->addTab(make_placeholder_tab(obsgs_tr("OBSTitles.PatternStyles"),
                                      obsgs_tr("OBSTitles.PatternStylesHint")),
                 obsgs_tr("OBSTitles.Pattern"));

    return tabs;
}

QWidget *TitleEditor::create_color_swatches_panel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *recent_label = new QLabel(obsgs_tr("OBSTitles.RecentColors"), panel);
    QFont section_font = recent_label->font();
    section_font.setBold(true);
    recent_label->setFont(section_font);
    layout->addWidget(recent_label);

    auto *recent_grid_widget = new QWidget(panel);
    recent_color_swatches_grid_ = new QGridLayout(recent_grid_widget);
    recent_color_swatches_grid_->setContentsMargins(0, 0, 0, 0);
    recent_color_swatches_grid_->setHorizontalSpacing(6);
    recent_color_swatches_grid_->setVerticalSpacing(6);
    recent_color_swatches_grid_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    recent_color_swatch_buttons_.clear();
    recent_color_swatch_buttons_.reserve(16);
    for (int i = 0; i < 16; ++i) {
        auto *swatch = new QToolButton(recent_grid_widget);
        swatch->setObjectName(QStringLiteral("OBSGraphicsStudioProRecentColorSwatch"));
        swatch->setFixedSize(24, 24);
        swatch->setAutoRaise(false);
        connect(swatch, &QToolButton::clicked, this, [this, i]() {
            QStringList recent_hexes = obsgs_load_recent_color_hexes();
            if (title_) {
                for (const auto &hex : title_->editor_recent_color_hexes) {
                    const QString value = QString::fromStdString(hex);
                    if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                        recent_hexes << value;
                }
            }
            if (i >= recent_hexes.size())
                return;
            QColor color;
            if (!editor_parse_recent_color_hex(recent_hexes[i], color))
                return;
            apply_picked_color_to_selection(color);
            remember_recent_color(color);
        });
        swatch->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(swatch, &QToolButton::customContextMenuRequested, this, [this, swatch, i](const QPoint &pos) {
            QStringList recent_hexes = obsgs_load_recent_color_hexes();
            if (title_) {
                for (const auto &hex : title_->editor_recent_color_hexes) {
                    const QString value = QString::fromStdString(hex);
                    if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                        recent_hexes << value;
                }
            }
            if (i >= recent_hexes.size())
                return;
            QColor color;
            const bool has_color = editor_parse_recent_color_hex(recent_hexes[i], color);
            QMenu menu(swatch);
            QAction *save_action = nullptr;
            if (has_color)
                save_action = menu.addAction(obsgs_tr("OBSTitles.AddToLibrary"));
            QAction *remove_action = menu.addAction(obsgs_tr("OBSTitles.Delete"));
            QAction *selected = menu.exec(swatch->mapToGlobal(pos));
            if (selected == save_action)
                show_add_color_to_library_dialog(color);
            else if (selected == remove_action)
                remove_recent_color(i);
        });
        recent_color_swatches_grid_->addWidget(swatch, i / 8, i % 8);
        recent_color_swatch_buttons_.push_back(swatch);
    }
    layout->addWidget(recent_grid_widget, 0, Qt::AlignTop | Qt::AlignLeft);

    auto *library_label = new QLabel(obsgs_tr("OBSTitles.ColorLibraries"), panel);
    library_label->setFont(section_font);
    layout->addWidget(library_label);

    auto *library_toolbar = new QWidget(panel);
    auto *library_toolbar_layout = new QHBoxLayout(library_toolbar);
    library_toolbar_layout->setContentsMargins(0, 0, 0, 0);
    library_toolbar_layout->setSpacing(4);
    color_library_combo_ = new QComboBox(library_toolbar);
    color_library_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    color_library_add_button_ = new QToolButton(library_toolbar);
    color_library_add_button_->setIcon(obs_icon("add.svg"));
    color_library_add_button_->setToolTip(obsgs_tr("OBSTitles.New"));
    color_library_rename_button_ = new QToolButton(library_toolbar);
    color_library_rename_button_->setIcon(obs_icon("settings.svg"));
    color_library_rename_button_->setToolTip(obsgs_tr("OBSTitles.Rename"));
    color_library_delete_button_ = new QToolButton(library_toolbar);
    color_library_delete_button_->setIcon(obs_icon("delete.svg"));
    color_library_delete_button_->setToolTip(obsgs_tr("OBSTitles.Delete"));
    for (auto *button : {color_library_add_button_, color_library_rename_button_, color_library_delete_button_})
        button->setFixedSize(26, 24);
    library_toolbar_layout->addWidget(color_library_combo_, 1);
    library_toolbar_layout->addWidget(color_library_add_button_);
    library_toolbar_layout->addWidget(color_library_rename_button_);
    library_toolbar_layout->addWidget(color_library_delete_button_);
    layout->addWidget(library_toolbar);

    auto *library_scroll = new QScrollArea(panel);
    library_scroll->setWidgetResizable(true);
    library_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    library_scroll->setMinimumHeight(140);
    color_library_swatch_widget_ = new QWidget(library_scroll);
    color_library_swatches_grid_ = new QGridLayout(color_library_swatch_widget_);
    color_library_swatches_grid_->setContentsMargins(0, 0, 0, 0);
    color_library_swatches_grid_->setHorizontalSpacing(6);
    color_library_swatches_grid_->setVerticalSpacing(6);
    color_library_swatches_grid_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    library_scroll->setWidget(color_library_swatch_widget_);
    layout->addWidget(library_scroll, 1);

    connect(color_library_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || index >= (int)color_libraries_.size())
            return;
        selected_color_library_index_ = index;
        persist_selected_color_library();
        refresh_color_library_controls();
    });
    connect(color_library_add_button_, &QToolButton::clicked, this, &TitleEditor::create_color_library);
    connect(color_library_rename_button_, &QToolButton::clicked, this, &TitleEditor::rename_current_color_library);
    connect(color_library_delete_button_, &QToolButton::clicked, this, &TitleEditor::delete_current_color_library);

    load_color_libraries();
    refresh_color_library_controls();
    refresh_color_swatches_panel();
    return panel;
}

void TitleEditor::remember_recent_color(const QColor &color)
{
    if (!color.isValid() || color.alpha() <= 0)
        return;

    const QString hex = editor_recent_color_hex(color);
    if (hex.isEmpty())
        return;

    QStringList recent_hexes = obsgs_load_recent_color_hexes();
    for (int i = recent_hexes.size() - 1; i >= 0; --i) {
        if (recent_hexes[i].compare(hex, Qt::CaseInsensitive) == 0)
            recent_hexes.removeAt(i);
    }
    recent_hexes.prepend(hex);
    while (recent_hexes.size() > 16)
        recent_hexes.removeLast();
    obsgs_save_recent_color_hexes(recent_hexes);

    if (title_) {
        title_->editor_recent_color_hexes.clear();
        for (const QString &entry : recent_hexes)
            title_->editor_recent_color_hexes.push_back(entry.toStdString());
    }

    refresh_color_swatches_panel();
    persist_recent_colors();
}

void TitleEditor::remove_recent_color(int index)
{
    QStringList recent_hexes = obsgs_load_recent_color_hexes();
    if (title_) {
        for (const auto &hex : title_->editor_recent_color_hexes) {
            const QString value = QString::fromStdString(hex);
            if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                recent_hexes << value;
        }
    }
    if (index < 0 || index >= recent_hexes.size())
        return;

    recent_hexes.removeAt(index);
    obsgs_save_recent_color_hexes(recent_hexes);
    if (title_) {
        title_->editor_recent_color_hexes.clear();
        for (const QString &hex : recent_hexes)
            title_->editor_recent_color_hexes.push_back(hex.toStdString());
    }
    refresh_color_swatches_panel();
    persist_recent_colors();
}

void TitleEditor::persist_recent_colors()
{
    if (!title_)
        return;

    auto stored = TitleDataStore::instance().get_title(editing_title_id_.empty() ? title_->id : editing_title_id_);
    if (!stored)
        return;

    stored->editor_recent_color_hexes = title_->editor_recent_color_hexes;
    QStringList recent_hexes;
    for (const auto &hex : title_->editor_recent_color_hexes)
        recent_hexes << QString::fromStdString(hex);
    obsgs_save_recent_color_hexes(recent_hexes);
    TitleDataStore::instance().save_async();
}

void TitleEditor::load_color_libraries()
{
    color_libraries_.clear();

    ColorLibrary open_color;
    open_color.name = QStringLiteral("Open Color");
    open_color.slug = QStringLiteral("open-color");
    open_color.built_in = true;
    auto add_open_color = [&](const QString &group, const QStringList &hexes) {
        for (int i = 0; i < hexes.size(); ++i) {
            QColor color(hexes[i]);
            if (color.isValid())
                open_color.colors.push_back({QStringLiteral("%1 %2").arg(group).arg(i), color});
        }
    };
    open_color.colors.push_back({QStringLiteral("white"), QColor(QStringLiteral("#ffffff"))});
    open_color.colors.push_back({QStringLiteral("black"), QColor(QStringLiteral("#000000"))});
    add_open_color(QStringLiteral("gray"), {"#f8f9fa", "#f1f3f5", "#e9ecef", "#dee2e6", "#ced4da", "#adb5bd", "#868e96", "#495057", "#343a40", "#212529"});
    add_open_color(QStringLiteral("red"), {"#fff5f5", "#ffe3e3", "#ffc9c9", "#ffa8a8", "#ff8787", "#ff6b6b", "#fa5252", "#f03e3e", "#e03131", "#c92a2a"});
    add_open_color(QStringLiteral("pink"), {"#fff0f6", "#ffdeeb", "#fcc2d7", "#faa2c1", "#f783ac", "#f06595", "#e64980", "#d6336c", "#c2255c", "#a61e4d"});
    add_open_color(QStringLiteral("grape"), {"#f8f0fc", "#f3d9fa", "#eebefa", "#e599f7", "#da77f2", "#cc5de8", "#be4bdb", "#ae3ec9", "#9c36b5", "#862e9c"});
    add_open_color(QStringLiteral("violet"), {"#f3f0ff", "#e5dbff", "#d0bfff", "#b197fc", "#9775fa", "#845ef7", "#7950f2", "#7048e8", "#6741d9", "#5f3dc4"});
    add_open_color(QStringLiteral("indigo"), {"#edf2ff", "#dbe4ff", "#bac8ff", "#91a7ff", "#748ffc", "#5c7cfa", "#4c6ef5", "#4263eb", "#3b5bdb", "#364fc7"});
    add_open_color(QStringLiteral("blue"), {"#e7f5ff", "#d0ebff", "#a5d8ff", "#74c0fc", "#4dabf7", "#339af0", "#228be6", "#1c7ed6", "#1971c2", "#1864ab"});
    add_open_color(QStringLiteral("cyan"), {"#e3fafc", "#c5f6fa", "#99e9f2", "#66d9e8", "#3bc9db", "#22b8cf", "#15aabf", "#1098ad", "#0c8599", "#0b7285"});
    add_open_color(QStringLiteral("teal"), {"#e6fcf5", "#c3fae8", "#96f2d7", "#63e6be", "#38d9a9", "#20c997", "#12b886", "#0ca678", "#099268", "#087f5b"});
    add_open_color(QStringLiteral("green"), {"#ebfbee", "#d3f9d8", "#b2f2bb", "#8ce99a", "#69db7c", "#51cf66", "#40c057", "#37b24d", "#2f9e44", "#2b8a3e"});
    add_open_color(QStringLiteral("lime"), {"#f4fce3", "#e9fac8", "#d8f5a2", "#c0eb75", "#a9e34b", "#94d82d", "#82c91e", "#74b816", "#66a80f", "#5c940d"});
    add_open_color(QStringLiteral("yellow"), {"#fff9db", "#fff3bf", "#ffec99", "#ffe066", "#ffd43b", "#fcc419", "#fab005", "#f59f00", "#f08c00", "#e67700"});
    add_open_color(QStringLiteral("orange"), {"#fff4e6", "#ffe8cc", "#ffd8a8", "#ffc078", "#ffa94d", "#ff922b", "#fd7e14", "#f76707", "#e8590c", "#d9480f"});
    color_libraries_.push_back(open_color);

    QFile file(editor_color_libraries_path());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        const QJsonArray palettes = doc.object().value(QStringLiteral("palettes")).toArray();
        for (const QJsonValue &value : palettes) {
            const QJsonObject object = value.toObject();
            ColorLibrary library;
            library.name = object.value(QStringLiteral("name")).toString().trimmed();
            library.slug = object.value(QStringLiteral("slug")).toString().trimmed();
            library.built_in = false;
            if (library.name.isEmpty())
                continue;
            if (library.slug.isEmpty())
                library.slug = editor_slugify(library.name);

            const QJsonArray colors = object.value(QStringLiteral("colors")).toArray();
            for (int i = 0; i < colors.size(); ++i) {
                const QJsonObject color_object = colors[i].toObject();
                const QString hex = color_object.value(QStringLiteral("hex")).toString();
                QColor color;
                if (!editor_parse_recent_color_hex(hex, color))
                    continue;
                const QString color_name = color_object.value(QStringLiteral("name")).toString(
                    QStringLiteral("%1 %2").arg(library.name).arg(i + 1));
                library.colors.push_back({color_name, color});
            }
            color_libraries_.push_back(library);
        }
    }

    restore_selected_color_library();
    selected_color_library_index_ = std::clamp(selected_color_library_index_, 0, (int)color_libraries_.size() - 1);
}

void TitleEditor::save_color_libraries() const
{
    QJsonArray palettes;
    for (const ColorLibrary &library : color_libraries_) {
        if (library.built_in)
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("name"), library.name);
        object.insert(QStringLiteral("slug"), library.slug.isEmpty() ? editor_slugify(library.name) : library.slug);
        object.insert(QStringLiteral("type"), QStringLiteral("categorical"));
        QJsonArray colors;
        for (const ColorLibraryColor &entry : library.colors) {
            QJsonObject color;
            if (!entry.name.isEmpty())
                color.insert(QStringLiteral("name"), entry.name);
            color.insert(QStringLiteral("hex"), editor_recent_color_hex(entry.color));
            colors.append(color);
        }
        object.insert(QStringLiteral("colors"), colors);
        palettes.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("palettes"), palettes);
    QFile file(editor_color_libraries_path());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void TitleEditor::restore_selected_color_library()
{
    const QString saved_slug = obsgs_load_selected_color_library_slug().trimmed();
    if (saved_slug.isEmpty())
        return;

    for (int i = 0; i < (int)color_libraries_.size(); ++i) {
        if (color_libraries_[i].slug.compare(saved_slug, Qt::CaseInsensitive) == 0) {
            selected_color_library_index_ = i;
            return;
        }
    }
}

void TitleEditor::persist_selected_color_library() const
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    const QString slug = color_libraries_[selected_color_library_index_].slug.trimmed();
    if (!slug.isEmpty())
        obsgs_save_selected_color_library_slug(slug);
}

void TitleEditor::refresh_color_library_controls()
{
    if (!color_library_combo_)
        return;

    QSignalBlocker blocker(color_library_combo_);
    color_library_combo_->clear();
    for (const ColorLibrary &library : color_libraries_)
        color_library_combo_->addItem(library.name, library.slug);
    selected_color_library_index_ = std::clamp(selected_color_library_index_, 0, std::max(0, (int)color_libraries_.size() - 1));
    color_library_combo_->setCurrentIndex(selected_color_library_index_);

    const bool editable = selected_color_library_index_ >= 0 &&
        selected_color_library_index_ < (int)color_libraries_.size() &&
        !color_libraries_[selected_color_library_index_].built_in;
    if (color_library_rename_button_)
        color_library_rename_button_->setEnabled(editable);
    if (color_library_delete_button_)
        color_library_delete_button_->setEnabled(editable);
    refresh_color_library_swatches();
}

void TitleEditor::refresh_color_library_swatches()
{
    if (!color_library_swatches_grid_ || !color_library_swatch_widget_)
        return;

    while (QLayoutItem *item = color_library_swatches_grid_->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;

    const auto &colors = color_libraries_[selected_color_library_index_].colors;
    const int columns = 8;
    for (int i = 0; i < (int)colors.size(); ++i) {
        const ColorLibraryColor entry = colors[i];
        auto *swatch = new QToolButton(color_library_swatch_widget_);
        swatch->setObjectName(QStringLiteral("OBSGraphicsStudioProLibraryColorSwatch"));
        swatch->setFixedSize(24, 24);
        swatch->setAutoRaise(false);
        swatch->setToolTip(obsgs_color_swatch_tooltip(entry.name, entry.color, editor_recent_color_hex(entry.color)));
        swatch->setStyleSheet(editor_color_swatch_style(entry.color));
        connect(swatch, &QToolButton::clicked, this, [this, color = entry.color]() {
            apply_picked_color_to_selection(color);
            remember_recent_color(color);
        });
        swatch->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(swatch, &QToolButton::customContextMenuRequested, this, [this, swatch, i](const QPoint &pos) {
            if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
                return;
            if (color_libraries_[selected_color_library_index_].built_in)
                return;
            QMenu menu(swatch);
            QAction *remove_action = menu.addAction(obsgs_tr("OBSTitles.Delete"));
            if (menu.exec(swatch->mapToGlobal(pos)) == remove_action)
                remove_color_from_current_library(i);
        });
        color_library_swatches_grid_->addWidget(swatch, i / columns, i % columns);
    }
}

void TitleEditor::create_color_library()
{
    prompt_create_color_library();
}

int TitleEditor::prompt_create_color_library()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, obsgs_tr("OBSTitles.ColorLibraries"),
                                               obsgs_tr("OBSTitles.Name"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return -1;

    ColorLibrary library;
    library.name = name;
    library.slug = editor_slugify(name);
    int suffix = 2;
    auto slug_exists = [&]() {
        return std::any_of(color_libraries_.begin(), color_libraries_.end(), [&](const ColorLibrary &candidate) {
            return candidate.slug.compare(library.slug, Qt::CaseInsensitive) == 0;
        });
    };
    const QString base_slug = library.slug;
    while (slug_exists())
        library.slug = QStringLiteral("%1-%2").arg(base_slug).arg(suffix++);
    color_libraries_.push_back(library);
    selected_color_library_index_ = (int)color_libraries_.size() - 1;
    save_color_libraries();
    persist_selected_color_library();
    refresh_color_library_controls();
    return selected_color_library_index_;
}

void TitleEditor::rename_current_color_library()
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    ColorLibrary &library = color_libraries_[selected_color_library_index_];
    if (library.built_in)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, obsgs_tr("OBSTitles.ColorLibraries"),
                                               obsgs_tr("OBSTitles.Name"), QLineEdit::Normal,
                                               library.name, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    library.name = name;
    const QString base_slug = editor_slugify(name);
    library.slug = base_slug;
    int suffix = 2;
    auto slug_exists = [&]() {
        for (int i = 0; i < (int)color_libraries_.size(); ++i) {
            if (i == selected_color_library_index_)
                continue;
            if (color_libraries_[i].slug.compare(library.slug, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    while (slug_exists())
        library.slug = QStringLiteral("%1-%2").arg(base_slug).arg(suffix++);
    save_color_libraries();
    persist_selected_color_library();
    refresh_color_library_controls();
}

void TitleEditor::delete_current_color_library()
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    if (color_libraries_[selected_color_library_index_].built_in)
        return;

    if (QMessageBox::question(this, obsgs_tr("OBSTitles.ColorLibraries"),
                              obsgs_tr("OBSTitles.DeleteColorLibraryQuestionFormat").arg(color_libraries_[selected_color_library_index_].name)) != QMessageBox::Yes)
        return;
    color_libraries_.erase(color_libraries_.begin() + selected_color_library_index_);
    selected_color_library_index_ = std::clamp(selected_color_library_index_, 0, std::max(0, (int)color_libraries_.size() - 1));
    save_color_libraries();
    persist_selected_color_library();
    refresh_color_library_controls();
}

void TitleEditor::add_color_to_current_library(const QColor &color)
{
    if (!color.isValid() || selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    add_color_to_library(selected_color_library_index_, color, editor_recent_color_hex(color));
}

bool TitleEditor::show_add_color_to_library_dialog(const QColor &color)
{
    if (!color.isValid() || color.alpha() <= 0)
        return false;
    load_color_libraries();

    QDialog dialog(this);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.AddToLibrary"));
    auto *root = new QHBoxLayout(&dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto *left = new QWidget(&dialog);
    auto *left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(8);

    auto *swatch = new QLabel(left);
    swatch->setFixedSize(96, 96);
    swatch->setStyleSheet(QStringLiteral("QLabel{background:%1;border:1px solid %2;border-radius:4px;}")
                               .arg(color.name(QColor::HexArgb),
                                    qApp->palette().color(QPalette::Mid).name(QColor::HexRgb)));
    swatch->setToolTip(obsgs_color_swatch_tooltip(editor_recent_color_hex(color), color, editor_recent_color_hex(color)));
    auto *name_edit = new QLineEdit(editor_recent_color_hex(color), left);
    auto *hex_label = new QLabel(editor_recent_color_hex(color), left);
    hex_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left_layout->addWidget(swatch, 0, Qt::AlignLeft);
    left_layout->addWidget(name_edit);
    left_layout->addWidget(hex_label);
    left_layout->addStretch(1);

    auto *right = new QWidget(&dialog);
    auto *right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(8);
    auto *library_list = new QListWidget(right);
    library_list->addItem(obsgs_tr("OBSTitles.AddNewLibrary"));
    for (int i = 0; i < (int)color_libraries_.size(); ++i) {
        if (color_libraries_[i].built_in)
            continue;
        auto *item = new QListWidgetItem(color_libraries_[i].name, library_list);
        item->setData(Qt::UserRole, i);
    }
    library_list->setCurrentRow(library_list->count() > 1 ? 1 : 0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, right);
    right_layout->addWidget(library_list, 1);
    right_layout->addWidget(buttons);

    root->addWidget(left);
    root->addWidget(right, 1);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    int library_index = -1;
    if (library_list->currentRow() == 0) {
        library_index = prompt_create_color_library();
    } else if (auto *item = library_list->currentItem()) {
        library_index = item->data(Qt::UserRole).toInt();
    }
    if (library_index < 0)
        return false;

    add_color_to_library(library_index, color, name_edit->text().trimmed());
    selected_color_library_index_ = library_index;
    persist_selected_color_library();
    refresh_color_library_controls();
    return true;
}

void TitleEditor::add_color_to_library(int library_index, const QColor &color, const QString &name)
{
    if (!color.isValid() || library_index < 0 || library_index >= (int)color_libraries_.size())
        return;
    ColorLibrary &library = color_libraries_[library_index];
    if (library.built_in)
        return;

    const QString hex = editor_recent_color_hex(color);
    library.colors.erase(std::remove_if(library.colors.begin(), library.colors.end(), [&](const ColorLibraryColor &entry) {
        return editor_recent_color_hex(entry.color).compare(hex, Qt::CaseInsensitive) == 0;
    }), library.colors.end());
    library.colors.push_back({name.trimmed().isEmpty() ? hex : name.trimmed(), color});
    save_color_libraries();
    refresh_color_library_swatches();
}

void TitleEditor::remove_color_from_current_library(int index)
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    ColorLibrary &library = color_libraries_[selected_color_library_index_];
    if (library.built_in || index < 0 || index >= (int)library.colors.size())
        return;

    library.colors.erase(library.colors.begin() + index);
    save_color_libraries();
    refresh_color_library_swatches();
}

void TitleEditor::refresh_color_swatches_panel()
{
    QStringList recent_hexes = obsgs_load_recent_color_hexes();
    if (title_) {
        for (const auto &hex : title_->editor_recent_color_hexes) {
            const QString value = QString::fromStdString(hex);
            if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                recent_hexes << value;
        }
    }
    for (int i = 0; i < (int)recent_color_swatch_buttons_.size(); ++i) {
        auto *button = recent_color_swatch_buttons_[i];
        if (!button)
            continue;

        QColor color;
        const bool has_color = i < recent_hexes.size() && editor_parse_recent_color_hex(recent_hexes[i], color);
        button->setEnabled(has_color);
        button->setToolTip(has_color ? obsgs_color_swatch_tooltip(obsgs_tr("OBSTitles.RecentColors"), color, editor_recent_color_hex(color)) : QString());
        button->setStyleSheet(editor_color_swatch_style(color, has_color));
    }
}

void TitleEditor::create_docked_panel_menu(QMenuBar *menu_bar)
{
    if (!menu_bar) return;

    auto *windows_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.Windows"));

    act_lock_panels_ = windows_menu->addAction(obsgs_tr("OBSTitles.LockPanels"));
    act_lock_panels_->setCheckable(true);
    connect(act_lock_panels_, &QAction::toggled, this, &TitleEditor::set_panels_locked);

    QAction *reset_layout_action = windows_menu->addAction(obsgs_tr("OBSTitles.ResetDefaultLayout"));
    connect(reset_layout_action, &QAction::triggered, this, &TitleEditor::reset_default_layout);

    windows_menu->addSeparator();
    act_tools_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.Tools"));
    act_tools_visible_->setCheckable(true);
    act_tools_visible_->setChecked(true);
    connect(act_tools_visible_, &QAction::triggered, this, [this](bool visible) {
        if (tools_dock_) tools_dock_->setVisible(visible);
    });

    act_graphic_props_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.GraphicProperties"));
    act_graphic_props_visible_->setCheckable(true);
    act_graphic_props_visible_->setChecked(true);
    connect(act_graphic_props_visible_, &QAction::triggered, this, [this](bool visible) {
        if (graphic_props_dock_) graphic_props_dock_->setVisible(visible);
    });

    act_layer_props_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.LayerProperties"));
    act_layer_props_visible_->setCheckable(true);
    act_layer_props_visible_->setChecked(true);
    connect(act_layer_props_visible_, &QAction::triggered, this, [this](bool visible) {
        if (layer_props_dock_) layer_props_dock_->setVisible(visible);
    });

    act_effects_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.Effects"));
    act_effects_visible_->setCheckable(true);
    act_effects_visible_->setChecked(true);
    connect(act_effects_visible_, &QAction::triggered, this, [this](bool visible) {
        if (effects_dock_) effects_dock_->setVisible(visible);
    });

    act_styles_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.Styles"));
    act_styles_visible_->setCheckable(true);
    act_styles_visible_->setChecked(true);
    connect(act_styles_visible_, &QAction::triggered, this, [this](bool visible) {
        if (styles_dock_) styles_dock_->setVisible(visible);
    });

    act_color_swatches_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.ColorSwatches"));
    act_color_swatches_visible_->setCheckable(true);
    act_color_swatches_visible_->setChecked(true);
    connect(act_color_swatches_visible_, &QAction::triggered, this, [this](bool visible) {
        if (color_swatches_dock_) color_swatches_dock_->setVisible(visible);
    });

    act_timeline_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.Timeline"));
    act_timeline_visible_->setCheckable(true);
    act_timeline_visible_->setChecked(true);
    connect(act_timeline_visible_, &QAction::triggered, this, [this](bool visible) {
        if (timeline_dock_) timeline_dock_->setVisible(visible);
    });

    act_prerender_visible_ = windows_menu->addAction(obsgs_tr("OBSTitles.PlaybackAndCache"));
    act_prerender_visible_->setCheckable(true);
    act_prerender_visible_->setChecked(true);
    connect(act_prerender_visible_, &QAction::triggered, this, [this](bool visible) {
        if (prerender_dock_) prerender_dock_->setVisible(visible);
    });
}

QDockWidget *TitleEditor::create_editor_dock(const QString &object_name, const QString &title, QWidget *panel)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(object_name);
    const bool timeline_dock = object_name == QString::fromUtf8(kTimelineDockObjectName);
    dock->setAllowedAreas(timeline_dock
                              ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                              : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
    dock->setWidget(panel);
    dock->setFeatures(QDockWidget::DockWidgetClosable |
                      QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setMinimumWidth(panel ? panel->minimumWidth() : 220);
    if (timeline_dock)
        dock->setMinimumHeight(panel ? panel->minimumHeight() : 180);

    QAction *visibility_action = nullptr;
    if (object_name == QString::fromUtf8(kGraphicPropertiesDockObjectName))
        visibility_action = act_graphic_props_visible_;
    else if (object_name == QString::fromUtf8(kLayerPropertiesDockObjectName))
        visibility_action = act_layer_props_visible_;
    else if (object_name == QString::fromUtf8(kEffectsDockObjectName))
        visibility_action = act_effects_visible_;
    else if (object_name == QString::fromUtf8(kStylesDockObjectName))
        visibility_action = act_styles_visible_;
    else if (object_name == QString::fromUtf8(kColorSwatchesDockObjectName))
        visibility_action = act_color_swatches_visible_;
    else if (object_name == QString::fromUtf8(kTimelineDockObjectName))
        visibility_action = act_timeline_visible_;
    else if (object_name == QString::fromUtf8(kPrerenderDockObjectName))
        visibility_action = act_prerender_visible_;
    else if (object_name == QStringLiteral("OBSGraphicsStudioProToolsDock"))
        visibility_action = act_tools_visible_;

    if (visibility_action) {
        connect(dock, &QDockWidget::visibilityChanged, this, [visibility_action](bool visible) {
            QSignalBlocker blocker(visibility_action);
            visibility_action->setChecked(visible);
        });
    }

    connect(dock, &QDockWidget::topLevelChanged, this, [this]() { save_editor_layout(); });
    connect(dock, &QDockWidget::dockLocationChanged, this, [this]() { save_editor_layout(); });
    connect(dock, &QDockWidget::visibilityChanged, this, [this]() { save_editor_layout(); });
    return dock;
}

void TitleEditor::load_editor_layout()
{
    restoring_editor_layout_ = true;

    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));

    const QByteArray geometry = settings.value(QString::fromUtf8(kEditorGeometryKey)).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    panels_locked_ = settings.value(QString::fromUtf8(kEditorPanelsLockedKey), panels_locked_).toBool();
    if (act_lock_panels_) {
        QSignalBlocker blocker(act_lock_panels_);
        act_lock_panels_->setChecked(panels_locked_);
    }

    const QByteArray window_state = settings.value(QString::fromUtf8(kEditorWindowStateKey)).toByteArray();
    if (!window_state.isEmpty())
        restoreState(window_state);

    settings.endGroup();

    if (act_tools_visible_ && tools_dock_) {
        QSignalBlocker blocker(act_tools_visible_);
        act_tools_visible_->setChecked(!tools_dock_->isHidden());
    }
    if (act_graphic_props_visible_ && graphic_props_dock_) {
        QSignalBlocker blocker(act_graphic_props_visible_);
        act_graphic_props_visible_->setChecked(!graphic_props_dock_->isHidden());
    }
    if (act_layer_props_visible_ && layer_props_dock_) {
        QSignalBlocker blocker(act_layer_props_visible_);
        act_layer_props_visible_->setChecked(!layer_props_dock_->isHidden());
    }
    if (act_effects_visible_ && effects_dock_) {
        QSignalBlocker blocker(act_effects_visible_);
        act_effects_visible_->setChecked(!effects_dock_->isHidden());
    }
    if (act_styles_visible_ && styles_dock_) {
        QSignalBlocker blocker(act_styles_visible_);
        act_styles_visible_->setChecked(!styles_dock_->isHidden());
    }
    if (act_color_swatches_visible_ && color_swatches_dock_) {
        QSignalBlocker blocker(act_color_swatches_visible_);
        act_color_swatches_visible_->setChecked(!color_swatches_dock_->isHidden());
    }
    if (act_timeline_visible_ && timeline_dock_) {
        QSignalBlocker blocker(act_timeline_visible_);
        act_timeline_visible_->setChecked(!timeline_dock_->isHidden());
    }
    if (act_prerender_visible_ && prerender_dock_) {
        QSignalBlocker blocker(act_prerender_visible_);
        act_prerender_visible_->setChecked(!prerender_dock_->isHidden());
    }

    restoring_editor_layout_ = false;
    update_panel_lock_state();
}

void TitleEditor::save_editor_layout() const
{
    if (restoring_editor_layout_ || editor_layout_save_suppressed_)
        return;

    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorGeometryKey), saveGeometry());
    settings.setValue(QString::fromUtf8(kEditorWindowStateKey), saveState());
    settings.setValue(QString::fromUtf8(kEditorPanelsLockedKey), panels_locked_);
    settings.endGroup();
    settings.sync();
}

void TitleEditor::reset_default_layout()
{
    restoring_editor_layout_ = true;

    const QDockWidget::DockWidgetFeatures reset_features =
        QDockWidget::DockWidgetClosable |
        QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable;
    for (auto *dock : {tools_dock_, graphic_props_dock_, layer_props_dock_, effects_dock_, styles_dock_, color_swatches_dock_, timeline_dock_, prerender_dock_}) {
        if (!dock) continue;
        dock->setMaximumWidth(dock == tools_dock_ ? 64 : QWIDGETSIZE_MAX);
        dock->setMinimumWidth(dock == tools_dock_ ? 46 : (dock->widget() ? dock->widget()->minimumWidth() : 220));
        dock->setFeatures(reset_features);
        dock->setAllowedAreas(dock == timeline_dock_
                                  ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                                  : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
    }

    if (tools_dock_) {
        tools_dock_->setFloating(false);
        tools_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, tools_dock_);
    }
    if (graphic_props_dock_) {
        graphic_props_dock_->setFloating(false);
        graphic_props_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, graphic_props_dock_);
    }
    if (layer_props_dock_) {
        layer_props_dock_->setFloating(false);
        layer_props_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, layer_props_dock_);
        if (tools_dock_) splitDockWidget(tools_dock_, layer_props_dock_, Qt::Horizontal);
    }
    if (styles_dock_) {
        styles_dock_->setFloating(false);
        styles_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, styles_dock_);
        if (graphic_props_dock_) splitDockWidget(graphic_props_dock_, styles_dock_, Qt::Horizontal);
    }
    if (color_swatches_dock_) {
        color_swatches_dock_->setFloating(false);
        color_swatches_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, color_swatches_dock_);
        if (styles_dock_) tabifyDockWidget(styles_dock_, color_swatches_dock_);
        else if (graphic_props_dock_) splitDockWidget(graphic_props_dock_, color_swatches_dock_, Qt::Horizontal);
    }
    if (effects_dock_) {
        effects_dock_->setFloating(false);
        effects_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
        if (layer_props_dock_) splitDockWidget(layer_props_dock_, effects_dock_, Qt::Horizontal);
    }
    if (timeline_dock_) {
        timeline_dock_->setFloating(false);
        timeline_dock_->show();
        addDockWidget(Qt::BottomDockWidgetArea, timeline_dock_);
    }
    if (prerender_dock_) {
        prerender_dock_->setFloating(false);
        prerender_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, prerender_dock_);
        if (effects_dock_) tabifyDockWidget(effects_dock_, prerender_dock_);
    }
    if (tools_dock_) tools_dock_->raise();
    if (graphic_props_dock_) graphic_props_dock_->raise();
    if (layer_props_dock_) layer_props_dock_->raise();
    if (styles_dock_) styles_dock_->raise();

    if (act_tools_visible_) {
        QSignalBlocker blocker(act_tools_visible_);
        act_tools_visible_->setChecked(true);
    }
    if (act_graphic_props_visible_) {
        QSignalBlocker blocker(act_graphic_props_visible_);
        act_graphic_props_visible_->setChecked(true);
    }
    if (act_layer_props_visible_) {
        QSignalBlocker blocker(act_layer_props_visible_);
        act_layer_props_visible_->setChecked(true);
    }
    if (act_effects_visible_) {
        QSignalBlocker blocker(act_effects_visible_);
        act_effects_visible_->setChecked(true);
    }
    if (act_styles_visible_) {
        QSignalBlocker blocker(act_styles_visible_);
        act_styles_visible_->setChecked(true);
    }
    if (act_color_swatches_visible_) {
        QSignalBlocker blocker(act_color_swatches_visible_);
        act_color_swatches_visible_->setChecked(true);
    }
    if (act_timeline_visible_) {
        QSignalBlocker blocker(act_timeline_visible_);
        act_timeline_visible_->setChecked(true);
    }
    if (act_prerender_visible_) {
        QSignalBlocker blocker(act_prerender_visible_);
        act_prerender_visible_->setChecked(true);
    }

    resize(1280, 760);
    update_panel_lock_state();
    restoring_editor_layout_ = false;
    save_editor_layout();
}

void TitleEditor::set_panels_locked(bool locked)
{
    panels_locked_ = locked;
    update_panel_lock_state();
    save_editor_layout();
}

void TitleEditor::update_panel_lock_state()
{
    const QDockWidget::DockWidgetFeatures unlocked_features =
        QDockWidget::DockWidgetClosable |
        QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable;
    const QDockWidget::DockWidgetFeatures locked_features = QDockWidget::DockWidgetClosable;

    for (auto *dock : {tools_dock_, graphic_props_dock_, layer_props_dock_, effects_dock_, styles_dock_, color_swatches_dock_, timeline_dock_, prerender_dock_}) {
        if (!dock) continue;
        dock->setFeatures(panels_locked_ ? locked_features : unlocked_features);
        dock->setAllowedAreas(panels_locked_ ? Qt::NoDockWidgetArea
                                             : (dock == timeline_dock_
                                                    ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                                                    : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea)));
        if (panels_locked_) {
            const int locked_width = std::max(dock->minimumWidth(), dock->width());
            if (dock == timeline_dock_) {
                const int locked_height = std::max(dock->minimumHeight(), dock->height());
                dock->setMinimumHeight(locked_height);
                dock->setMaximumHeight(locked_height);
            } else {
                dock->setMinimumWidth(locked_width);
                dock->setMaximumWidth(locked_width);
            }
        } else {
            dock->setMinimumWidth(dock == tools_dock_ ? 46 : (dock->widget() ? dock->widget()->minimumWidth() : 220));
            dock->setMaximumWidth(dock == tools_dock_ ? 64 : QWIDGETSIZE_MAX);
            if (dock == timeline_dock_) {
                dock->setMinimumHeight(dock->widget() ? dock->widget()->minimumHeight() : 180);
                dock->setMaximumHeight(QWIDGETSIZE_MAX);
            }
        }
    }
}


static bool text_has_rtl_direction(const QString &text, bool fallback_rtl = false)
{
    for (const QChar ch : text) {
        switch (ch.direction()) {
        case QChar::DirL:
        case QChar::DirLRE:
        case QChar::DirLRO:
            return false;
        case QChar::DirR:
        case QChar::DirAL:
        case QChar::DirRLE:
        case QChar::DirRLO:
            return true;
        default:
            break;
        }
    }
    return fallback_rtl;
}

static void set_text_layer_direction_defaults(Layer &layer, bool paragraph_box, bool rtl)
{
    layer.align_h = rtl ? 2 : 0;
    layer.align_v = 0;
    layer.origin_x = rtl ? 1.0f : 0.0f;
    layer.origin_y = 0.0f;
    layer.origin_prop.static_value.x = layer.origin_x;
    layer.origin_prop.static_value.y = layer.origin_y;
    layer.text_overflow_mode = paragraph_box ? 0 : 1;
    layer.text_box_width_to_text = !paragraph_box;
    layer.text_box_height_to_text = !paragraph_box;
    layer.max_text_box_width = 9999.0f;
    layer.max_text_box_height = 9999.0f;
    layer.rich_text.default_paragraph_format.align_h = layer.align_h;
    layer.rich_text.default_paragraph_format.align_v = layer.align_v;
    for (auto &block : layer.rich_text.blocks) {
        block.format.align_h = layer.align_h;
        block.format.align_v = layer.align_v;
    }
}

static void set_new_text_layer_contents_empty(Layer &layer)
{
    layer.text_content.clear();
    layer.rich_text_html.clear();
    layer.rich_text = rich_text_document_from_layer_defaults(layer);
    layer.rich_text.plain_text.clear();
    layer.rich_text.ranges.clear();
    layer.rich_text.blocks.clear();
    layer.rich_text.selection = {0, 0};
}

static bool text_has_visible_name_content(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.isSpace()) continue;
        const QChar::Category category = ch.category();
        if (category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing ||
            category == QChar::Other_Format ||
            category == QChar::Other_Control)
            continue;
        return true;
    }
    return false;
}

std::shared_ptr<Layer> TitleEditor::create_basic_layer(LayerType type, const QString &name_override)
{
    if (!title_) return nullptr;

    auto l = std::make_shared<Layer>();
    l->id = TitleDataStore::make_uuid();
    if (!name_override.isEmpty()) {
        l->name = name_override.toStdString();
    } else {
        l->name = (type == LayerType::Text) ? editor_text_std("OBSTitles.Text") :
                  (type == LayerType::Clock) ? editor_text_std("OBSTitles.Clock") :
                  (type == LayerType::Ticker) ? editor_text_std("OBSTitles.Ticker") :
                  (type == LayerType::Image) ? editor_text_std("OBSTitles.Image") : editor_text_std("OBSTitles.Shape");
    }
    l->name = unique_layer_name(l->name);
    l->type = type;
    l->text_content = (type == LayerType::Text) ? editor_text_std("OBSTitles.NewText") :
                      (type == LayerType::Ticker) ? editor_text_std("OBSTitles.NewTickerText") : "";
    l->rich_text = rich_text_document_from_layer_defaults(*l);
    if (type == LayerType::Text) {
        set_new_text_layer_contents_empty(*l);
        set_text_layer_direction_defaults(*l, false, false);
    }
    l->clock_format = (type == LayerType::Clock) ? "H:i:s" : l->clock_format;
    l->position.static_value.x = title_->width / 2.0;
    l->position.static_value.y = title_->height / 2.0;
    l->rect_width = title_->width * 0.5f;
    l->rect_height = (type == LayerType::Image) ? title_->height * 0.4f : 160.0f;
    l->size.static_value.x = l->rect_width;
    l->size.static_value.y = l->rect_height;
    if (type == LayerType::Image) {
        l->image_width = l->rect_width;
        l->image_height = l->rect_height;
        l->image_size.static_value.x = l->image_width;
        l->image_size.static_value.y = l->image_height;
    }
    if (type == LayerType::Shape || type == LayerType::SolidRect)
        l->lock_aspect_ratio = false;
    l->origin_prop.static_value.x = l->origin_x;
    l->origin_prop.static_value.y = l->origin_y;
    apply_new_layer_defaults(*l);
    set_channel_statics(*l, true, l->text_color);
    set_channel_statics(*l, false, l->fill_color);
    l->out_time = title_->duration;
    return l;
}

void TitleEditor::create_shape_layer_from_canvas(ShapeType shape_type, const QPointF &canvas_pt)
{
    if (!title_) return;

    auto layer = create_basic_layer(LayerType::Shape, shape_display_name(shape_type));
    if (!layer) return;
    layer->shape_type = shape_type;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    if (shape_type == ShapeType::Ellipse || shape_type == ShapeType::Triangle ||
        shape_type == ShapeType::Star || shape_type == ShapeType::Polygon ||
        shape_type == ShapeType::Diamond) {
        const float size = std::min(layer->rect_width, layer->rect_height);
        layer->rect_width = size;
        layer->rect_height = size;
        layer->size.static_value.x = layer->rect_width;
        layer->size.static_value.y = layer->rect_height;
    }
    if (shape_type == ShapeType::RoundedRectangle) {
        set_layer_all_corner_radii(*layer, 18.0f);
        layer->corner_radius_locked = true;
    }

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
}


void TitleEditor::create_pen_path_layer_from_canvas(const std::vector<BezierPathPoint> &canvas_points, bool closed)
{
    if (!title_ || canvas_points.size() < 2)
        return;

    QPainterPath canvas_path;
    canvas_path.moveTo(canvas_points.front().x, canvas_points.front().y);
    const size_t segment_count = closed ? canvas_points.size() : canvas_points.size() - 1;
    for (size_t i = 0; i < segment_count; ++i) {
        const size_t next_index = (i + 1) % canvas_points.size();
        const BezierPathPoint &from = canvas_points[i];
        const BezierPathPoint &to = canvas_points[next_index];
        const QPointF from_anchor(from.x, from.y);
        const QPointF to_anchor(to.x, to.y);
        const QPointF control1 = from.has_out ? QPointF(from.out_x, from.out_y) : from_anchor;
        const QPointF control2 = to.has_in ? QPointF(to.in_x, to.in_y) : to_anchor;
        if (from.has_out || to.has_in)
            canvas_path.cubicTo(control1, control2, to_anchor);
        else
            canvas_path.lineTo(to_anchor);
    }
    if (closed)
        canvas_path.closeSubpath();

    QRectF bounds = canvas_path.boundingRect();
    if (bounds.width() < 1.0) {
        const double center_x = bounds.center().x();
        bounds.setLeft(center_x - 0.5);
        bounds.setWidth(1.0);
    }
    if (bounds.height() < 1.0) {
        const double center_y = bounds.center().y();
        bounds.setTop(center_y - 0.5);
        bounds.setHeight(1.0);
    }
    const double width = bounds.width();
    const double height = bounds.height();

    auto layer = create_basic_layer(LayerType::Shape, obsgs_tr("OBSTitles.Path"));
    if (!layer)
        return;
    layer->shape_type = ShapeType::Path;
    layer->path_closed = closed;
    layer->path_points = canvas_points;
    layer->position.static_value.x = bounds.center().x();
    layer->position.static_value.y = bounds.center().y();
    layer->rect_width = (float)width;
    layer->rect_height = (float)height;
    layer->size.static_value.x = width;
    layer->size.static_value.y = height;
    layer->origin_x = 0.5f;
    layer->origin_y = 0.5f;
    layer->origin_prop.static_value = {0.5, 0.5};

    auto normalize = [&](double x, double y) {
        return QPointF((x - bounds.left()) / width,
                       (y - bounds.top()) / height);
    };
    for (auto &point : layer->path_points) {
        const QPointF anchor = normalize(point.x, point.y);
        const QPointF in_handle = point.has_in ? normalize(point.in_x, point.in_y) : anchor;
        const QPointF out_handle = point.has_out ? normalize(point.out_x, point.out_y) : anchor;
        point.x = anchor.x(); point.y = anchor.y();
        point.in_x = in_handle.x(); point.in_y = in_handle.y();
        point.out_x = out_handle.x(); point.out_y = out_handle.y();
    }

    title_->add_layer(layer);
    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer->id);
    force_next_title_visual_update();
    on_title_modified();
}


void TitleEditor::create_text_layer_from_canvas(LayerType type, const QPointF &canvas_pt)
{
    if (!title_) return;
    if (type != LayerType::Text && type != LayerType::Clock && type != LayerType::Ticker)
        type = LayerType::Text;

    auto layer = create_basic_layer(type, text_tool_display_name(type));
    if (!layer) return;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    if (type == LayerType::Text) {
        set_new_text_layer_contents_empty(*layer);
        set_text_layer_direction_defaults(*layer, false, false);
    } else {
        layer->rich_text_html.clear();
    }

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    if (type == LayerType::Text)
        pending_text_layer_auto_names_.insert(layer->id);
    layers_->refresh();
    on_layer_selected(layer->id);
}



void TitleEditor::create_image_layer_from_canvas(const QPointF &canvas_pt)
{
    if (!title_) return;

    auto layer = create_basic_layer(LayerType::Image, obsgs_tr("OBSTitles.Image"));
    if (!layer) return;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    layer->image_path.clear();
    layer->image_size_auto_fit = true;
    layer->lock_aspect_ratio = true;

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
}

void TitleEditor::choose_image_file_for_layer(const std::string &layer_id)
{
    if (!title_ || layer_id.empty()) return;
    auto layer = title_->find_layer(layer_id);
    if (!layer || layer->type != LayerType::Image) return;

    const QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ChooseImage"), QString(),
        obsgs_tr("OBSTitles.ImageFileFilter"));
    if (path.isEmpty()) {
        // Keep the empty image layer as a usable canvas placeholder.
        force_next_title_visual_update();
        if (canvas_) canvas_->refresh_preview();
        return;
    }

    gsp::apply_exposed_image_cue_value(*layer, path.toStdString());
    layer->image_path = path.toStdString();
    layer->name = unique_layer_name(QFileInfo(path).completeBaseName().toStdString(), {layer_id});
    layers_->refresh();
    on_layer_selected(layer_id);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::create_image_layer_from_external_source(const QString &image_path, const QPointF &canvas_pt)
{
    if (!title_ || image_path.trimmed().isEmpty())
        return;

    auto layer = create_basic_layer(LayerType::Image, QFileInfo(image_path).completeBaseName());
    if (!layer)
        return;

    layer->image_path = image_path.toStdString();
    layer->lock_aspect_ratio = true;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();

    QSize image_size = editor_image_intrinsic_size(image_path);
    if (image_size.isValid() && !image_size.isEmpty()) {
        layer->image_width = (float)image_size.width();
        layer->image_height = (float)image_size.height();
        layer->image_size.static_value.x = layer->image_width;
        layer->image_size.static_value.y = layer->image_height;
        const double max_w = title_->width * 0.65;
        const double max_h = title_->height * 0.65;
        const double scale = std::min({1.0, max_w / std::max(1, image_size.width()), max_h / std::max(1, image_size.height())});
        layer->rect_width = (float)std::max(1.0, image_size.width() * scale);
        layer->rect_height = (float)std::max(1.0, image_size.height() * scale);
        layer->size.static_value.x = layer->rect_width;
        layer->size.static_value.y = layer->rect_height;
    }

    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::create_text_layer_from_external_source(const QString &text, const QPointF &canvas_pt)
{
    if (!title_ || text.isEmpty())
        return;

    const QString normalized = text.left(20000);
    auto layer = create_basic_layer(LayerType::Text, QString());
    if (!layer)
        return;

    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    layer->text_content = normalized.toStdString();
    layer->rich_text = rich_text_document_from_layer_defaults(*layer);
    rich_text_document_replace_text(layer->rich_text, layer->text_content);
    rich_text_document_sync_layer_mirrors(*layer);
    set_text_layer_direction_defaults(*layer, normalized.contains(QLatin1Char('\n')), text_has_rtl_direction(normalized));
    layer->name = unique_layer_name(normalized.simplified().left(40).toStdString());

    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::update_canvas_created_shape(const QRectF &canvas_rect)
{
    if (!title_ || canvas_created_shape_layer_id_.empty()) return;
    auto layer = title_->find_layer(canvas_created_shape_layer_id_);
    if (!layer) return;

    QRectF rect = canvas_rect.normalized();
    const double width = std::max(1.0, rect.width());
    const double height = std::max(1.0, rect.height());
    if (rect.width() < 1.0) rect.setWidth(width);
    if (rect.height() < 1.0) rect.setHeight(height);

    if (is_canvas_text_layer(*layer)) {
        const bool rtl = text_has_rtl_direction(QString::fromStdString(layer->text_content), layer->origin_x > 0.5f);
        set_text_layer_direction_defaults(*layer, true, rtl);
        layer->position.static_value.x = rtl ? rect.right() : rect.left();
        layer->position.static_value.y = rect.top();
    } else {
        layer->position.static_value.x = rect.center().x();
        layer->position.static_value.y = rect.center().y();
    }
    layer->rect_width = (float)width;
    layer->rect_height = (float)height;
    layer->size.static_value.x = layer->rect_width;
    layer->size.static_value.y = layer->rect_height;
    if (!is_canvas_text_layer(*layer) && layer->shape_type == ShapeType::RoundedRectangle) {
        set_layer_all_corner_radii(*layer, (float)std::min(width, height) * 0.12f);
        layer->corner_radius_locked = true;
    }

    update_layer_panels(layer, playhead_);
}

void TitleEditor::finish_canvas_created_shape(bool keep_layer)
{
    if (!title_ || canvas_created_shape_layer_id_.empty()) return;
    const std::string layer_id = canvas_created_shape_layer_id_;
    canvas_created_shape_layer_id_.clear();

    auto layer = title_->find_layer(layer_id);
    if (!layer) return;
    const bool is_text_layer = is_canvas_text_layer(*layer);
    const bool is_image_layer = layer->type == LayerType::Image;
    const bool too_small = is_text_layer
        ? false
        : (layer->shape_type == ShapeType::Line
               ? layer->rect_width < 2.0f
               : (layer->rect_width < 2.0f || layer->rect_height < 2.0f));
    if (!keep_layer || too_small) {
        title_->remove_layer(layer_id);
        layers_->refresh();
        timeline_->set_title(title_);
        sel_layer_id_.clear();
        if (canvas_) canvas_->set_selected_layers({});
        update_layer_panels(nullptr, playhead_);
        force_next_title_visual_update();
        on_title_modified();
        return;
    }

    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer_id);
    force_next_title_visual_update();
    on_title_modified();
    if (is_text_layer && canvas_) {
        canvas_->begin_text_edit_for_layer(layer_id);
    } else if (is_image_layer) {
        // Let the canvas/layer list paint the new image box before opening the picker.
        // Cancelling intentionally keeps the layer as an empty image placeholder.
        QTimer::singleShot(0, this, [this, layer_id]() {
            choose_image_file_for_layer(layer_id);
        });
    }
}

void TitleEditor::build_ui()
{
    restoring_editor_layout_ = true;

    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *menu_bar = new QMenuBar(this);
    auto *file_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.FileMenu"));
    QAction *new_action = file_menu->addAction(obsgs_tr("OBSTitles.New"));
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, &TitleEditor::new_title_contents);
    QAction *save_action = file_menu->addAction(obs_icon("save.svg"), obsgs_tr("OBSTitles.Save"));
    save_action->setShortcut(QKeySequence::Save);
    connect(save_action, &QAction::triggered, this, &TitleEditor::save_title);
    QAction *save_as_new_action = file_menu->addAction(obsgs_tr("OBSTitles.SaveAsNew"));
    connect(save_as_new_action, &QAction::triggered, this, &TitleEditor::save_title_as_new);
    QAction *save_library_action = file_menu->addAction(obsgs_tr("OBSTitles.SaveInLibrary"));
    connect(save_library_action, &QAction::triggered, this, [this]() { export_title_template(true); });
    QAction *export_action = file_menu->addAction(obs_icon("export.svg"), obsgs_tr("OBSTitles.Export"));
    connect(export_action, &QAction::triggered, this, [this]() { export_title_template(false); });
    file_menu->addSeparator();
    QAction *exit_action = file_menu->addAction(obs_icon("file-exit.svg"), obsgs_tr("OBSTitles.Exit"));
    connect(exit_action, &QAction::triggered, this, &TitleEditor::close);

    auto *edit_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.EditMenu"));
    edit_menu->addAction(act_undo_ = new QAction(obs_icon("undo.svg"), obsgs_tr("OBSTitles.Undo"), this));
    act_undo_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Z")));
    act_undo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(act_undo_, &QAction::triggered, this, [this]() {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
    });
    edit_menu->addAction(act_redo_ = new QAction(obs_icon("redo.svg"), obsgs_tr("OBSTitles.Redo"), this));
    act_redo_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
    act_redo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(act_redo_, &QAction::triggered, this, [this]() {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
    });
    edit_menu->addSeparator();
    QAction *copy_action = edit_menu->addAction(obsgs_tr("OBSTitles.Copy"));
    copy_action->setShortcut(QKeySequence::Copy);
    connect(copy_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->copy_keyframe_selection();
            return;
        }
        copy_selected_layer();
    });
    QAction *cut_action = edit_menu->addAction(obsgs_tr("OBSTitles.Cut"));
    cut_action->setShortcut(QKeySequence::Cut);
    connect(cut_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->cut_keyframe_selection();
            return;
        }
        cut_selected_layer();
    });
    QAction *paste_action = edit_menu->addAction(obsgs_tr("OBSTitles.Paste"));
    paste_action->setShortcut(QKeySequence::Paste);
    connect(paste_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_keyframe_clipboard()) {
            timeline_->paste_keyframes_at_playhead();
            return;
        }
        if (!layer_clipboard_.empty())
            paste_layer_from_clipboard();
        else
            paste_external_clipboard_to_canvas();
    });
    QAction *delete_action = edit_menu->addAction(obsgs_tr("OBSTitles.Delete"));
    delete_action->setShortcut(QKeySequence::Delete);
    connect(delete_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->delete_keyframe_selection();
            return;
        }
        delete_selected_layer();
    });
    QAction *duplicate_action = edit_menu->addAction(obsgs_tr("OBSTitles.DuplicateLayer"));
    duplicate_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(duplicate_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        duplicate_selected_layers();
    });
    edit_menu->addSeparator();
    QAction *preferences_action = edit_menu->addAction(obsgs_tr("OBSTitles.Preferences"));
    connect(preferences_action, &QAction::triggered, this, &TitleEditor::show_preferences);

    auto *view_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.View"));
    act_rulers_visible_ = view_menu->addAction(obsgs_tr("OBSTitles.Rulers"));
    act_rulers_visible_->setCheckable(true);
    act_rulers_visible_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    act_rulers_visible_->setToolTip(obsgs_tr("OBSTitles.RulersTooltip"));
    connect(act_rulers_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_rulers_visible(visible);
    });

    act_guides_visible_ = view_menu->addAction(obsgs_tr("OBSTitles.Guides"));
    act_guides_visible_->setCheckable(true);
    act_guides_visible_->setChecked(true);
    act_guides_visible_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft));
    act_guides_visible_->setToolTip(obsgs_tr("OBSTitles.GuidesTooltip"));
    connect(act_guides_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_guides_visible(visible);
    });

    act_guides_locked_ = view_menu->addAction(obsgs_tr("OBSTitles.LockGuides"));
    act_guides_locked_->setCheckable(true);
    act_guides_locked_->setToolTip(obsgs_tr("OBSTitles.LockGuidesTooltip"));
    connect(act_guides_locked_, &QAction::toggled, this, [this](bool locked) {
        if (canvas_) canvas_->set_guides_locked(locked);
    });

    act_guide_coordinates_ = view_menu->addAction(obsgs_tr("OBSTitles.ShowGuideCoordinates"));
    act_guide_coordinates_->setCheckable(true);
    act_guide_coordinates_->setChecked(true);
    connect(act_guide_coordinates_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_show_guide_coordinates(visible);
    });

    act_canvas_border_visible_ = view_menu->addAction(obsgs_tr("OBSTitles.CanvasBorder"));
    act_canvas_border_visible_->setCheckable(true);
    act_canvas_border_visible_->setChecked(true);
    act_canvas_border_visible_->setToolTip(obsgs_tr("OBSTitles.CanvasBorderTooltip"));
    connect(act_canvas_border_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_canvas_border_visible(visible);
    });

    act_clear_guides_ = view_menu->addAction(obsgs_tr("OBSTitles.ClearGuides"));
    act_clear_guides_->setToolTip(obsgs_tr("OBSTitles.ClearGuidesTooltip"));
    connect(act_clear_guides_, &QAction::triggered, this, [this]() {
        if (canvas_) canvas_->clear_user_guides();
    });
    view_menu->addSeparator();

    QAction *snap_action = view_menu->addAction(obsgs_tr("OBSTitles.Snap"));
    snap_action->setCheckable(true);
    snap_action->setChecked(true);
    snap_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    snap_action->setToolTip(obsgs_tr("OBSTitles.SnapTooltip"));
    connect(snap_action, &QAction::toggled, this, [this](bool enabled) {
        if (canvas_) canvas_->set_snap_enabled(enabled);
    });

    auto *snap_to_menu = view_menu->addMenu(obsgs_tr("OBSTitles.SnapTo"));
    auto add_snap_to_action = [this, snap_to_menu](const QString &text, bool checked, auto setter) {
        QAction *action = snap_to_menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, [this, setter](bool enabled) {
            if (canvas_) (canvas_->*setter)(enabled);
        });
        return action;
    };
    add_snap_to_action(obsgs_tr("OBSTitles.Guides"), true, &CanvasPreview::set_snap_to_guides);
    add_snap_to_action(obsgs_tr("OBSTitles.Grid"), false, &CanvasPreview::set_snap_to_grid);
    add_snap_to_action(obsgs_tr("OBSTitles.ObjectEdges"), true, &CanvasPreview::set_snap_to_object_edges);
    add_snap_to_action(obsgs_tr("OBSTitles.ObjectCenters"), true, &CanvasPreview::set_snap_to_object_centers);
    add_snap_to_action(obsgs_tr("OBSTitles.CanvasBounds"), true, &CanvasPreview::set_snap_to_canvas_bounds);
    add_snap_to_action(obsgs_tr("OBSTitles.SpacingAlignment"), true, &CanvasPreview::set_snap_to_spacing);

    create_docked_panel_menu(menu_bar);

    auto *help_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.HelpMenu"));
    QAction *about_action = help_menu->addAction(obs_icon("about.svg"), obsgs_tr("OBSTitles.About"));
    connect(about_action, &QAction::triggered, this, &TitleEditor::show_about);
    setMenuBar(menu_bar);

    /* ── Toolbar ── */
    build_toolbar();
    root->addWidget(toolbar_);

    /* ── Title name label bar ── */
    const QPalette editor_pal = qApp->palette();
    const QColor editor_window = editor_pal.color(QPalette::Window);
    const QColor editor_base = editor_pal.color(QPalette::Base);
    const QColor editor_text = editor_pal.color(QPalette::WindowText);
    const QColor editor_field_text = editor_pal.color(QPalette::Text);
    const QColor editor_button = editor_pal.color(QPalette::Button);
    const QColor editor_button_text = editor_pal.color(QPalette::ButtonText);
    const QColor editor_border = editor_pal.color(QPalette::Mid);
    const QColor editor_highlight = editor_pal.color(QPalette::Highlight);
    const QColor editor_hover = editor_button.lightness() < 128 ? editor_button.lighter(125) : editor_button.darker(108);

    auto *title_bar = new QWidget(this);
    title_bar->setStyleSheet(QStringLiteral("background:%1;color:%2;")
                                 .arg(editor_window.name(QColor::HexRgb),
                                      editor_text.name(QColor::HexRgb)));
    auto *title_bar_layout = new QHBoxLayout(title_bar);
    title_bar_layout->setContentsMargins(0, 3, 0, 3);
    title_bar_layout->setSpacing(6);
    title_bar_layout->addStretch(1);
    dirty_indicator_ = new QLabel(title_bar);
    dirty_indicator_->setFixedSize(10, 10);
    dirty_indicator_->setStyleSheet(QStringLiteral("background:%1;border-radius:5px;").arg(QColor(0xE3, 0x33, 0x33).name(QColor::HexRgb)));
    dirty_indicator_->setToolTip(obsgs_tr("OBSTitles.UnsavedChangesIndicator"));
    dirty_indicator_->hide();
    title_bar_layout->addWidget(dirty_indicator_, 0, Qt::AlignVCenter);
    title_lbl_ = new QLabel("—", title_bar);
    title_lbl_->setAlignment(Qt::AlignCenter);
    QFont tf = title_lbl_->font();
    tf.setPointSize(tf.pointSize() + 1);
    tf.setBold(true);
    title_lbl_->setFont(tf);
    title_lbl_->setStyleSheet(QStringLiteral("color:%1;").arg(editor_text.name(QColor::HexRgb)));
    title_lbl_->setToolTip(obsgs_tr("OBSTitles.DoubleClickRename"));
    title_lbl_->installEventFilter(this);
    title_bar_layout->addWidget(title_lbl_, 0, Qt::AlignVCenter);
    title_name_edit_ = new QLineEdit(title_bar);
    title_name_edit_->setAlignment(Qt::AlignCenter);
    title_name_edit_->setFont(tf);
    title_name_edit_->setMinimumWidth(180);
    title_name_edit_->setStyleSheet(QStringLiteral(
        "QLineEdit{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:1px 6px;}")
        .arg(editor_field_text.name(QColor::HexRgb),
             editor_base.name(QColor::HexRgb),
             editor_highlight.name(QColor::HexRgb)));
    title_name_edit_->hide();
    title_name_edit_->installEventFilter(this);
    connect(title_name_edit_, &QLineEdit::returnPressed, this, [this]() {
        commit_title_name_edit(true);
    });
    title_bar_layout->addWidget(title_name_edit_, 0, Qt::AlignVCenter);
    gpu_warning_lbl_ = new QLabel(title_bar);
    QFont gpu_warning_font = gpu_warning_lbl_->font();
    gpu_warning_font.setBold(true);
    gpu_warning_lbl_->setFont(gpu_warning_font);
    gpu_warning_lbl_->setStyleSheet(QStringLiteral("color:%1;").arg(QColor(0xFF, 0xCA, 0x4A).name(QColor::HexRgb)));
    gpu_warning_lbl_->hide();
    title_bar_layout->addWidget(gpu_warning_lbl_, 0, Qt::AlignVCenter);
    title_bar_layout->addStretch(1);
    root->addWidget(title_bar);

    /* ── Upper split: Canvas (dockable property panels live in QMainWindow dock areas) ── */
    auto *upper_split = new QSplitter(Qt::Horizontal, central);

    title_props_ = new TitlePropertiesPanel(this);
    title_props_->setMinimumWidth(240);

    auto *canvas_panel = new QWidget(upper_split);
    canvas_panel->setStyleSheet(QStringLiteral("background:%1;color:%2;")
                                    .arg(editor_window.name(QColor::HexRgb),
                                         editor_text.name(QColor::HexRgb)));
    auto *canvas_layout = new QVBoxLayout(canvas_panel);
    canvas_layout->setContentsMargins(0, 0, 0, 0);
    canvas_layout->setSpacing(0);
    canvas_ = new CanvasPreview(canvas_panel);
    canvas_->setMinimumSize(300, 200);
    if (act_rulers_visible_) act_rulers_visible_->setChecked(canvas_->rulers_visible());
    if (act_guides_visible_) act_guides_visible_->setChecked(canvas_->guides_visible());
    if (act_guides_locked_) act_guides_locked_->setChecked(canvas_->guides_locked());
    if (act_guide_coordinates_) act_guide_coordinates_->setChecked(canvas_->show_guide_coordinates());
    if (act_canvas_border_visible_) act_canvas_border_visible_->setChecked(canvas_->canvas_border_visible());
    canvas_layout->addWidget(canvas_, 1);

    auto *canvas_zoom_bar = new QWidget(canvas_panel);
    canvas_zoom_bar->setFixedHeight(34);
    canvas_zoom_bar->setStyleSheet(QStringLiteral(
        "QWidget{background:%1;border-top:1px solid %2;color:%3;}"
        "QPushButton,QToolButton{color:%4;background:%5;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
        "QPushButton:hover,QToolButton:hover{background:%6;}"
        "QToolButton::menu-indicator{image:none;}"
        "QSpinBox{color:%7;background:%8;border:1px solid %2;border-radius:3px;padding:2px 6px;}"
        "QSpinBox::up-button,QSpinBox::down-button{width:0;border:none;}"
        "QSlider::groove:horizontal{height:4px;background:%8;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:%4;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:%9;border-radius:2px;}")
        .arg(editor_window.name(QColor::HexRgb),
             editor_border.name(QColor::HexRgb),
             editor_text.name(QColor::HexRgb),
             editor_button_text.name(QColor::HexRgb),
             editor_button.name(QColor::HexRgb),
             editor_hover.name(QColor::HexRgb),
             editor_field_text.name(QColor::HexRgb),
             editor_base.name(QColor::HexRgb),
             editor_highlight.name(QColor::HexRgb)));
    auto *canvas_zoom_layout = new QHBoxLayout(canvas_zoom_bar);
    canvas_zoom_layout->setContentsMargins(10, 0, 10, 0);
    canvas_zoom_layout->setSpacing(8);
    auto *canvas_zoom_out = new QPushButton(canvas_zoom_bar);
    canvas_zoom_out->setIcon(obs_icon("zoom-out.svg"));
    canvas_zoom_out->setFixedWidth(30);
    auto *canvas_zoom_slider = new QSlider(Qt::Horizontal, canvas_zoom_bar);
    canvas_zoom_slider->setRange(5, 1600);
    canvas_zoom_slider->setValue(canvas_->zoom_percent());
    canvas_zoom_slider->setMinimumWidth(220);
    canvas_zoom_slider->setMaximumWidth(360);
    auto *canvas_zoom_in = new QPushButton(canvas_zoom_bar);
    canvas_zoom_in->setIcon(obs_icon("zoom-in.svg"));
    canvas_zoom_in->setFixedWidth(30);
    auto *canvas_zoom_percent = new QSpinBox(canvas_zoom_bar);
    canvas_zoom_percent->setRange(5, 1600);
    canvas_zoom_percent->setSuffix("%");
    canvas_zoom_percent->setAlignment(Qt::AlignCenter);
    canvas_zoom_percent->setButtonSymbols(QAbstractSpinBox::NoButtons);
    canvas_zoom_percent->setFixedWidth(72);
    canvas_zoom_percent->setValue(canvas_->zoom_percent());
    auto *fit_canvas = new LongPressToolButton(canvas_zoom_bar);
    fit_canvas->setText("Fit");
    fit_canvas->setCheckable(true);
    fit_canvas->setChecked(canvas_->fit_zoom_active());
    fit_canvas->setPopupMode(QToolButton::DelayedPopup);
    fit_canvas->setStyleSheet(QStringLiteral(
        "QToolButton:checked {"
        " background-color: palette(highlight); color: palette(highlighted-text);"
        " border: 1px solid palette(highlight); border-radius: 3px; }"
        "QToolButton::menu-indicator{image:none;width:0px;}"));
    auto *fit_canvas_menu = new QMenu(fit_canvas);
    auto add_canvas_zoom_action = [fit_canvas_menu](const QString &text, int percent) {
        QAction *action = fit_canvas_menu->addAction(text);
        action->setData(percent);
        return action;
    };
    QAction *fit_action = fit_canvas_menu->addAction("Fit");
    fit_action->setData(-1);
    QAction *fit_100_action = fit_canvas_menu->addAction("Fit up to 100%");
    fit_100_action->setData(-2);
    add_canvas_zoom_action("50%", 50);
    add_canvas_zoom_action("100%", 100);
    add_canvas_zoom_action("200%", 200);
    add_canvas_zoom_action("400%", 400);
    add_canvas_zoom_action("800%", 800);
    add_canvas_zoom_action("1600%", 1600);
    fit_canvas->setMenu(fit_canvas_menu);
    QSettings canvas_settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    canvas_settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    const int saved_checkerboard_pattern = std::clamp(
        canvas_settings.value(QString::fromUtf8(kEditorCanvasTransparencyKey), 1).toInt(), 0, 5);
    const bool saved_safe_guides_visible =
        canvas_settings.value(QString::fromUtf8(kEditorSafeGuidesVisibleKey), false).toBool();
    canvas_settings.endGroup();

    auto *checkerboard = new QToolButton(canvas_zoom_bar);
    checkerboard->setText("Transparency: Medium");
    checkerboard->setPopupMode(QToolButton::InstantPopup);
    auto *checkerboard_menu = new QMenu(checkerboard);
    auto *checkerboard_group = new QActionGroup(checkerboard_menu);
    checkerboard_group->setExclusive(true);
    auto add_checkerboard_action = [checkerboard_menu, checkerboard_group, saved_checkerboard_pattern](const QString &text, int pattern) {
        QAction *action = checkerboard_menu->addAction(text);
        action->setData(pattern);
        action->setCheckable(true);
        action->setChecked(pattern == saved_checkerboard_pattern);
        checkerboard_group->addAction(action);
        return action;
    };
    add_checkerboard_action("Light", 0);
    add_checkerboard_action("Medium", 1);
    add_checkerboard_action("Dark", 2);
    add_checkerboard_action("Solid white", 3);
    add_checkerboard_action("Solid black", 4);
    add_checkerboard_action("Solid grey", 5);
    if (auto *checked = checkerboard_group->checkedAction())
        checkerboard->setText(QString("Transparency: %1").arg(checked->text()));
    checkerboard->setMenu(checkerboard_menu);
    canvas_->set_checkerboard_pattern(saved_checkerboard_pattern);
    if (act_safe_guides_) {
        act_safe_guides_->setChecked(saved_safe_guides_visible);
        canvas_->set_safe_guides_visible(saved_safe_guides_visible);
    }
    canvas_zoom_layout->addWidget(canvas_zoom_out);
    canvas_zoom_layout->addWidget(canvas_zoom_slider);
    canvas_zoom_layout->addWidget(canvas_zoom_in);
    canvas_zoom_layout->addWidget(canvas_zoom_percent);
    canvas_zoom_layout->addWidget(fit_canvas);
    canvas_zoom_layout->addWidget(checkerboard);
    auto *safe_guides = new QToolButton(canvas_zoom_bar);
    safe_guides->setDefaultAction(act_safe_guides_);
    safe_guides->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    safe_guides->setStyleSheet(QStringLiteral(
        "QToolButton:checked {"
        " background-color: palette(highlight); color: palette(highlighted-text);"
        " border: 1px solid palette(highlight); border-radius: 3px; }"));
    canvas_zoom_layout->addWidget(safe_guides);

    auto *adaptive_rendering = new LongPressToolButton(canvas_zoom_bar);
    adaptive_rendering->setObjectName(QStringLiteral("editorAdaptiveRenderingButton"));
    adaptive_rendering->setIcon(obs_icon("lightning.svg"));
    adaptive_rendering->setIconSize(QSize(16, 16));
    adaptive_rendering->setCheckable(true);
    adaptive_rendering->setChecked(canvas_->adaptive_rendering_enabled());
    adaptive_rendering->setToolTip(obsgs_tr("OBSTitles.AdaptiveRenderingTooltip"));
    adaptive_rendering->setToolButtonStyle(Qt::ToolButtonIconOnly);
    adaptive_rendering->setPopupMode(QToolButton::DelayedPopup);
    adaptive_rendering->setStyleSheet(QStringLiteral(
        "QToolButton#editorAdaptiveRenderingButton:checked {"
        " background-color: palette(highlight); color: palette(highlighted-text);"
        " border: 1px solid palette(highlight); border-radius: 3px; }"));

    auto *adaptive_menu = new QMenu(adaptive_rendering);
    auto *adaptive_group = new QActionGroup(adaptive_menu);
    adaptive_group->setExclusive(true);
    const auto add_quality = [&](const QString &label, CanvasPreview::AdaptiveQualityMode mode) {
        QAction *action = adaptive_menu->addAction(label);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        adaptive_group->addAction(action);
        if (canvas_->adaptive_quality_mode() == mode)
            action->setChecked(true);
        return action;
    };
    add_quality(QStringLiteral("Auto"), CanvasPreview::AdaptiveQualityMode::Auto);
    add_quality(QStringLiteral("75%"), CanvasPreview::AdaptiveQualityMode::Percent75);
    add_quality(QStringLiteral("50%"), CanvasPreview::AdaptiveQualityMode::Percent50);
    add_quality(QStringLiteral("37,5%"), CanvasPreview::AdaptiveQualityMode::Percent37_5);
    add_quality(QStringLiteral("25%"), CanvasPreview::AdaptiveQualityMode::Percent25);
    adaptive_rendering->setMenu(adaptive_menu);
    canvas_zoom_layout->addWidget(adaptive_rendering);

    connect(adaptive_rendering, &QToolButton::toggled, canvas_,
            &CanvasPreview::set_adaptive_rendering_enabled);
    connect(adaptive_group, &QActionGroup::triggered, this, [this, adaptive_rendering](QAction *action) {
        const auto mode = static_cast<CanvasPreview::AdaptiveQualityMode>(action->data().toInt());
        canvas_->set_adaptive_quality_mode(mode);
        adaptive_rendering->setToolTip(
            obsgs_tr("OBSTitles.AdaptiveRenderingTooltip") +
            QStringLiteral("\n") + obsgs_tr("OBSTitles.AdaptiveRenderingQuality") +
            QStringLiteral(": ") + canvas_->adaptive_quality_label());
    });
    adaptive_rendering->setToolTip(
        obsgs_tr("OBSTitles.AdaptiveRenderingTooltip") +
        QStringLiteral("\n") + obsgs_tr("OBSTitles.AdaptiveRenderingQuality") +
        QStringLiteral(": ") + canvas_->adaptive_quality_label());

    canvas_zoom_layout->addStretch(1);
    canvas_layout->addWidget(canvas_zoom_bar);
    connect(canvas_zoom_slider, &QSlider::valueChanged, canvas_, &CanvasPreview::set_zoom_percent);
    connect(canvas_zoom_percent, qOverload<int>(&QSpinBox::valueChanged), canvas_, &CanvasPreview::set_zoom_percent);
    connect(canvas_, &CanvasPreview::zoom_percent_changed, this, [this, canvas_zoom_slider, canvas_zoom_percent, fit_canvas](int percent) {
        QSignalBlocker slider_blocker(canvas_zoom_slider);
        QSignalBlocker spin_blocker(canvas_zoom_percent);
        canvas_zoom_slider->setValue(percent);
        canvas_zoom_percent->setValue(percent);
        QSignalBlocker fit_blocker(fit_canvas);
        fit_canvas->setChecked(canvas_->fit_zoom_active());
    });
    connect(canvas_zoom_out, &QPushButton::clicked, this, [this]() {
        canvas_->set_zoom_percent((int)std::round(canvas_->zoom_percent() / 1.18));
    });
    connect(canvas_zoom_in, &QPushButton::clicked, this, [this]() {
        canvas_->set_zoom_percent((int)std::round(canvas_->zoom_percent() * 1.18));
    });
    connect(fit_canvas, &QToolButton::clicked, this, [this, fit_canvas]() {
        canvas_->fit_canvas(false);
        fit_canvas->setText(QStringLiteral("Fit"));
        QSignalBlocker fit_blocker(fit_canvas);
        fit_canvas->setChecked(canvas_->fit_zoom_active());
    });
    connect(fit_canvas_menu, &QMenu::triggered, this, [this, fit_canvas](QAction *action) {
        int value = action->data().toInt();
        fit_canvas->setText(action->text());
        if (value == -1) canvas_->fit_canvas(false);
        else if (value == -2) canvas_->fit_canvas(true);
        else canvas_->set_zoom_percent(value);
        QSignalBlocker fit_blocker(fit_canvas);
        fit_canvas->setChecked(canvas_->fit_zoom_active());
    });
    connect(checkerboard_menu, &QMenu::triggered, this, [this, checkerboard](QAction *action) {
        checkerboard->setText(QString("Transparency: %1").arg(action->text()));
        canvas_->set_checkerboard_pattern(action->data().toInt());
    });
    upper_split->addWidget(canvas_panel);

    props_ = new PropertiesPanel(this);
    props_->setMinimumWidth(260);
    upper_split->setStretchFactor(0, 1);

    graphic_props_dock_ = create_editor_dock(QString::fromUtf8(kGraphicPropertiesDockObjectName),
                                             obsgs_tr("OBSTitles.GraphicProperties"),
                                             title_props_);
    layer_props_dock_ = create_editor_dock(QString::fromUtf8(kLayerPropertiesDockObjectName),
                                           obsgs_tr("OBSTitles.LayerProperties"),
                                           props_);
    effects_dock_ = create_editor_dock(QString::fromUtf8(kEffectsDockObjectName),
                                       obsgs_tr("OBSTitles.Effects"),
                                       create_effects_panel());
    styles_dock_ = create_editor_dock(QString::fromUtf8(kStylesDockObjectName),
                                      obsgs_tr("OBSTitles.Styles"),
                                      create_styles_panel());
    color_swatches_dock_ = create_editor_dock(QString::fromUtf8(kColorSwatchesDockObjectName),
                                              obsgs_tr("OBSTitles.ColorSwatches"),
                                              create_color_swatches_panel());
    prerender_dock_ = create_editor_dock(QString::fromUtf8(kPrerenderDockObjectName),
                                         obsgs_tr("OBSTitles.PlaybackAndCache"),
                                         create_prerender_panel());
    tools_sidebar_ = new ToolsSidebar(this);
    tools_dock_ = create_editor_dock(QStringLiteral("OBSGraphicsStudioProToolsDock"),
                                     obsgs_tr("OBSTitles.Tools"),
                                     tools_sidebar_);
    tools_dock_->setMinimumWidth(46);
    tools_dock_->setMaximumWidth(64);
    addDockWidget(Qt::LeftDockWidgetArea, graphic_props_dock_);
    addDockWidget(Qt::RightDockWidgetArea, tools_dock_);
    addDockWidget(Qt::RightDockWidgetArea, layer_props_dock_);
    splitDockWidget(tools_dock_, layer_props_dock_, Qt::Horizontal);
    addDockWidget(Qt::LeftDockWidgetArea, styles_dock_);
    splitDockWidget(graphic_props_dock_, styles_dock_, Qt::Horizontal);
    addDockWidget(Qt::LeftDockWidgetArea, color_swatches_dock_);
    tabifyDockWidget(styles_dock_, color_swatches_dock_);
    addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
    splitDockWidget(layer_props_dock_, effects_dock_, Qt::Horizontal);
    addDockWidget(Qt::RightDockWidgetArea, prerender_dock_);
    tabifyDockWidget(effects_dock_, prerender_dock_);
    tools_dock_->raise();
    graphic_props_dock_->raise();
    layer_props_dock_->raise();
    styles_dock_->raise();

    /* ── Timeline editor: full-width transport | LayerStack + Timeline | full-width zoom ── */
    auto *timeline_editor = new QWidget(this);
    auto *timeline_editor_layout = new QVBoxLayout(timeline_editor);
    timeline_editor_layout->setContentsMargins(0, 0, 0, 0);
    timeline_editor_layout->setSpacing(0);

    auto *timeline_transport = new QWidget(timeline_editor);
    timeline_transport->setFixedHeight(34);
    const QPalette timeline_pal = qApp->palette();
    const QColor timeline_window = timeline_pal.color(QPalette::Window);
    const QColor timeline_base = timeline_pal.color(QPalette::Base);
    const QColor timeline_text = timeline_pal.color(QPalette::WindowText);
    const QColor timeline_button = timeline_pal.color(QPalette::Button);
    const QColor timeline_button_text = timeline_pal.color(QPalette::ButtonText);
    const QColor timeline_border = timeline_pal.color(QPalette::Mid);
    const QColor timeline_highlight = timeline_pal.color(QPalette::Highlight);
    const QColor timeline_hover = timeline_button.lightness() < 128 ? timeline_button.lighter(125) : timeline_button.darker(108);
    timeline_transport->setStyleSheet(QStringLiteral(
        "QWidget{background:%1;border-bottom:1px solid %2;color:%3;}"
        "QToolButton{color:%4;background:transparent;padding:3px 7px;border:none;}"
        "QToolButton:hover{background:%5;border-radius:2px;}"
        "QToolButton:checked{background:%6;color:%7;border-radius:2px;}"
        "QLabel{color:%6;font-family:monospace;}")
        .arg(timeline_window.name(QColor::HexRgb),
             timeline_border.name(QColor::HexRgb),
             timeline_text.name(QColor::HexRgb),
             timeline_button_text.name(QColor::HexRgb),
             timeline_hover.name(QColor::HexRgb),
             timeline_highlight.name(QColor::HexRgb),
             timeline_pal.color(QPalette::HighlightedText).name(QColor::HexRgb)));
    auto *transport_layout = new QHBoxLayout(timeline_transport);
    transport_layout->setContentsMargins(8, 0, 8, 0);
    transport_layout->setSpacing(2);
    auto make_transport_button = [timeline_transport](QAction *action) {
        auto *button = new QToolButton(timeline_transport);
        button->setDefaultAction(action);
        button->setIconSize(QSize(14, 14));
        button->setAutoRaise(true);
        return button;
    };
    transport_layout->addStretch(1);
    time_lbl_ = new QLabel("0.000 s", timeline_transport);
    time_lbl_->setMinimumWidth(116);
    time_lbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    time_lbl_->setStyleSheet(QStringLiteral("color:%1;font-family:monospace;")
                                 .arg(timeline_highlight.name(QColor::HexRgb)));
    transport_layout->addWidget(time_lbl_, 0, Qt::AlignVCenter);
    transport_layout->addSpacing(8);
    transport_layout->addWidget(make_transport_button(act_go_start_));
    transport_layout->addWidget(make_transport_button(act_prev_kf_));
    transport_layout->addWidget(make_transport_button(act_step_back_));
    transport_layout->addWidget(make_transport_button(act_rew_));
    transport_layout->addWidget(make_transport_button(act_play_));
    QAction *step_forward_action = new QAction(obs_icon("step-forward.svg"), obsgs_tr("OBSTitles.StepForward"), timeline_transport);
    connect(step_forward_action, &QAction::triggered, this, &TitleEditor::step_forward);
    transport_layout->addWidget(make_transport_button(step_forward_action));
    transport_layout->addWidget(make_transport_button(act_next_kf_));
    transport_layout->addWidget(make_transport_button(act_go_end_));
    transport_layout->addStretch(1);
    timeline_editor_layout->addWidget(timeline_transport);

    auto *lower_split = new QSplitter(Qt::Horizontal, timeline_editor);

    auto *layers_panel = new QWidget(lower_split);
    auto *layers_layout = new QVBoxLayout(layers_panel);
    layers_layout->setContentsMargins(0, 0, 0, 0);
    layers_layout->setSpacing(0);

    layers_ = new LayerStack(layers_panel);
    layers_->setMinimumHeight(140);
    layers_layout->addWidget(layers_, 1);
    layers_panel->setMinimumWidth(280);
    layers_panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    lower_split->addWidget(layers_panel);

    auto *timeline_panel = new QWidget(lower_split);
    auto *timeline_panel_layout = new QVBoxLayout(timeline_panel);
    timeline_panel_layout->setContentsMargins(0, 0, 0, 0);
    timeline_panel_layout->setSpacing(0);

    timeline_ = new TimelineWidget(timeline_panel);
    timeline_->setMinimumHeight(140);
    timeline_panel_layout->addWidget(timeline_, 1);

    auto *timeline_zoom_bar = new QWidget(timeline_panel);
    timeline_zoom_bar->setFixedHeight(34);
    timeline_zoom_bar->setStyleSheet(QStringLiteral(
        "QWidget{background:%1;border-top:1px solid %2;color:%3;}"
        "QPushButton{color:%4;background:%5;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
        "QPushButton:hover{background:%6;}"
        "QSlider::groove:horizontal{height:4px;background:%7;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:%4;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:%8;border-radius:2px;}")
        .arg(timeline_window.name(QColor::HexRgb),
             timeline_border.name(QColor::HexRgb),
             timeline_text.name(QColor::HexRgb),
             timeline_button_text.name(QColor::HexRgb),
             timeline_button.name(QColor::HexRgb),
             timeline_hover.name(QColor::HexRgb),
             timeline_base.name(QColor::HexRgb),
             timeline_highlight.name(QColor::HexRgb)));
    auto *zoom_layout = new QHBoxLayout(timeline_zoom_bar);
    zoom_layout->setContentsMargins(10, 0, 10, 0);
    zoom_layout->setSpacing(8);
    auto *zoom_out = new QPushButton(timeline_zoom_bar);
    zoom_out->setIcon(obs_icon("zoom-out.svg"));
    zoom_out->setFixedWidth(30);
    auto *zoom_slider = new QSlider(Qt::Horizontal, timeline_zoom_bar);
    zoom_slider->setRange(5, 1200);
    zoom_slider->setValue(timeline_->zoom_percent());
    zoom_slider->setMinimumWidth(220);
    zoom_slider->setMaximumWidth(360);
    auto *zoom_in = new QPushButton(timeline_zoom_bar);
    zoom_in->setIcon(obs_icon("zoom-in.svg"));
    zoom_in->setFixedWidth(30);
    auto *fit_timeline = new QPushButton(obsgs_tr("OBSTitles.FitTimeline"), timeline_zoom_bar);
    zoom_layout->addWidget(zoom_out);
    zoom_layout->addWidget(zoom_slider);
    zoom_layout->addWidget(zoom_in);
    zoom_layout->addWidget(fit_timeline);
    zoom_layout->addStretch(1);
    connect(zoom_slider, &QSlider::valueChanged, timeline_, &TimelineWidget::set_zoom_percent);
    connect(timeline_, &TimelineWidget::zoom_percent_changed, this, [zoom_slider](int percent) {
        QSignalBlocker blocker(zoom_slider);
        zoom_slider->setValue(percent);
    });
    connect(zoom_out, &QPushButton::clicked, this, [this]() {
        timeline_->set_zoom_percent((int)std::round(timeline_->zoom_percent() / 1.18));
    });
    connect(zoom_in, &QPushButton::clicked, this, [this]() {
        timeline_->set_zoom_percent((int)std::round(timeline_->zoom_percent() * 1.18));
    });
    connect(fit_timeline, &QPushButton::clicked, timeline_, &TimelineWidget::fit_timeline);
    timeline_panel_layout->addWidget(timeline_zoom_bar);
    lower_split->addWidget(timeline_panel);

    if (auto *scroll_bar = layers_->vertical_scroll_bar()) {
        connect(scroll_bar, &QScrollBar::valueChanged, timeline_, &TimelineWidget::set_vertical_scroll);
        connect(timeline_, &TimelineWidget::vertical_scroll_delta_requested, this,
                [scroll_bar](int delta) { scroll_bar->setValue(scroll_bar->value() + delta); });
    }
    lower_split->setStretchFactor(0, 1);
    lower_split->setStretchFactor(1, 3);
    lower_split->setCollapsible(0, false);
    lower_split->setCollapsible(1, false);
    timeline_editor_layout->addWidget(lower_split, 1);

    root->addWidget(upper_split, 1);

    timeline_editor->setMinimumHeight(210);
    timeline_dock_ = create_editor_dock(QString::fromUtf8(kTimelineDockObjectName),
                                        obsgs_tr("OBSTitles.Timeline"),
                                        timeline_editor);
    timeline_dock_->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    timeline_dock_->setMinimumHeight(220);
    addDockWidget(Qt::BottomDockWidgetArea, timeline_dock_);

    connect(&CacheManager::instance(), &CacheManager::frameReady, this,
            [this](const QString &title_id, int frame) {
                if (!title_ || title_id != QString::fromStdString(title_->id)) return;
                const int current_frame = (int)std::round(playhead_ * CacheManager::instance().effectiveFrameRate());
                if (canvas_ && frame == current_frame) canvas_->refresh_preview();
                if (timeline_) timeline_->update();
            });
    connect(&CacheManager::instance(), &CacheManager::cacheStatesChanged, this,
            [this](const QString &title_id, int, int) {
                if (!title_id.isEmpty() && title_ && title_id != QString::fromStdString(title_->id)) return;
                if (timeline_) timeline_->update();
            });

    load_editor_layout();

    /* ── Connect sub-widget signals ── */
    connect(layers_, &LayerStack::layer_selected,
            this, &TitleEditor::on_layer_selected);
    connect(layers_, &LayerStack::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.size() == 1 ? ids.back() : std::string();
                canvas_->set_selected_layers(ids);
                timeline_->set_selected_layers(ids);
                if (!title_ || ids.size() != 1) {
                    update_layer_panels(nullptr, playhead_);
                    return;
                }
                auto layer = title_->find_layer(sel_layer_id_);
                update_layer_panels(layer, playhead_);
            });

    connect(layers_, &LayerStack::add_layer_requested,
            this, [this](LayerType type) {
                if (!title_) return;
                if (type == LayerType::Image) {
                    // Image layers use the same draw-first workflow as toolbar objects:
                    // activate the image tool, then draw the image box on the canvas.
                    if (tools_sidebar_)
                        tools_sidebar_->activate_image_tool();
                    else if (canvas_)
                        canvas_->set_image_tool_active();
                    return;
                }
                auto l = create_basic_layer(type);
                if (!l) return;
                title_->add_layer(l);
                if (type == LayerType::Text)
                    pending_text_layer_auto_names_.insert(l->id);
                layers_->refresh();
                on_layer_selected(l->id);
                force_next_title_visual_update();
                on_title_modified();
                if (type == LayerType::Text && canvas_)
                    canvas_->begin_text_edit_for_layer(l->id);
            });

    connect(layers_, &LayerStack::clone_layer_requested,
            this, [this](const std::string &lid) {
                if (!title_) return;
                auto selected = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
                if (std::find(selected.begin(), selected.end(), lid) == selected.end())
                    on_layer_selected(lid);
                duplicate_selected_layers();
            });

    connect(layers_, &LayerStack::copy_layer_requested,
            this, [this](const std::string &lid) {
                auto selected = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
                if (std::find(selected.begin(), selected.end(), lid) == selected.end())
                    on_layer_selected(lid);
                copy_selected_layer();
            });

    connect(layers_, &LayerStack::paste_layer_requested,
            this, [this](const std::string &anchor_id) {
                if (!anchor_id.empty()) on_layer_selected(anchor_id);
                paste_layer_from_clipboard();
            });

    connect(layers_, &LayerStack::delete_layer_requested,
            this, [this](const std::string &lid) {
                auto selected = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
                if (std::find(selected.begin(), selected.end(), lid) == selected.end())
                    on_layer_selected(lid);
                delete_selected_layer();
            });

    connect(layers_, &LayerStack::layer_visibility_changed,
            this, [this](const std::string &lid, bool visible) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->visible = visible;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_lock_changed,
            this, [this](const std::string &lid, bool locked) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->locked = locked;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::property_keyframe_toggled,
            this, [this](const std::string &lid, const std::string &property_name) {
                if (!title_) return;
                auto layer = title_->find_layer(lid);
                if (!layer || layer->locked) return;
                const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                             std::max(0.0, layer->out_time - layer->in_time));
                for (auto prop : timeline_properties(*layer)) {
                    if (prop.name() != property_name) continue;
                    if (prop.vector)
                        toggle_keyframe(*prop.vector, lt, prop.vector->evaluate(lt));
                    else if (prop.scalar)
                        toggle_keyframe(*prop.scalar, lt, prop.scalar->evaluate(lt));
                    layers_->refresh();
                    timeline_->set_title(title_);
                    on_title_modified();
                    break;
                }
            });

    connect(layers_, &LayerStack::property_value_changed,
            this, [this](const std::string &lid, const std::string &property_name, double x, double y) {
                if (!title_) return;
                auto layer = title_->find_layer(lid);
                if (!layer || layer->locked) return;
                const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                             std::max(0.0, layer->out_time - layer->in_time));
                for (auto prop : timeline_properties(*layer)) {
                    if (prop.name() != property_name) continue;
                    if (property_name == "scale") { x /= 100.0; y /= 100.0; }
                    if (property_name == "opacity" || property_name == "char_scale_x" || property_name == "char_scale_y")
                        x /= 100.0;
                    if (prop.vector) set_animated_value(*prop.vector, lt, {x, y});
                    else if (prop.scalar) set_animated_value(*prop.scalar, lt, x);
                    timeline_->update();
                    if (canvas_) canvas_->update();
                    on_title_modified(false);
                    break;
                }
            });

    connect(layers_, &LayerStack::layer_expand_changed,
            this, [this](const std::string &lid, bool expanded) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->properties_expanded = expanded;
                    layers_->refresh();
                    timeline_->set_title(title_);
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_parent_changed,
            this, [this](const std::string &lid, const std::string &parent_id) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                                 std::max(0.0, layer->out_time - layer->in_time));
                    const QPointF world_pos = editor_layer_world_transform_for_parenting(title_, *layer, playhead_)
                        .map(QPointF(0.0, 0.0));

                    std::string next_parent_id = parent_id;
                    if (next_parent_id == layer->id || editor_parenting_would_cycle(title_, layer->id, next_parent_id))
                        next_parent_id.clear();

                    layer->parent_id = next_parent_id;
                    QPointF local_pos = world_pos;
                    if (!next_parent_id.empty()) {
                        if (const Layer *parent = editor_layer_by_id_for_parenting(title_, next_parent_id)) {
                            bool invertible = false;
                            QTransform parent_xf = editor_layer_world_transform_for_parenting(title_, *parent, playhead_);
                            QTransform parent_inv = parent_xf.inverted(&invertible);
                            if (invertible)
                                local_pos = parent_inv.map(world_pos);
                        }
                    }
                    set_animated_x(layer->position, lt, local_pos.x());
                    set_animated_y(layer->position, lt, local_pos.y());
                    layers_->refresh();
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_mask_changed,
            this, [this](const std::string &lid, const std::string &mask_source_id, MaskMode mask_mode) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->mask_source_id = mask_source_id;
                    layer->mask_mode = mask_source_id.empty() ? MaskMode::None : mask_mode;
                    layers_->refresh();
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_blend_mode_changed,
            this, [this](const std::string &lid, EffectBlendMode blend_mode) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->blend_mode = blend_mode;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_name_changed,
            this, [this](const std::string &lid, const std::string &name) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    const std::string next_name = unique_layer_name(name.empty() ? editor_text_std("OBSTitles.Layer") : name,
                                                                    {lid});
                    if (layer->name == next_name) return;
                    layer->name = next_name;
                    timeline_->set_title(title_);
                    on_title_modified();
                    QTimer::singleShot(0, layers_, [this]() {
                        if (layers_) layers_->refresh();
                    });
                }
            });

    connect(timeline_, &TimelineWidget::playhead_changed,
            this, &TitleEditor::on_playhead_changed);
    connect(timeline_, &TimelineWidget::layer_selected,
            this, &TitleEditor::on_layer_selected);
    connect(timeline_, &TimelineWidget::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.size() == 1 ? ids.back() : std::string();
                layers_->set_selected_layers(ids);
                canvas_->set_selected_layers(ids);
                if (!title_ || ids.size() != 1) {
                    update_layer_panels(nullptr, playhead_);
                    return;
                }
                auto layer = title_->find_layer(sel_layer_id_);
                update_layer_panels(layer, playhead_);
            });
    connect(timeline_, &TimelineWidget::keyframe_easing_changed,
            this, [this]() { on_title_modified(); });

    connect(props_, &PropertiesPanel::property_changed,
            this, [this](bool push_undo_snapshot) {
                if (updating_layer_panels_)
                    return;
                // Every EffectsPanel notification represents a real visual-model edit.
                // Bypass the selection-only visual hash guard so enable/disable and
                // stack changes always invalidate prerender/frame caches immediately.
                force_next_title_visual_update();
                on_title_modified(push_undo_snapshot);
                if (layers_) layers_->refresh();
            });
    connect(props_, &PropertiesPanel::live_visual_changed,
            this, [this]() {
                if (updating_layer_panels_)
                    return;
                force_next_title_visual_update();
                if (title_)
                    set_dirty(true);
                schedule_cache_invalidation();
                if (canvas_)
                    canvas_->refresh_preview();
            });
    connect(props_, &PropertiesPanel::text_char_format_changed,
            this, [this](const std::string &layer_id, const RichTextCharFormat &format, uint32_t mask) {
                if (canvas_) canvas_->apply_active_text_char_format(layer_id, format, mask);
            });
    connect(props_, &PropertiesPanel::gradient_editor_active_changed,
            this, [this](bool active) {
                if (canvas_) canvas_->set_gradient_editor_active(active);
            });
    connect(props_, &PropertiesPanel::recent_colors_changed,
            this, [this]() {
                refresh_color_swatches_panel();
                persist_recent_colors();
            });
    connect(props_, &PropertiesPanel::color_library_add_requested,
            this, &TitleEditor::show_add_color_to_library_dialog);
    connect(props_, &PropertiesPanel::color_picker_tool_requested,
            this, [this]() {
                reopen_color_tab_after_canvas_pick_ = true;
                if (canvas_) canvas_->set_color_picker_tool_active();
            });
    connect(title_props_, &TitlePropertiesPanel::title_changed,
            this, [this](bool push_undo_snapshot) {
                if (!title_) return;
                playhead_ = std::clamp(playhead_, 0.0, title_->duration);
                on_title_modified(push_undo_snapshot);
                timeline_->set_title(title_);
                on_playhead_changed(playhead_);
            });
    connect(layers_, &LayerStack::layer_order_changed,
            this, [this]() {
                layers_->refresh();
                canvas_->refresh_preview();
                timeline_->set_title(title_);
                on_title_modified();
            });

    connect(canvas_, &CanvasPreview::layer_clicked,
            this, &TitleEditor::on_layer_selected);
    connect(canvas_, &CanvasPreview::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.size() == 1 ? ids.back() : std::string();
                layers_->set_selected_layers(ids);
                canvas_->set_selected_layers(ids);
                timeline_->set_selected_layers(ids);
                if (!title_ || ids.size() != 1) {
                    update_layer_panels(nullptr, playhead_);
                    return;
                }
                auto layer = title_->find_layer(sel_layer_id_);
                update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::layer_geometry_changed,
            this, [this]() {
                on_title_modified();
                if (title_ && !sel_layer_id_.empty()) {
                    if (auto layer = title_->find_layer(sel_layer_id_))
                        update_layer_panels(layer, playhead_);
                }
            });
    connect(canvas_, &CanvasPreview::corner_context_changed,
            this, &TitleEditor::update_corner_toolbar);
    connect(canvas_, &CanvasPreview::text_edit_changed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                active_text_edit_layer_id_ = layer_id;
                if (props_) props_->set_active_text_edit_layer(layer_id);
                on_title_modified(false);
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::text_edit_cursor_changed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                active_text_edit_layer_id_ = layer_id;
                if (props_) props_->set_active_text_edit_layer(layer_id);
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::text_edit_committed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                active_text_edit_layer_id_.clear();
                if (props_) props_->set_active_text_edit_layer(std::string());
                bool renamed_layer = false;
                if (auto layer = title_->find_layer(layer_id)) {
                    if (layer->type == LayerType::Text &&
                        pending_text_layer_auto_names_.find(layer_id) != pending_text_layer_auto_names_.end()) {
                        const QString text = !layer->rich_text.empty()
                            ? QString::fromStdString(layer->rich_text.plain_text)
                            : QString::fromStdString(layer->text_content);
                        if (text_has_visible_name_content(text)) {
                            const std::string next_name = unique_layer_name(text.simplified().toStdString(), {layer_id});
                            if (layer->name != next_name) {
                                layer->name = next_name;
                                renamed_layer = true;
                            }
                        }
                        pending_text_layer_auto_names_.erase(layer_id);
                    }
                }
                on_title_modified();
                if (renamed_layer && layers_)
                    layers_->refresh();
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::color_picked,
            this, [this](const QColor &color, bool foreground) {
                if (!color.isValid())
                    return;
                if (foreground) {
                    default_foreground_color_ = color;
                    apply_picked_color_to_selection(color);
                } else {
                    default_background_color_ = color;
                }
                remember_recent_color(color);
                save_sidebar_default_colors();
                update_sidebar_color_swatches(nullptr);
                if (tools_sidebar_)
                    tools_sidebar_->activate_selection_tool();
                else if (canvas_)
                    canvas_->set_selection_tool_active();
            });
    connect(canvas_, &CanvasPreview::layer_structure_changed,
            this, [this]() {
                layers_->refresh();
                timeline_->set_title(title_);
            });
    connect(canvas_, &CanvasPreview::shape_drawing_started,
            this, &TitleEditor::create_shape_layer_from_canvas);
    connect(canvas_, &CanvasPreview::text_drawing_started,
            this, &TitleEditor::create_text_layer_from_canvas);
    connect(canvas_, &CanvasPreview::image_drawing_started,
            this, &TitleEditor::create_image_layer_from_canvas);
    connect(canvas_, &CanvasPreview::external_image_layer_requested,
            this, &TitleEditor::create_image_layer_from_external_source);
    connect(canvas_, &CanvasPreview::external_text_layer_requested,
            this, &TitleEditor::create_text_layer_from_external_source);
    connect(canvas_, &CanvasPreview::shape_drawing_changed,
            this, &TitleEditor::update_canvas_created_shape);
    connect(canvas_, &CanvasPreview::shape_drawing_finished,
            this, &TitleEditor::finish_canvas_created_shape);
    connect(canvas_, &CanvasPreview::pen_path_finished,
            this, &TitleEditor::create_pen_path_layer_from_canvas);
    if (tools_sidebar_) {
        connect(tools_sidebar_, &ToolsSidebar::selection_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_selection_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::direct_selection_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_direct_selection_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::shape_tool_requested, this, [this](ShapeType shape_type) {
            if (tools_sidebar_) tools_sidebar_->set_selected_shape(shape_type);
            if (canvas_) canvas_->set_shape_tool_active(shape_type);
        });
        connect(tools_sidebar_, &ToolsSidebar::pen_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_pen_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::text_tool_requested, this, [this](LayerType type) {
            if (tools_sidebar_) tools_sidebar_->set_selected_text_layer_type(type);
            if (canvas_) canvas_->set_text_tool_active(type);
        });
        connect(tools_sidebar_, &ToolsSidebar::image_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_image_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::color_picker_tool_requested, this, [this]() {
            reopen_color_tab_after_canvas_pick_ = false;
            if (canvas_) canvas_->set_color_picker_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::gradient_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_gradient_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::foreground_color_requested, this, [this]() {
            if (title_ && !sel_layer_id_.empty()) {
                if (props_) props_->open_foreground_color_selector();
            } else {
                open_default_sidebar_color_popup(true);
            }
        });
        connect(tools_sidebar_, &ToolsSidebar::background_color_requested, this, [this]() {
            if (title_ && !sel_layer_id_.empty()) {
                if (props_) props_->open_background_color_selector();
            } else {
                open_default_sidebar_color_popup(false);
            }
        });
        connect(tools_sidebar_, &ToolsSidebar::foreground_background_swap_requested, this, [this]() {
            if (title_ && !sel_layer_id_.empty()) {
                if (props_) props_->swap_foreground_background_colors();
                auto layer = title_->find_layer(sel_layer_id_);
                if (layer) set_default_sidebar_colors_from_layer(*layer);
                update_layer_panels(layer, playhead_);
            } else {
                std::swap(default_foreground_color_, default_background_color_);
                save_sidebar_default_colors();
                update_sidebar_color_swatches(nullptr);
            }
        });

    }
}

void TitleEditor::align_selected_to_canvas(int x_mode, int y_mode)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        double w = eval_box_width(*layer, lt);
        double h = eval_box_height(*layer, lt);
        double x = layer->origin_x * w;
        if (x_mode == 1) x = title_->width / 2.0;
        if (x_mode == 2) x = title_->width - (1.0 - layer->origin_x) * w;
        double y = layer->origin_y * h;
        if (y_mode == 1) y = title_->height / 2.0;
        if (y_mode == 2) y = title_->height - (1.0 - layer->origin_y) * h;
        set_animated_x(layer->position, lt, x);
        set_animated_y(layer->position, lt, y);
        last_layer = layer;
    }
    on_title_modified();
    if (last_layer) update_layer_panels(last_layer, playhead_);
}


void TitleEditor::flip_selected_layers(bool horizontal)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        Vec2Value scale = layer->scale.evaluate(lt);
        if (horizontal)
            scale.x = -scale.x;
        else
            scale.y = -scale.y;
        set_animated_value(layer->scale, lt, scale);
        last_layer = layer;
    }

    if (!last_layer) return;
    on_title_modified();
    update_layer_panels(last_layer, playhead_);
}

void TitleEditor::rotate_selected_layers(double degrees)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        set_animated_value(layer->rotation, lt, layer->rotation.evaluate(lt) + degrees);
        last_layer = layer;
    }

    if (!last_layer) return;
    on_title_modified();
    update_layer_panels(last_layer, playhead_);
}

void TitleEditor::align_selected_layers_horizontal()
{
    align_selected_layers(1, -1);
}

void TitleEditor::align_selected_layers_vertical()
{
    align_selected_layers(-1, 1);
}

void TitleEditor::align_selected_layers(int x_mode, int y_mode)
{
    if (!title_) return;

    // Multi-selection clears sel_layer_id_. Use the common operation selection
    // path so alignment works consistently from both the canvas and layer list.
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    struct Entry {
        std::shared_ptr<Layer> layer;
        double lt = 0.0;
        double anchor_x = 0.0;
        double anchor_y = 0.0;
        double left_offset = 0.0;
        double right_offset = 0.0;
        double top_offset = 0.0;
        double bottom_offset = 0.0;
    };

    std::vector<Entry> entries;
    entries.reserve(ids.size());

    double min_left = std::numeric_limits<double>::infinity();
    double max_right = -std::numeric_limits<double>::infinity();
    double min_top = std::numeric_limits<double>::infinity();
    double max_bottom = -std::numeric_limits<double>::infinity();
    double min_anchor_x = std::numeric_limits<double>::infinity();
    double max_anchor_x = -std::numeric_limits<double>::infinity();
    double min_anchor_y = std::numeric_limits<double>::infinity();
    double max_anchor_y = -std::numeric_limits<double>::infinity();

    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;

        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        const double width = eval_box_width(*layer, lt);
        const double height = eval_box_height(*layer, lt);
        const Vec2Value scale = layer->scale.evaluate(lt);
        const Vec2Value position = layer->position.evaluate(lt);
        const double origin_x = eval_origin_x(*layer, lt);
        const double origin_y = eval_origin_y(*layer, lt);
        const double rotation = layer->rotation.evaluate(lt) * 3.14159265358979323846 / 180.0;
        const double cos_r = std::cos(rotation);
        const double sin_r = std::sin(rotation);

        const double local_left = -origin_x * width;
        const double local_right = (1.0 - origin_x) * width;
        const double local_top = -origin_y * height;
        const double local_bottom = (1.0 - origin_y) * height;
        const double xs[4] = {local_left, local_right, local_right, local_left};
        const double ys[4] = {local_top, local_top, local_bottom, local_bottom};

        double left_offset = std::numeric_limits<double>::infinity();
        double right_offset = -std::numeric_limits<double>::infinity();
        double top_offset = std::numeric_limits<double>::infinity();
        double bottom_offset = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < 4; ++i) {
            const double sx = xs[i] * scale.x;
            const double sy = ys[i] * scale.y;
            const double tx = sx * cos_r - sy * sin_r;
            const double ty = sx * sin_r + sy * cos_r;
            left_offset = std::min(left_offset, tx);
            right_offset = std::max(right_offset, tx);
            top_offset = std::min(top_offset, ty);
            bottom_offset = std::max(bottom_offset, ty);
        }

        min_left = std::min(min_left, position.x + left_offset);
        max_right = std::max(max_right, position.x + right_offset);
        min_top = std::min(min_top, position.y + top_offset);
        max_bottom = std::max(max_bottom, position.y + bottom_offset);
        min_anchor_x = std::min(min_anchor_x, position.x);
        max_anchor_x = std::max(max_anchor_x, position.x);
        min_anchor_y = std::min(min_anchor_y, position.y);
        max_anchor_y = std::max(max_anchor_y, position.y);

        entries.push_back({layer, lt, position.x, position.y,
                           left_offset, right_offset, top_offset, bottom_offset});
    }

    if (entries.empty()) return;
    const bool align_to_selection = alignment_target_ == 0 || alignment_target_ == 4;
    const bool selection_anchors = alignment_target_ == 4;
    if (align_to_selection && entries.size() < 2) return;

    double target_left = selection_anchors ? min_anchor_x : min_left;
    double target_hcenter = selection_anchors ? (min_anchor_x + max_anchor_x) / 2.0
                                              : (min_left + max_right) / 2.0;
    double target_right = selection_anchors ? max_anchor_x : max_right;
    double target_top = selection_anchors ? min_anchor_y : min_top;
    double target_vcenter = selection_anchors ? (min_anchor_y + max_anchor_y) / 2.0
                                              : (min_top + max_bottom) / 2.0;
    double target_bottom = selection_anchors ? max_anchor_y : max_bottom;

    if (alignment_target_ == 1 || alignment_target_ == 2) {
        const double safe_inset = alignment_target_ == 1 ? OBS_GRAPHICS_SAFE_PERCENT : OBS_ACTION_SAFE_PERCENT;
        target_left = title_->width * safe_inset;
        target_hcenter = title_->width / 2.0;
        target_right = title_->width * (1.0 - safe_inset);
        target_top = title_->height * safe_inset;
        target_vcenter = title_->height / 2.0;
        target_bottom = title_->height * (1.0 - safe_inset);
    } else if (alignment_target_ == 3) {
        target_left = 0.0;
        target_hcenter = title_->width / 2.0;
        target_right = title_->width;
        target_top = 0.0;
        target_vcenter = title_->height / 2.0;
        target_bottom = title_->height;
    }

    std::shared_ptr<Layer> last_layer;
    for (const auto &entry : entries) {
        if (x_mode >= 0) {
            double next_x = entry.anchor_x;
            if (selection_anchors) {
                if (x_mode == 0) next_x = target_left;
                if (x_mode == 1) next_x = target_hcenter;
                if (x_mode == 2) next_x = target_right;
            } else {
                if (x_mode == 0) next_x = target_left - entry.left_offset;
                if (x_mode == 1) next_x = target_hcenter - (entry.left_offset + entry.right_offset) / 2.0;
                if (x_mode == 2) next_x = target_right - entry.right_offset;
            }
            set_animated_x(entry.layer->position, entry.lt, next_x);
        }
        if (y_mode >= 0) {
            double next_y = entry.anchor_y;
            if (selection_anchors) {
                if (y_mode == 0) next_y = target_top;
                if (y_mode == 1) next_y = target_vcenter;
                if (y_mode == 2) next_y = target_bottom;
            } else {
                if (y_mode == 0) next_y = target_top - entry.top_offset;
                if (y_mode == 1) next_y = target_vcenter - (entry.top_offset + entry.bottom_offset) / 2.0;
                if (y_mode == 2) next_y = target_bottom - entry.bottom_offset;
            }
            set_animated_y(entry.layer->position, entry.lt, next_y);
        }
        last_layer = entry.layer;
    }

    if (!last_layer) return;
    on_title_modified();
    update_layer_panels(last_layer, playhead_);
    if (canvas_) canvas_->update();
    if (timeline_) timeline_->update();
}

void TitleEditor::distribute_selected_layers(bool horizontal)
{
    if (!title_) return;

    // Multi-selection intentionally clears sel_layer_id_. Always use the shared
    // operation-selection helper so canvas/layer-list multi-selection works.
    const auto ids = selected_layer_ids_for_operation();
    if (ids.size() < 3) return;

    struct Entry {
        std::shared_ptr<Layer> layer;
        double lt = 0.0;
        double anchor = 0.0;
        double start = 0.0;
        double end = 0.0;
        double start_offset = 0.0;
    };

    std::vector<Entry> entries;
    entries.reserve(ids.size());
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;

        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        const double width = eval_box_width(*layer, lt);
        const double height = eval_box_height(*layer, lt);
        const Vec2Value scale = layer->scale.evaluate(lt);
        const Vec2Value position = layer->position.evaluate(lt);
        const double origin_x = eval_origin_x(*layer, lt);
        const double origin_y = eval_origin_y(*layer, lt);
        const double rotation_radians = layer->rotation.evaluate(lt) * 3.14159265358979323846 / 180.0;
        const double cos_r = std::cos(rotation_radians);
        const double sin_r = std::sin(rotation_radians);

        // Build the axis-aligned bounds from this layer's own transformed
        // corners. Do not use the aggregate selection bounds as an item in
        // the distribution calculation.
        const double local_left = -origin_x * width;
        const double local_right = (1.0 - origin_x) * width;
        const double local_top = -origin_y * height;
        const double local_bottom = (1.0 - origin_y) * height;
        const double xs[4] = {local_left, local_right, local_right, local_left};
        const double ys[4] = {local_top, local_top, local_bottom, local_bottom};
        double start_offset = std::numeric_limits<double>::max();
        double end_offset = std::numeric_limits<double>::lowest();
        for (int corner = 0; corner < 4; ++corner) {
            const double sx = xs[corner] * scale.x;
            const double sy = ys[corner] * scale.y;
            const double axis_offset = horizontal
                ? (sx * cos_r - sy * sin_r)
                : (sx * sin_r + sy * cos_r);
            start_offset = std::min(start_offset, axis_offset);
            end_offset = std::max(end_offset, axis_offset);
        }
        const double axis_position = horizontal ? position.x : position.y;

        entries.push_back({layer, lt, axis_position, axis_position + start_offset,
                           axis_position + end_offset, start_offset});
    }

    if (entries.size() < 3) return;

    if (distribute_to_anchors_) {
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            return a.anchor < b.anchor;
        });

        const double step = (entries.back().anchor - entries.front().anchor) /
                            static_cast<double>(entries.size() - 1);
        for (size_t i = 1; i + 1 < entries.size(); ++i) {
            const double next_position = entries.front().anchor + step * static_cast<double>(i);
            if (horizontal)
                set_animated_x(entries[i].layer->position, entries[i].lt, next_position);
            else
                set_animated_y(entries[i].layer->position, entries[i].lt, next_position);
        }
    } else {
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.start != b.start) return a.start < b.start;
            return a.end < b.end;
        });

        // Use only each object's individual bounds. The usable interval is
        // the space between the trailing bound of the first object and the
        // leading bound of the last object; the aggregate selection bound is
        // never treated as an additional bound.
        double intermediate_extent = 0.0;
        for (size_t i = 1; i + 1 < entries.size(); ++i)
            intermediate_extent += entries[i].end - entries[i].start;
        const double available_between_outer_objects =
            entries.back().start - entries.front().end;
        const double gap = (available_between_outer_objects - intermediate_extent) /
                           static_cast<double>(entries.size() - 1);

        double next_start = entries.front().end + gap;
        for (size_t i = 1; i + 1 < entries.size(); ++i) {
            const double next_position = next_start - entries[i].start_offset;
            if (horizontal)
                set_animated_x(entries[i].layer->position, entries[i].lt, next_position);
            else
                set_animated_y(entries[i].layer->position, entries[i].lt, next_position);
            next_start += (entries[i].end - entries[i].start) + gap;
        }
    }

    on_title_modified();
    update_layer_panels(entries.back().layer, playhead_);
    if (canvas_) canvas_->update();
    if (timeline_) timeline_->update();
}

void TitleEditor::build_toolbar()
{
    constexpr int kEditorToolbarIconSize = 18;
    constexpr int kEditorToolbarButtonExtent = 30;
    const QPalette pal = qApp->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor base = pal.color(QPalette::Base);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);

    toolbar_ = new QToolBar(this);
    toolbar_->setMovable(false);
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar_->setIconSize(QSize(kEditorToolbarIconSize, kEditorToolbarIconSize));
    toolbar_->setStyleSheet(QStringLiteral(
        "QToolBar{background:%1;border-bottom:1px solid %2;spacing:2px;}"
        "QToolButton{color:%3;background:transparent;min-width:%4px;min-height:%4px;max-width:%4px;max-height:%4px;padding:0;border:none;}"
        "QToolButton:hover{background:%5;border-radius:3px;}"
        "QToolButton:pressed{background:%6;color:%7;border-radius:3px;}"
        "QToolButton:checked{background:%6;color:%7;border-radius:3px;}"
        "QToolButton[toolButtonStyle=\"1\"]{min-width:auto;max-width:none;padding:2px 8px;}"
        "QDoubleSpinBox{color:%8;background:%9;border:1px solid %2;border-radius:3px;padding:2px 4px;}"
        "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:0;border:none;}")
        .arg(window.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             button_text.name(QColor::HexRgb),
             QString::number(kEditorToolbarButtonExtent),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             base.name(QColor::HexRgb)));

    act_go_start_ = new QAction(obs_icon("go-to-start.svg"), obsgs_tr("OBSTitles.GoToStart"), this);
    act_prev_kf_ = new QAction(obs_icon("previous-keyframe.svg"), obsgs_tr("OBSTitles.PreviousKeyframe"), this);
    act_step_back_ = new QAction(obs_icon("step-backward.svg"), obsgs_tr("OBSTitles.StepBackward"), this);
    act_rew_ = new QAction(obs_icon("play-reverse.svg"), obsgs_tr("OBSTitles.PlayReverse"), this);
    act_play_ = new QAction(obs_icon("play.svg"), obsgs_tr("OBSTitles.Play"), this);
    act_play_->setToolTip(obsgs_tr("OBSTitles.PlayTooltip"));
    act_go_end_ = new QAction(obs_icon("go-to-end.svg"), obsgs_tr("OBSTitles.GoToEnd"), this);
    act_next_kf_ = new QAction(obs_icon("next-keyframe.svg"), obsgs_tr("OBSTitles.NextKeyframe"), this);

    connect(act_go_start_, &QAction::triggered, this, &TitleEditor::go_to_start);
    connect(act_prev_kf_, &QAction::triggered, this, &TitleEditor::previous_keyframe);
    connect(act_step_back_, &QAction::triggered, this, &TitleEditor::step_backward);
    connect(act_rew_, &QAction::triggered, this, &TitleEditor::reverse_play);
    connect(act_play_, &QAction::triggered, this, &TitleEditor::play_pause);
    connect(act_go_end_, &QAction::triggered, this, &TitleEditor::go_to_end);
    connect(act_next_kf_, &QAction::triggered, this, &TitleEditor::next_keyframe);

    toolbar_->addSeparator();
    auto *align_target = new QToolButton(toolbar_);
    align_target->setIcon(obs_icon("alignment-target.svg"));
    align_target->setIconSize(toolbar_->iconSize());
    align_target->setToolButtonStyle(Qt::ToolButtonIconOnly);
    align_target->setToolTip(obsgs_tr("OBSTitles.AlignmentTarget"));
    align_target->setAccessibleName(obsgs_tr("OBSTitles.AlignmentTarget"));
    align_target->setPopupMode(QToolButton::InstantPopup);
    align_target->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
    align_target->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;}"));
    auto *align_menu = new QMenu(align_target);
    QAction *target_selection_bounds = align_menu->addAction(obsgs_tr("OBSTitles.AlignToSelectionBounds"));
    QAction *target_selection_anchors = align_menu->addAction(obsgs_tr("OBSTitles.AlignToSelectionAnchors"));
    QAction *target_title_safe = align_menu->addAction(obsgs_tr("OBSTitles.AlignToTitleSafeGuides"));
    QAction *target_action_safe = align_menu->addAction(obsgs_tr("OBSTitles.AlignToActionSafeGuides"));
    QAction *target_artboard = align_menu->addAction(obsgs_tr("OBSTitles.AlignToArtboard"));
    target_selection_bounds->setCheckable(true);
    target_selection_anchors->setCheckable(true);
    target_title_safe->setCheckable(true);
    target_action_safe->setCheckable(true);
    target_artboard->setCheckable(true);
    target_artboard->setChecked(true);
    auto update_alignment_target = [this, align_target, target_selection_bounds, target_selection_anchors, target_title_safe, target_action_safe, target_artboard](int target) {
        alignment_target_ = target;
        target_selection_bounds->setChecked(target == 0);
        target_selection_anchors->setChecked(target == 4);
        target_title_safe->setChecked(target == 1);
        target_action_safe->setChecked(target == 2);
        target_artboard->setChecked(target == 3);
        QString tooltip = obsgs_tr("OBSTitles.AlignToArtboard");
        if (target == 0)
            tooltip = obsgs_tr("OBSTitles.AlignToSelectionBounds");
        else if (target == 4)
            tooltip = obsgs_tr("OBSTitles.AlignToSelectionAnchors");
        else if (target == 1)
            tooltip = obsgs_tr("OBSTitles.AlignToTitleSafeGuides");
        else if (target == 2)
            tooltip = obsgs_tr("OBSTitles.AlignToActionSafeGuides");
        align_target->setToolTip(tooltip);
    };
    connect(target_selection_bounds, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(0); });
    connect(target_selection_anchors, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(4); });
    connect(target_title_safe, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(1); });
    connect(target_action_safe, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(2); });
    connect(target_artboard, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(3); });
    align_target->setMenu(align_menu);
    toolbar_->addWidget(align_target);

    auto add_align_action = [this](const char *icon_name, const QString &tip, int x_mode, int y_mode) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), tip);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, x_mode, y_mode]() {
            align_selected_layers(x_mode, y_mode);
        });
        return action;
    };
    add_align_action("align-left.svg", obsgs_tr("OBSTitles.AlignLeft"), 0, -1);
    add_align_action("align-horizontal-center.svg", obsgs_tr("OBSTitles.AlignHorizontalCenter"), 1, -1);
    add_align_action("align-right.svg", obsgs_tr("OBSTitles.AlignRight"), 2, -1);
    add_align_action("align-top.svg", obsgs_tr("OBSTitles.AlignTop"), -1, 0);
    add_align_action("align-vertical-center.svg", obsgs_tr("OBSTitles.AlignVerticalCenter"), -1, 1);
    add_align_action("align-bottom.svg", obsgs_tr("OBSTitles.AlignBottom"), -1, 2);
    add_align_action("align-center-artboard.svg", obsgs_tr("OBSTitles.AlignCenterToArtboard"), 1, 1);

    auto *distribute_mode = new QToolButton(toolbar_);
    distribute_mode->setIcon(obs_icon("distribute-mode.svg"));
    distribute_mode->setIconSize(toolbar_->iconSize());
    distribute_mode->setToolButtonStyle(Qt::ToolButtonIconOnly);
    distribute_mode->setPopupMode(QToolButton::InstantPopup);
    distribute_mode->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
    distribute_mode->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;}"));

    auto *distribute_menu = new QMenu(distribute_mode);
    auto *distribute_group = new QActionGroup(distribute_menu);
    distribute_group->setExclusive(true);
    QAction *distribute_bounds = distribute_menu->addAction(obsgs_tr("OBSTitles.DistributeToBounds"));
    QAction *distribute_anchors = distribute_menu->addAction(obsgs_tr("OBSTitles.DistributeToAnchors"));
    distribute_bounds->setCheckable(true);
    distribute_anchors->setCheckable(true);
    distribute_group->addAction(distribute_bounds);
    distribute_group->addAction(distribute_anchors);
    distribute_bounds->setChecked(true);
    distribute_mode->setToolTip(obsgs_tr("OBSTitles.DistributeToBounds"));
    distribute_mode->setAccessibleName(obsgs_tr("OBSTitles.DistributeMode"));
    connect(distribute_bounds, &QAction::triggered, this, [this, distribute_mode]() {
        distribute_to_anchors_ = false;
        distribute_mode->setToolTip(obsgs_tr("OBSTitles.DistributeToBounds"));
    });
    connect(distribute_anchors, &QAction::triggered, this, [this, distribute_mode]() {
        distribute_to_anchors_ = true;
        distribute_mode->setToolTip(obsgs_tr("OBSTitles.DistributeToAnchors"));
    });
    distribute_mode->setMenu(distribute_menu);
    toolbar_->addWidget(distribute_mode);

    auto add_distribute_action = [this](const char *icon_name, const QString &tip, bool horizontal) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), tip);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, horizontal]() {
            distribute_selected_layers(horizontal);
        });
        return action;
    };
    add_distribute_action("distribute-horizontal.svg", obsgs_tr("OBSTitles.DistributeHorizontally"), true);
    add_distribute_action("distribute-vertical.svg", obsgs_tr("OBSTitles.DistributeVertically"), false);

    toolbar_->addSeparator();
    auto add_flip_action = [this](const char *icon_name, const QString &text, bool horizontal) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, horizontal]() {
            flip_selected_layers(horizontal);
        });
        return action;
    };
    add_flip_action("flip-horizontal.svg", obsgs_tr("OBSTitles.FlipHorizontal"), true);
    add_flip_action("flip-vertical.svg", obsgs_tr("OBSTitles.FlipVertical"), false);

    toolbar_->addSeparator();
    auto *rotation_degrees = new QDoubleSpinBox(toolbar_);
    rotation_degrees->setRange(-9999.0, 9999.0);
    rotation_degrees->setDecimals(1);
    rotation_degrees->setSingleStep(1.0);
    rotation_degrees->setValue(90.0);
    rotation_degrees->setSuffix(QStringLiteral("°"));
    rotation_degrees->setToolTip(obsgs_tr("OBSTitles.RotateDegreesTooltip"));
    rotation_degrees->setAccessibleName(obsgs_tr("OBSTitles.RotateDegrees"));
    rotation_degrees->setFixedWidth(78);
    toolbar_->addWidget(rotation_degrees);
    auto add_rotate_action = [this, rotation_degrees](const char *icon_name, const QString &text, double direction) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, rotation_degrees, direction]() {
            rotate_selected_layers(rotation_degrees->value() * direction);
        });
        return action;
    };
    add_rotate_action("rotate-left.svg", obsgs_tr("OBSTitles.RotateLeft"), -1.0);
    add_rotate_action("rotate-right.svg", obsgs_tr("OBSTitles.RotateRight"), 1.0);

    act_safe_guides_ = new QAction(obs_icon("safe.svg"), obsgs_tr("OBSTitles.Safe"), this);
    act_safe_guides_->setCheckable(true);
    act_safe_guides_->setToolTip(obsgs_tr("OBSTitles.SafeTooltip"));
    connect(act_safe_guides_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_safe_guides_visible(visible);
    });


    /*
     * Keep the context controls in one QWidgetAction.  QToolBar owns widgets
     * through QWidgetAction; toggling only a child widget that was hidden when
     * it was added can leave the toolbar action with a zero-sized geometry on
     * Windows.  The action itself is therefore the single source of truth for
     * visibility and layout recalculation.
     */
    dynamic_toolbar_widget_ = new QWidget(toolbar_);
    dynamic_toolbar_widget_->setObjectName(QStringLiteral("canvasDynamicToolbar"));
    dynamic_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *dynamic_toolbar_layout = new QHBoxLayout(dynamic_toolbar_widget_);
    dynamic_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    dynamic_toolbar_layout->setSpacing(2);

    point_toolbar_widget_ = new QWidget(dynamic_toolbar_widget_);
    point_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *point_toolbar_layout = new QHBoxLayout(point_toolbar_widget_);
    point_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    point_toolbar_layout->setSpacing(3);
    auto make_point_button = [this](const char *icon_name, const QString &tooltip) {
        auto *button = new QToolButton(point_toolbar_widget_);
        button->setIcon(obs_icon(icon_name));
        button->setIconSize(toolbar_->iconSize());
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
        return button;
    };
    point_toolbar_corner_ = make_point_button("draw-polygon.svg", obsgs_tr("OBSTitles.ConvertAnchorToCorner"));
    point_toolbar_smooth_ = make_point_button("bezier-curve.svg", obsgs_tr("OBSTitles.ConvertAnchorToSmooth"));
    point_toolbar_handles_ = make_point_button("eye.svg", obsgs_tr("OBSTitles.ShowSelectedPointHandles"));
    point_toolbar_handles_->setChecked(true);

    auto make_point_position = [this](const QString &prefix, const QString &tooltip) {
        auto *spin = new QDoubleSpinBox(point_toolbar_widget_);
        spin->setRange(-100000.0, 100000.0);
        spin->setDecimals(1);
        spin->setSingleStep(1.0);
        spin->setPrefix(prefix);
        spin->setSpecialValueText(QStringLiteral("—"));
        spin->setToolTip(tooltip);
        spin->setKeyboardTracking(false);
        spin->setFixedWidth(78);
        return spin;
    };
    point_toolbar_x_ = make_point_position(QStringLiteral("X: "), obsgs_tr("OBSTitles.PointXTooltip"));
    point_toolbar_y_ = make_point_position(QStringLiteral("Y: "), obsgs_tr("OBSTitles.PointYTooltip"));
    point_toolbar_layout->addWidget(point_toolbar_corner_);
    point_toolbar_layout->addWidget(point_toolbar_smooth_);
    point_toolbar_layout->addWidget(point_toolbar_handles_);
    point_toolbar_layout->addWidget(point_toolbar_x_);
    point_toolbar_layout->addWidget(point_toolbar_y_);
    dynamic_toolbar_layout->addWidget(point_toolbar_widget_);

    corner_toolbar_widget_ = new QWidget(dynamic_toolbar_widget_);
    corner_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *corner_toolbar_layout = new QHBoxLayout(corner_toolbar_widget_);
    corner_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    corner_toolbar_layout->setSpacing(5);
    corner_toolbar_label_ = new QLabel(obsgs_tr("OBSTitles.Corners"), corner_toolbar_widget_);
    corner_toolbar_type_ = new QComboBox(corner_toolbar_widget_);
    corner_toolbar_type_->addItem(obsgs_tr("OBSTitles.Round"), (int)CornerType::Round);
    corner_toolbar_type_->addItem(obsgs_tr("OBSTitles.InvertedRound"), (int)CornerType::Concave);
    corner_toolbar_type_->addItem(obsgs_tr("OBSTitles.Chamfer"), (int)CornerType::Straight);
    corner_toolbar_type_->setToolTip(obsgs_tr("OBSTitles.CornerType"));
    corner_toolbar_type_->setFixedWidth(106);
    corner_toolbar_radius_ = new QDoubleSpinBox(corner_toolbar_widget_);
    corner_toolbar_radius_->setRange(-1.0, 9999.0);
    corner_toolbar_radius_->setDecimals(1);
    corner_toolbar_radius_->setSingleStep(1.0);
    corner_toolbar_radius_->setSuffix(QStringLiteral(" px"));
    corner_toolbar_radius_->setSpecialValueText(obsgs_tr("OBSTitles.MixedValues"));
    corner_toolbar_radius_->setToolTip(obsgs_tr("OBSTitles.CornerRadius"));
    corner_toolbar_radius_->setFixedWidth(82);
    corner_toolbar_sync_ = new QCheckBox(obsgs_tr("OBSTitles.SyncCornerRadii"), corner_toolbar_widget_);
    corner_toolbar_sync_->setTristate(true);
    corner_toolbar_sync_->setToolTip(obsgs_tr("OBSTitles.SyncCornerRadiiTooltip"));
    corner_toolbar_layout->addWidget(corner_toolbar_label_);
    corner_toolbar_layout->addWidget(corner_toolbar_type_);
    corner_toolbar_layout->addWidget(corner_toolbar_radius_);
    corner_toolbar_layout->addWidget(corner_toolbar_sync_);
    dynamic_toolbar_layout->addWidget(corner_toolbar_widget_);

    /* Insert exactly between Rotate and Undo/Redo. */
    dynamic_toolbar_action_ = toolbar_->addWidget(dynamic_toolbar_widget_);
    dynamic_toolbar_action_->setPriority(QAction::HighPriority);
    dynamic_toolbar_action_->setVisible(false);
    point_toolbar_widget_->setVisible(false);
    corner_toolbar_widget_->setVisible(false);

    dynamic_toolbar_separator_ = toolbar_->addSeparator();
    dynamic_toolbar_separator_->setVisible(false);
    toolbar_->addAction(act_undo_);
    toolbar_->addAction(act_redo_);
    update_undo_redo_actions();

    auto *toolbar_spacer = new QWidget(toolbar_);
    toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar_->addWidget(toolbar_spacer);

    connect(point_toolbar_corner_, &QToolButton::clicked, this, [this]() {
        if (!updating_corner_toolbar_ && canvas_)
            canvas_->convert_selected_points_to_corner();
    });
    connect(point_toolbar_smooth_, &QToolButton::clicked, this, [this]() {
        if (!updating_corner_toolbar_ && canvas_)
            canvas_->convert_selected_points_to_smooth();
    });
    connect(point_toolbar_handles_, &QToolButton::toggled, this, [this](bool checked) {
        if (!updating_corner_toolbar_ && canvas_)
            canvas_->set_point_control_show_handles(checked);
    });
    connect(point_toolbar_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (updating_corner_toolbar_ || !canvas_ || value <= point_toolbar_x_->minimum()) return;
                canvas_->set_point_control_x(value);
            });
    connect(point_toolbar_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (updating_corner_toolbar_ || !canvas_ || value <= point_toolbar_y_->minimum()) return;
                canvas_->set_point_control_y(value);
            });

    connect(corner_toolbar_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (updating_corner_toolbar_ || !canvas_ || index < 0) return;
                const QVariant data = corner_toolbar_type_->itemData(index);
                if (!data.isValid()) return;
                canvas_->set_corner_control_type((CornerType)data.toInt());
            });
    connect(corner_toolbar_radius_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (updating_corner_toolbar_ || !canvas_ || value < 0.0) return;
                canvas_->set_corner_control_radius(value);
            });
    connect(corner_toolbar_sync_, &QCheckBox::stateChanged,
            this, [this](int state) {
                if (updating_corner_toolbar_ || !canvas_ || state == Qt::PartiallyChecked) return;
                canvas_->set_corner_control_sync(state == Qt::Checked);
            });

    act_live_editing_ = new QAction(obsgs_tr("OBSTitles.LiveEditing"), this);
    act_live_editing_->setCheckable(true);
    act_live_editing_->setToolTip(obsgs_tr("OBSTitles.LiveEditingTooltip"));
    connect(act_live_editing_, &QAction::toggled, this, &TitleEditor::set_live_editing_enabled);

    auto *live_editing_button = new QToolButton(toolbar_);
    live_editing_button->setDefaultAction(act_live_editing_);
    live_editing_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    live_editing_button->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;background:transparent;min-width:0;max-width:none;min-height:%2px;max-height:%2px;padding:2px 8px;border:none;}"
        "QToolButton:hover{background:%3;border-radius:3px;}"
        "QToolButton:checked{background:%4;color:%5;border-radius:3px;}")
        .arg(button_text.name(QColor::HexRgb),
             QString::number(kEditorToolbarButtonExtent),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb)));
    toolbar_->addWidget(live_editing_button);

}

void TitleEditor::update_corner_toolbar()
{
    if ((!corner_toolbar_widget_ && !point_toolbar_widget_) || !canvas_)
        return;
    QScopedValueRollback<bool> guard(updating_corner_toolbar_, true);

    const bool point_available = canvas_->point_controls_available();
    const bool corner_available = canvas_->corner_controls_available();
    const bool context_available = point_available || corner_available;

    if (point_toolbar_widget_)
        point_toolbar_widget_->setVisible(point_available);
    if (corner_toolbar_widget_)
        corner_toolbar_widget_->setVisible(corner_available);

    /*
     * QWidgetAction visibility must be changed directly.  Showing only its
     * default widget is not sufficient after the action has been laid out as
     * hidden/zero-width by QToolBar, particularly with the Windows style.
     */
    if (dynamic_toolbar_action_)
        dynamic_toolbar_action_->setVisible(context_available);
    else if (dynamic_toolbar_widget_)
        dynamic_toolbar_widget_->setVisible(context_available);
    if (dynamic_toolbar_separator_)
        dynamic_toolbar_separator_->setVisible(context_available);

    if (context_available && dynamic_toolbar_widget_) {
        dynamic_toolbar_widget_->adjustSize();
        dynamic_toolbar_widget_->updateGeometry();
    }
    if (toolbar_) {
        toolbar_->updateGeometry();
        toolbar_->update();
    }

    if (point_available) {
        bool x_mixed = false;
        bool y_mixed = false;
        const QPointF position = canvas_->point_control_position(&x_mixed, &y_mixed);
        if (point_toolbar_x_)
            point_toolbar_x_->setValue(x_mixed ? point_toolbar_x_->minimum() : position.x());
        if (point_toolbar_y_)
            point_toolbar_y_->setValue(y_mixed ? point_toolbar_y_->minimum() : position.y());
        bool smooth_mixed = false;
        const bool smooth = canvas_->point_control_smooth(&smooth_mixed);
        if (point_toolbar_corner_)
            point_toolbar_corner_->setChecked(!smooth_mixed && !smooth);
        if (point_toolbar_smooth_)
            point_toolbar_smooth_->setChecked(!smooth_mixed && smooth);
        if (point_toolbar_handles_)
            point_toolbar_handles_->setChecked(canvas_->point_control_show_handles());
    }

    if (!corner_available)
        return;

    bool radius_mixed = false;
    const double radius = canvas_->corner_control_radius(&radius_mixed);
    if (corner_toolbar_radius_)
        corner_toolbar_radius_->setValue(radius_mixed ? -1.0 : radius);

    bool sync_mixed = false;
    const bool sync = canvas_->corner_control_sync(&sync_mixed);
    if (corner_toolbar_sync_) {
        corner_toolbar_sync_->setCheckState(sync_mixed ? Qt::PartiallyChecked
                                                       : (sync ? Qt::Checked : Qt::Unchecked));
    }

    bool type_mixed = false;
    const CornerType type = canvas_->corner_control_type(&type_mixed);
    if (corner_toolbar_type_) {
        const int index = type_mixed ? -1 : corner_toolbar_type_->findData((int)type);
        corner_toolbar_type_->setCurrentIndex(index);
    }
}


static QString editor_template_library_root_path()
{
    char *path = obs_module_config_path("template-library");
    QString root = path ? QString::fromUtf8(path) : QDir::homePath();
    if (path) bfree(path);
    QDir().mkpath(root);
    return root;
}

static int editor_dialog_layout_spacing(QWidget *widget)
{
    const int spacing = widget->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, widget);
    return spacing > 0 ? spacing : 6;
}

static QStringList editor_template_library_category_paths(const QString &root_path)
{
    QStringList categories;
    QDirIterator it(root_path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(root_path).relativeFilePath(it.filePath());
        if (!rel.isEmpty()) categories << rel;
    }
    if (categories.isEmpty()) categories << obsgs_tr("OBSTitles.Custom");
    categories.sort(Qt::CaseInsensitive);
    return categories;
}

static QString editor_sanitized_template_category_path(const QString &category)
{
    QString cleaned = QDir::cleanPath(category.trimmed());
    cleaned.replace(QRegularExpression(QStringLiteral("[\\\\:*?\"<>|]")), QStringLiteral("_"));
    while (cleaned.startsWith(QStringLiteral("../")))
        cleaned.remove(0, 3);
    if (cleaned == QStringLiteral(".") || cleaned == QStringLiteral(".."))
        cleaned.clear();
    return cleaned;
}

static bool prompt_editor_template_library_category(QWidget *parent, QString &category)
{
    const QString root_path = editor_template_library_root_path();
    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(editor_dialog_layout_spacing(&dialog));

    auto *prompt = new QLabel(obsgs_tr("OBSTitles.TemplateLibraryCategoryPrompt"), &dialog);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    auto *combo = new QComboBox(&dialog);
    combo->setEditable(true);
    combo->addItems(editor_template_library_category_paths(root_path));
    layout->addWidget(combo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString safe_category = editor_sanitized_template_category_path(combo->currentText());
        if (safe_category.isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"),
                                 obsgs_tr("OBSTitles.TemplateLibraryCategoryRequired"));
            return;
        }
        category = safe_category;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    return dialog.exec() == QDialog::Accepted;
}

static bool prompt_editor_template_metadata(QWidget *parent, const Title &title,
                                           TitleTemplateExportMetadata &metadata,
                                           const QString &window_title = obsgs_tr("OBSTitles.ExportTemplateDetails"))
{
    if (metadata.title.empty()) metadata.title = title.name;
    if (metadata.description.empty()) metadata.description = title.description;
    if (metadata.creator.empty()) metadata.creator = title.creator;
    if (metadata.creation_date.empty()) {
        metadata.creation_date = title.creation_date.empty()
            ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()
            : title.creation_date;
    }
    if (metadata.screenshot_png_base64.empty())
        metadata.screenshot_png_base64 = title.preview_screenshot_png_base64;

    QDialog dialog(parent);
    dialog.setWindowTitle(window_title);
    dialog.setModal(true);
    dialog.resize(560, 500);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(editor_dialog_layout_spacing(&dialog));

    auto *preview_label = new QLabel(obsgs_tr("OBSTitles.TemplateScreenshotPreviewLabel"), &dialog);
    QFont label_font = preview_label->font();
    label_font.setBold(true);
    preview_label->setFont(label_font);
    layout->addWidget(preview_label);

    auto *preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(160);
    QPixmap pixmap;
    const QByteArray png = QByteArray::fromBase64(QByteArray::fromStdString(metadata.screenshot_png_base64));
    if (!png.isEmpty() && pixmap.loadFromData(png, "PNG"))
        preview->setPixmap(pixmap.scaled(QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    layout->addWidget(preview);

    auto *form = new QFormLayout();
    auto *title_edit = new QLineEdit(QString::fromStdString(metadata.title), &dialog);
    auto *description_edit = new QTextEdit(&dialog);
    description_edit->setAcceptRichText(false);
    description_edit->setPlainText(QString::fromStdString(metadata.description));
    description_edit->setMinimumHeight(96);
    auto *creator_edit = new QLineEdit(QString::fromStdString(metadata.creator), &dialog);
    auto *date_edit = new QLineEdit(QString::fromStdString(metadata.creation_date), &dialog);

    form->addRow(obsgs_tr("OBSTitles.TemplateExportTitleLabel"), title_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportDescriptionLabel"), description_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportCreatorLabel"), creator_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateCreationDateLabel"), date_edit);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.ExportTemplateDetails"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    metadata.title = title_edit->text().trimmed().toStdString();
    metadata.description = description_edit->toPlainText().trimmed().toStdString();
    metadata.creator = creator_edit->text().trimmed().toStdString();
    metadata.creation_date = date_edit->text().trimmed().toStdString();
    return true;
}

void TitleEditor::copy_title_to_store(const std::shared_ptr<Title> &source,
                                      const std::shared_ptr<Title> &dest) const
{
    if (!source || !dest) return;
    const std::string dest_id = dest->id;
    *dest = *source;
    dest->id = dest_id;
    dest->layers.clear();
    dest->layers.reserve(source->layers.size());
    for (const auto &layer : source->layers) {
        if (layer) dest->layers.push_back(std::make_shared<Layer>(*layer));
    }
}

void TitleEditor::new_title_contents()
{
    if (!title_) return;
    if (QMessageBox::question(this, obsgs_tr("OBSTitles.New"),
                              obsgs_tr("OBSTitles.NewTitleConfirm")) != QMessageBox::Yes)
        return;

    title_->layers.clear();
    sel_layer_id_.clear();
    layers_->refresh();
    canvas_->set_selected_layers({});
    update_layer_panels(nullptr, playhead_);
    on_title_modified();
}

bool TitleEditor::persist_title_changes(bool update_preview_screenshot, bool show_saved_status)
{
    if (!title_) return false;

    /* Saving can replace/copy the stored Title while a deferred cache render is
     * still holding the same title identity. Cancel only pending work for this
     * title first; cached frames remain available and are selectively
     * invalidated after the save through the normal modification path. */
    const QString cache_title_id = QString::fromStdString(
        editing_title_id_.empty() ? title_->id : editing_title_id_);
    CacheManager::instance().cancelTitleWork(cache_title_id);

    auto stored = TitleDataStore::instance().get_title(editing_title_id_.empty() ? title_->id : editing_title_id_);
    if (!stored) {
        stored = TitleDataStore::instance().create_title(title_->name);
        editing_title_id_ = stored->id;
        title_->id = stored->id;
    }
    copy_title_to_store(title_, stored);
    if (update_preview_screenshot) {
        // Reuse the already rendered editor frame instead of performing another
        // full title render during Save. This removes the largest UI-thread spike
        // for complex titles. Fall back only when no canvas frame exists yet.
        const QImage current_preview = canvas_ ? canvas_->current_rendered_frame() : QImage();
        title_->preview_screenshot_png_base64 = current_preview.isNull()
            ? title_manual_screenshot_png_base64(*title_)
            : title_screenshot_png_base64(current_preview);
        stored->preview_screenshot_png_base64 = title_->preview_screenshot_png_base64;
    }
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save_async();
    emit title_saved(stored->id);
    set_dirty(false);
    if (show_saved_status)
        setWindowTitle(obsgs_tr("OBSTitles.EditorSavedTitle"));
    return true;
}

bool TitleEditor::save_title()
{
    return persist_title_changes(true, true);
}

void TitleEditor::set_live_editing_enabled(bool enabled)
{
    live_editing_ = enabled;
    if (act_live_editing_ && act_live_editing_->isChecked() != enabled) {
        QSignalBlocker blocker(act_live_editing_);
        act_live_editing_->setChecked(enabled);
    }
    if (live_editing_ && dirty_)
        save_live_edit();
}

void TitleEditor::set_gpu_pipeline_enabled(bool enabled)
{
    if (TitlePreferences::use_gpu() == enabled)
        return;

    TitlePreferences::set_use_gpu(enabled);
    TitlePreferences::notify_changed(this);
    update_title_bar();
    if (canvas_)
        canvas_->refresh_preview();
}

void TitleEditor::save_live_edit()
{
    if (!live_editing_ || !title_) return;
    persist_title_changes(false, false);
}

void TitleEditor::save_title_as_new()
{
    if (!title_) return;

    Title temp = *title_;
    temp.preview_screenshot_png_base64 = title_manual_screenshot_png_base64(temp);
    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = temp.preview_screenshot_png_base64;
    if (!prompt_editor_template_metadata(this, temp, metadata, obsgs_tr("OBSTitles.SaveAsNew")))
        return;

    auto created = TitleDataStore::instance().create_title(metadata.title);
    title_->name = metadata.title;
    title_->description = metadata.description;
    title_->creator = metadata.creator;
    title_->creation_date = metadata.creation_date;
    title_->preview_screenshot_png_base64 = metadata.screenshot_png_base64;
    copy_title_to_store(title_, created);
    created->name = metadata.title;
    created->description = metadata.description;
    created->creator = metadata.creator;
    created->creation_date = metadata.creation_date;
    created->preview_screenshot_png_base64 = metadata.screenshot_png_base64;
    editing_title_id_ = created->id;
    title_->id = created->id;
    update_title_bar();
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save_async();
    emit title_saved(created->id);
    set_dirty(false);
    setWindowTitle(obsgs_tr("OBSTitles.EditorSavedTitle"));
}

void TitleEditor::export_title_template(bool save_in_library)
{
    if (!title_) return;

    Title temp = *title_;
    temp.preview_screenshot_png_base64 = title_manual_screenshot_png_base64(temp);
    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = temp.preview_screenshot_png_base64;
    if (!prompt_editor_template_metadata(this, temp, metadata))
        return;

    QString safe_name = QString::fromStdString(metadata.title).trimmed();
    if (safe_name.isEmpty()) safe_name = obsgs_tr("OBSTitles.TemplateFileDialogTitle");
    safe_name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));

    QString path;
    if (save_in_library) {
        QString category;
        if (!prompt_editor_template_library_category(this, category))
            return;
        QDir root(editor_template_library_root_path());
        root.mkpath(category);
        path = root.filePath(QStringLiteral("%1/%2.ogspt").arg(category, safe_name));
    } else {
        path = QFileDialog::getSaveFileName(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                                            QDir(editor_template_library_root_path()).filePath(safe_name + QStringLiteral(".ogspt")),
                                            obsgs_tr("OBSTitles.TemplateFileFilter"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".ogspt");
    }

    auto stored = TitleDataStore::instance().create_title(metadata.title);
    copy_title_to_store(title_, stored);
    stored->name = metadata.title;
    stored->description = metadata.description;
    stored->creator = metadata.creator;
    stored->creation_date = metadata.creation_date;
    stored->preview_screenshot_png_base64 = metadata.screenshot_png_base64;

    std::string error;
    if (!TitleDataStore::instance().export_title(stored->id, path.toStdString(), metadata, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"), QString::fromStdString(error));
    } else {
        QMessageBox::information(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                                 obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
    }
    TitleDataStore::instance().delete_title(stored->id);
}

/* ── open_title ──────────────────────────────────────────────────── */
void TitleEditor::open_title(const std::string &tid)
{
    play_timer_->stop();
    if (cache_invalidation_timer_)
        cache_invalidation_timer_->stop();
    CacheManager::instance().setInteractiveBypass(false);
    playing_ = false;
    act_play_->setText("▶");
    act_play_->setIcon(obs_icon("play.svg"));
    playhead_ = 0.0;
    playback_reverse_ = false;
    manual_reverse_playback_ = false;
    full_loop_playback_ = false;

    auto stored_title = TitleDataStore::instance().get_title(tid);
    if (!stored_title) return;
    CacheManager::instance().setEditorPrerenderFocus(QString::fromStdString(tid), true);
    editing_title_id_ = tid;
    title_ = clone_title(*stored_title);
    load_new_layer_defaults();

    update_title_bar();
    canvas_->set_title(title_);
    layers_->set_title(title_);
    layers_->set_layer_clipboard_available(!layer_clipboard_.empty());
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);
    refresh_color_swatches_panel();
    if (prerender_panel_) prerender_panel_->setTitle(title_);
    CacheManager::instance().restoreDiskStates(title_);
    CacheManager::instance().reprioritize(title_, playhead_);
    // Opening the editor is an explicit request to make this title responsive.
    // Queue the remaining timeline immediately; the cache scheduler will suspend
    // non-urgent live-cue/background jobs while this editor focus is active.
    CacheManager::instance().queueWholeTimeline(title_);

    if (!title_->layers.empty())
        on_layer_selected(title_->layers.back()->id);
    else
        update_layer_panels(nullptr, playhead_);

    QTimer::singleShot(0, timeline_, [this]() {
        if (timeline_) timeline_->fit_timeline();
    });

    undo_stack_.clear();
    undo_index_ = -1;
    push_undo_snapshot();
    update_undo_redo_actions();

    on_playhead_changed(0.0);
    set_dirty(false);
}

std::shared_ptr<Title> TitleEditor::clone_title(const Title &title) const
{
    auto clone = std::make_shared<Title>(title);
    clone->layers.clear();
    clone->layers.reserve(title.layers.size());
    for (const auto &layer : title.layers) {
        if (layer) clone->layers.push_back(std::make_shared<Layer>(*layer));
    }
    return clone;
}


std::shared_ptr<Layer> TitleEditor::clone_layer_for_insert(const Layer &layer, bool suffix_name) const
{
    auto clone = std::make_shared<Layer>(layer);
    clone->id = TitleDataStore::make_uuid();
    if (suffix_name)
        clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy") : clone->name + editor_text_std("OBSTitles.CopySuffix");
    clone->name = unique_layer_name(clone->name);
    if (!clone->parent_id.empty() && (!title_ || !title_->find_layer(clone->parent_id)))
        clone->parent_id.clear();
    if (!clone->mask_source_id.empty() && (!title_ || !title_->find_layer(clone->mask_source_id))) {
        clone->mask_source_id.clear();
        clone->mask_mode = MaskMode::None;
    }
    return clone;
}

std::string TitleEditor::unique_layer_name(const std::string &base_name,
                                           const std::set<std::string> &exclude_ids,
                                           std::set<std::string> *reserved_names) const
{
    std::string base = QString::fromStdString(base_name).trimmed().toStdString();
    if (base.empty())
        base = editor_text_std("OBSTitles.Layer");

    std::set<std::string> used;
    if (title_) {
        for (const auto &layer : title_->layers) {
            if (!layer || exclude_ids.find(layer->id) != exclude_ids.end()) continue;
            used.insert(layer->name);
        }
    }
    if (reserved_names)
        used.insert(reserved_names->begin(), reserved_names->end());

    auto available = [&](const std::string &candidate) {
        return used.find(candidate) == used.end();
    };
    if (available(base)) {
        if (reserved_names) reserved_names->insert(base);
        return base;
    }

    for (int suffix = 2; suffix < 10000; ++suffix) {
        const QString candidate = QStringLiteral("%1 %2")
            .arg(QString::fromStdString(base))
            .arg(suffix, 2, 10, QChar('0'));
        const std::string value = candidate.toStdString();
        if (!available(value)) continue;
        if (reserved_names) reserved_names->insert(value);
        return value;
    }

    const std::string fallback = base + " " + TitleDataStore::make_uuid().substr(0, 8);
    if (reserved_names) reserved_names->insert(fallback);
    return fallback;
}

void TitleEditor::insert_layer_above(const std::string &anchor_id, std::shared_ptr<Layer> layer)
{
    if (!title_ || !layer) return;

    auto it = std::find_if(title_->layers.begin(), title_->layers.end(),
                           [&](const auto &candidate) {
                               return candidate && candidate->id == anchor_id;
                           });
    if (it == title_->layers.end())
        title_->layers.push_back(layer);
    else
        title_->layers.insert(it + 1, layer);
}

void TitleEditor::select_after_layer_list_mutation(const std::string &layer_id)
{
    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer_id);
}


std::vector<std::string> TitleEditor::selected_layer_ids_for_operation() const
{
    std::vector<std::string> requested = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
    if (requested.empty() && !sel_layer_id_.empty())
        requested.push_back(sel_layer_id_);

    std::set<std::string> requested_set(requested.begin(), requested.end());
    std::vector<std::string> ordered_ids;
    if (!title_ || requested_set.empty()) return ordered_ids;

    for (const auto &layer : title_->layers) {
        if (layer && requested_set.find(layer->id) != requested_set.end())
            ordered_ids.push_back(layer->id);
    }
    return ordered_ids;
}

std::vector<std::shared_ptr<Layer>> TitleEditor::clone_layers_for_insert(const std::vector<std::shared_ptr<Layer>> &layers,
                                                                         bool suffix_name) const
{
    std::map<std::string, std::string> cloned_ids_by_original;
    std::vector<std::shared_ptr<Layer>> clones;
    clones.reserve(layers.size());
    std::set<std::string> reserved_names;

    for (const auto &layer : layers) {
        if (!layer) continue;
        auto clone = std::make_shared<Layer>(*layer);
        clone->id = TitleDataStore::make_uuid();
        if (suffix_name)
            clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy")
                                              : clone->name + editor_text_std("OBSTitles.CopySuffix");
        clone->name = unique_layer_name(clone->name, {}, &reserved_names);
        cloned_ids_by_original[layer->id] = clone->id;
        clones.push_back(clone);
    }

    for (auto &clone : clones) {
        auto parent_clone = cloned_ids_by_original.find(clone->parent_id);
        if (parent_clone != cloned_ids_by_original.end()) {
            clone->parent_id = parent_clone->second;
        } else if (!clone->parent_id.empty() && (!title_ || !title_->find_layer(clone->parent_id))) {
            clone->parent_id.clear();
        }

        auto mask_clone = cloned_ids_by_original.find(clone->mask_source_id);
        if (mask_clone != cloned_ids_by_original.end()) {
            clone->mask_source_id = mask_clone->second;
        } else if (!clone->mask_source_id.empty() && (!title_ || !title_->find_layer(clone->mask_source_id))) {
            clone->mask_source_id.clear();
            clone->mask_mode = MaskMode::None;
        }
    }

    return clones;
}

void TitleEditor::apply_picked_color_to_selection(const QColor &color)
{
    if (!title_ || !color.isValid()) return;

    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    const uint32_t argb = argb_from_color(color);
    std::shared_ptr<Layer> last_changed;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;

        const double local_time = std::clamp(playhead_ - layer->in_time, 0.0,
                                             std::max(0.0, layer->out_time - layer->in_time));
        if (is_canvas_text_layer(*layer)) {
            RichTextCharFormat fmt = layer_char_format_for_editor(*layer);
            fmt.fill.type = 0;
            fmt.fill.color = argb;
            apply_rich_text_format_to_layer_range(*layer, fmt, RichTextCharFillColor, false);
            set_color_channels_at(*layer, true, local_time, argb);
            last_changed = layer;
        } else if (layer->type == LayerType::Shape || layer->type == LayerType::SolidRect) {
            layer->fill_type = 0;
            layer->fill_color = argb;
            set_color_channels_at(*layer, false, local_time, argb);
            last_changed = layer;
        }
    }

    if (!last_changed) return;
    on_title_modified();
    update_layer_panels(last_changed, playhead_);
    if (reopen_color_tab_after_canvas_pick_) {
        reopen_color_tab_after_canvas_pick_ = false;
        if (props_) props_->open_foreground_color_selector();
    }
}

void TitleEditor::duplicate_selected_layers()
{
    if (!title_) return;
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    std::set<std::string> selected_ids(ids.begin(), ids.end());
    std::vector<std::shared_ptr<Layer>> originals;
    originals.reserve(ids.size());
    for (const auto &layer : title_->layers) {
        if (layer && selected_ids.find(layer->id) != selected_ids.end())
            originals.push_back(layer);
    }

    auto clones = clone_layers_for_insert(originals, true);
    if (clones.empty()) return;

    std::map<std::string, std::shared_ptr<Layer>> clones_by_original;
    for (size_t i = 0; i < originals.size() && i < clones.size(); ++i)
        clones_by_original[originals[i]->id] = clones[i];

    std::vector<std::shared_ptr<Layer>> next_layers;
    next_layers.reserve(title_->layers.size() + clones.size());
    for (const auto &layer : title_->layers) {
        next_layers.push_back(layer);
        if (!layer) continue;
        auto clone = clones_by_original.find(layer->id);
        if (clone != clones_by_original.end())
            next_layers.push_back(clone->second);
    }
    title_->layers = std::move(next_layers);

    std::vector<std::string> clone_ids;
    clone_ids.reserve(clones.size());
    for (const auto &clone : clones)
        if (clone) clone_ids.push_back(clone->id);

    sel_layer_id_ = clone_ids.empty() ? std::string() : clone_ids.back();
    layers_->refresh();
    layers_->set_selected_layers(clone_ids);
    canvas_->set_selected_layers(clone_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(clone_ids);
    if (!sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) update_layer_panels(layer, playhead_);
    }
    on_title_modified();
}

void TitleEditor::copy_selected_layer()
{
    if (!title_) return;
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    layer_clipboard_.clear();
    layer_clipboard_.reserve(ids.size());
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (layer) layer_clipboard_.push_back(std::make_shared<Layer>(*layer));
    }
    if (layers_) layers_->set_layer_clipboard_available(!layer_clipboard_.empty());
}

void TitleEditor::paste_layer_from_clipboard()
{
    if (!title_ || layer_clipboard_.empty()) return;

    std::vector<std::shared_ptr<Layer>> clipboard_layers;
    clipboard_layers.reserve(layer_clipboard_.size());
    for (const auto &layer : layer_clipboard_)
        if (layer) clipboard_layers.push_back(layer);

    auto pasted_layers = clone_layers_for_insert(clipboard_layers, true);
    if (pasted_layers.empty()) return;

    auto insert_pos = title_->layers.end();
    if (!sel_layer_id_.empty()) {
        auto anchor = std::find_if(title_->layers.begin(), title_->layers.end(),
                                   [this](const auto &layer) { return layer && layer->id == sel_layer_id_; });
        if (anchor != title_->layers.end()) insert_pos = anchor + 1;
    }
    title_->layers.insert(insert_pos, pasted_layers.begin(), pasted_layers.end());

    std::vector<std::string> pasted_ids;
    pasted_ids.reserve(pasted_layers.size());
    for (const auto &layer : pasted_layers)
        if (layer) pasted_ids.push_back(layer->id);

    sel_layer_id_ = pasted_ids.empty() ? std::string() : pasted_ids.back();
    layers_->refresh();
    layers_->set_selected_layers(pasted_ids);
    canvas_->set_selected_layers(pasted_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(pasted_ids);
    if (!sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) update_layer_panels(layer, playhead_);
    }
    on_title_modified();
}



bool TitleEditor::paste_external_clipboard_to_canvas()
{
    if (!title_)
        return false;
    const QMimeData *mime = QApplication::clipboard() ? QApplication::clipboard()->mimeData() : nullptr;
    if (!mime)
        return false;

    const QPointF canvas_pt = canvas_ ? canvas_->view_center_canvas_point()
                                      : QPointF(title_->width * 0.5, title_->height * 0.5);

    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            QImageReader reader(path);
            if (!reader.canRead())
                continue;
            create_image_layer_from_external_source(path, canvas_pt);
            return true;
        }
    }

    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            QString base_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
            if (base_dir.isEmpty())
                base_dir = QDir::tempPath();
            QDir dir(base_dir + QStringLiteral("/obs-gsp-pasted-assets"));
            if (!dir.exists())
                dir.mkpath(QStringLiteral("."));
            const QString path = dir.filePath(QStringLiteral("pasted-image-%1.png")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"))));
            if (image.save(path, "PNG")) {
                create_image_layer_from_external_source(path, canvas_pt);
                return true;
            }
        }
    }

    if (mime->hasText()) {
        const QString text = mime->text().trimmed();
        if (!text.isEmpty()) {
            create_text_layer_from_external_source(text, canvas_pt);
            return true;
        }
    }

    return false;
}

void TitleEditor::delete_selected_layer()
{
    if (!title_) return;
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    std::set<std::string> removed_ids(ids.begin(), ids.end());
    int first_removed_index = (int)title_->layers.size();
    std::vector<std::shared_ptr<Layer>> remaining;
    remaining.reserve(title_->layers.size());
    for (int i = 0; i < (int)title_->layers.size(); ++i) {
        auto &layer = title_->layers[(size_t)i];
        if (!layer || removed_ids.find(layer->id) != removed_ids.end()) {
            first_removed_index = std::min(first_removed_index, i);
            continue;
        }
        remaining.push_back(layer);
    }

    if (remaining.size() == title_->layers.size()) return;

    for (auto &layer : remaining) {
        if (!layer) continue;
        if (removed_ids.find(layer->parent_id) != removed_ids.end()) layer->parent_id.clear();
        if (removed_ids.find(layer->mask_source_id) != removed_ids.end()) {
            layer->mask_source_id.clear();
            layer->mask_mode = MaskMode::None;
        }
    }

    title_->layers = std::move(remaining);
    sel_layer_id_.clear();
    layers_->refresh();

    if (!title_->layers.empty()) {
        const int select_index = std::clamp(first_removed_index, 0, (int)title_->layers.size() - 1);
        on_layer_selected(title_->layers[(size_t)select_index]->id);
    } else {
        layers_->set_selected_layers({});
        canvas_->set_selected_layers({});
        timeline_->set_title(title_);
        timeline_->set_selected_layers({});
        update_layer_panels(nullptr, playhead_);

        /* Deleting the final layer is a special hard-empty transition.  Clear
         * the editor artwork synchronously and discard every RAM/disk frame for
         * this title before any delayed invalidation or repaint can reuse the
         * last valid cached image.  The CanvasPreview itself stays alive and
         * continues painting the checkerboard/editor chrome. */
        canvas_->clear_rendered_frame();
        CacheManager::instance().removeTitleCache(QString::fromStdString(title_->id), true);
    }

    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::cut_selected_layer()
{
    copy_selected_layer();
    delete_selected_layer();
}

void TitleEditor::push_undo_snapshot()
{
    if (!title_ || restoring_undo_) return;
    if (undo_index_ + 1 < (int)undo_stack_.size())
        undo_stack_.erase(undo_stack_.begin() + undo_index_ + 1, undo_stack_.end());
    undo_stack_.push_back(clone_title(*title_));
    if (undo_stack_.size() > 30)
        undo_stack_.erase(undo_stack_.begin());
    undo_index_ = (int)undo_stack_.size() - 1;
    update_undo_redo_actions();
}

void TitleEditor::restore_undo_snapshot(int index)
{
    if (!title_ || index < 0 || index >= (int)undo_stack_.size()) return;
    restoring_undo_ = true;
    auto snapshot = undo_stack_[(size_t)index];
    title_->name = snapshot->name;
    title_->description = snapshot->description;
    title_->creator = snapshot->creator;
    title_->creation_date = snapshot->creation_date;
    title_->duration = snapshot->duration;
    title_->loop_start = snapshot->loop_start;
    title_->loop_end = snapshot->loop_end;
    title_->playback_mode = snapshot->playback_mode;
    title_->loop_type = snapshot->loop_type;
    title_->pause_time = snapshot->pause_time;
    title_->bg_color = snapshot->bg_color;
    title_->width = snapshot->width;
    title_->height = snapshot->height;
    title_->live_text_rows = snapshot->live_text_rows;
    title_->live_text_row_ids = snapshot->live_text_row_ids;
    title_->live_text_column_order = snapshot->live_text_column_order;
    title_->live_text_header_state = snapshot->live_text_header_state;
    title_->external_data_enabled = snapshot->external_data_enabled;
    title_->current_cue_row = snapshot->current_cue_row;
    title_->pending_cue_row = snapshot->pending_cue_row;
    title_->cue_revision = snapshot->cue_revision;
    title_->layers.clear();
    title_->layers.reserve(snapshot->layers.size());
    for (const auto &layer : snapshot->layers) {
        if (layer) title_->layers.push_back(std::make_shared<Layer>(*layer));
    }
    undo_index_ = index;
    if (!sel_layer_id_.empty() && !title_->find_layer(sel_layer_id_))
        sel_layer_id_.clear();
    if (sel_layer_id_.empty() && !title_->layers.empty())
        sel_layer_id_ = title_->layers.back()->id;
    update_title_bar();
    canvas_->set_title(title_, true);
    layers_->set_title(title_);
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);
    if (!sel_layer_id_.empty()) on_layer_selected(sel_layer_id_);
    else update_layer_panels(nullptr, playhead_);
    on_playhead_changed(std::clamp(playhead_, 0.0, title_->duration));
    restoring_undo_ = false;
    update_undo_redo_actions();
    set_dirty(true);
    save_live_edit();
}

void TitleEditor::update_undo_redo_actions()
{
    if (act_undo_) act_undo_->setEnabled(undo_index_ > 0);
    if (act_redo_) act_redo_->setEnabled(undo_index_ >= 0 && undo_index_ + 1 < (int)undo_stack_.size());
}

void TitleEditor::update_title_bar()
{
    if (title_ && title_lbl_)
        title_lbl_->setText(QString::fromStdString(title_->name));
    if (graphic_props_dock_) {
        graphic_props_dock_->setWindowTitle(title_
            ? obsgs_tr("OBSTitles.PropertiesNamed").arg(QString::fromStdString(title_->name))
            : obsgs_tr("OBSTitles.Properties"));
    }
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
    if (gpu_warning_lbl_) {
        const bool show_gpu_warning = TitlePreferences::use_gpu() && !TitlePreferences::gpu_available();
        gpu_warning_lbl_->setVisible(show_gpu_warning);
        gpu_warning_lbl_->setText(show_gpu_warning ? obsgs_tr("OBSTitles.GPUFallbackWarning") : QString());
        gpu_warning_lbl_->setToolTip(show_gpu_warning
            ? QString::fromUtf8(TitlePreferences::gpu_unavailable_reason())
            : QString());
    }
}

void TitleEditor::begin_title_name_edit()
{
    if (!title_ || !title_lbl_ || !title_name_edit_)
        return;

    title_name_edit_->setText(QString::fromStdString(title_->name));
    title_lbl_->hide();
    title_name_edit_->show();
    title_name_edit_->setFocus(Qt::MouseFocusReason);
    title_name_edit_->selectAll();
}

void TitleEditor::commit_title_name_edit(bool accept)
{
    if (!title_name_edit_ || !title_name_edit_->isVisible())
        return;

    const QString next_name = title_name_edit_->text().trimmed();
    title_name_edit_->hide();
    if (title_lbl_)
        title_lbl_->show();

    if (accept && title_ && !next_name.isEmpty() && next_name.toStdString() != title_->name) {
        title_->name = next_name.toStdString();
        update_title_bar();
        if (title_props_)
            title_props_->set_title(title_);
        set_dirty(true);
        push_undo_snapshot();
        save_live_edit();
    } else {
        update_title_bar();
    }
}

void TitleEditor::set_dirty(bool dirty)
{
    dirty_ = dirty;
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
    update_title_bar();
    setWindowTitle(obsgs_tr(dirty_ ? "OBSTitles.EditorModifiedTitle" : "OBSTitles.EditorWindowTitle"));
}

bool TitleEditor::confirm_save_before_close()
{
    if (!dirty_)
        return true;

    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.UnsavedChangesTitle"));
    dialog.setText(obsgs_tr("OBSTitles.UnsavedChangesPrompt"));
    dialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    dialog.setDefaultButton(QMessageBox::Yes);
    dialog.setEscapeButton(QMessageBox::Cancel);

    const auto result = static_cast<QMessageBox::StandardButton>(dialog.exec());
    if (result == QMessageBox::Yes)
        return save_title();
    if (result == QMessageBox::No)
        return true;
    return false;
}

/* ── Transport ───────────────────────────────────────────────────── */
void TitleEditor::play_pause()
{
    if (!title_) return;
    if (!playing_) {
        full_loop_playback_ = false;
        manual_reverse_playback_ = false;
        playback_reverse_ = false;
    }
    playing_ = !playing_;
    if (playing_) {
        const CachePlaybackSettings cache_settings = CacheManager::instance().playbackSettings();
        if (cache_settings.from_beginning)
            on_playhead_changed(0.0);
        if (title_->playback_mode != 2 && playhead_ >= title_->duration)
            on_playhead_changed(0.0);
        if (title_->playback_mode == 2 && playhead_ >= std::clamp(title_->pause_time, 0.0, title_->duration))
            on_playhead_changed(0.0);
        act_play_->setText("⏸");
        act_play_->setIcon(obs_icon("pause.svg"));
        playback_clock_.restart();
        play_timer_->start();
    } else {
        act_play_->setText("▶");
        act_play_->setIcon(obs_icon("play.svg"));
        play_timer_->stop();
    }
}

void TitleEditor::play_full_loop()
{
    if (!title_) return;
    full_loop_playback_ = true;
    playback_reverse_ = false;
    if (!playing_ || playhead_ >= title_->duration)
        on_playhead_changed(0.0);
    playing_ = true;
    act_play_->setText("⏸");
    act_play_->setIcon(obs_icon("pause.svg"));
    playback_clock_.restart();
    play_timer_->start();
}

void TitleEditor::reverse_play()
{
    if (!title_) return;
    if (playing_ && manual_reverse_playback_) {
        playing_ = false;
        manual_reverse_playback_ = false;
        play_timer_->stop();
        act_play_->setText("▶");
        act_play_->setIcon(obs_icon("play.svg"));
        return;
    }
    full_loop_playback_ = false;
    manual_reverse_playback_ = true;
    playback_reverse_ = true;
    if (playhead_ <= 0.0)
        on_playhead_changed(title_->duration);
    playing_ = true;
    act_play_->setText("⏸");
    act_play_->setIcon(obs_icon("pause.svg"));
    playback_clock_.restart();
    play_timer_->start();
}

void TitleEditor::go_to_start()
{
    manual_reverse_playback_ = false;
    playback_reverse_ = false;
    on_playhead_changed(0.0);
}

void TitleEditor::go_to_end()
{
    if (!title_) return;
    manual_reverse_playback_ = false;
    playback_reverse_ = false;
    on_playhead_changed(title_->duration);
}

void TitleEditor::step_backward()
{
    if (!title_) return;
    on_playhead_changed(std::max(0.0, snap_to_obs_frame(playhead_ - obs_frame_duration())));
}

void TitleEditor::step_forward()
{
    if (!title_) return;
    on_playhead_changed(std::min(snap_to_obs_frame(playhead_ + obs_frame_duration()), title_->duration));
}


static void collect_timeline_keyframes(const std::shared_ptr<Layer> &layer,
                                       std::vector<double> &times)
{
    if (!layer) return;
    for (auto prop : timeline_properties(*layer)) {
        for (size_t i = 0; i < prop.keyframe_count(); ++i)
            times.push_back(layer->in_time + prop.keyframe_time(i));
    }
}

void TitleEditor::previous_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = -1.0;
    for (double t : times) {
        if (t < playhead_ - kEpsilon)
            target = std::max(target, t);
    }
    if (target >= 0.0) on_playhead_changed(target);
}

void TitleEditor::next_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = title_->duration + 1.0;
    for (double t : times) {
        if (t > playhead_ + kEpsilon)
            target = std::min(target, t);
    }
    if (target <= title_->duration) on_playhead_changed(target);
}


namespace {
bool cached_preview_frame_available_for_playback(const std::shared_ptr<Title> &title, double time)
{
    if (!title)
        return false;
    /* This intentionally goes through the editor-facing cache API.  It can
     * synchronously promote an already-rendered disk frame to RAM, queues the
     * missing frame when needed, and returns a null image only when the user
     * requested "Play after rendering" and that exact frame is not available
     * yet.  Non-cacheable dynamic titles still return a live uncached frame, so
     * clock/ticker previews are not blocked. */
    return !CacheManager::instance().requestFrame(title, time, true).isNull();
}
}

void TitleEditor::tick()
{
    if (!title_ || !playing_) return;
    const CachePlaybackSettings cache_settings = CacheManager::instance().playbackSettings();
    double dt = obs_frame_duration();
    if (!cache_settings.play_every_frame || obs_frame_duration() < editor_playback_ui_frame_duration()) {
        dt = playback_clock_.isValid() ? playback_clock_.restart() / 1000.0 : 0.0;
        if (dt <= 0.0 || dt > 0.25) dt = play_timer_->interval() / 1000.0;
        dt *= std::max(0.01, cache_settings.speed_percent / 100.0);
        if (cache_settings.skip_frames > 0)
            dt += obs_frame_duration() * cache_settings.skip_frames;
    } else if (playback_clock_.isValid()) {
        playback_clock_.restart();
    }

    double duration = std::max(0.001, title_->duration);
    double loop_start = std::clamp(title_->loop_start, 0.0, title_->duration);
    double loop_end = std::clamp(title_->loop_end, loop_start, title_->duration);
    double loop_len = std::max(0.001, loop_end - loop_start);
    double t = playhead_;

    if (manual_reverse_playback_) {
        t = playhead_ - dt;
        if (t <= 0.0) {
            t = 0.0;
            playing_ = false;
            manual_reverse_playback_ = false;
            playback_reverse_ = false;
            play_timer_->stop();
            act_play_->setText("▶");
            act_play_->setIcon(obs_icon("play.svg"));
        }
        const double next_playhead = snap_to_obs_frame(t);
        if (cache_settings.cached_frames_only &&
            !cached_preview_frame_available_for_playback(title_, next_playhead))
            return;
        on_playhead_changed(next_playhead);
        return;
    }

    const bool follow_title_mode = cache_settings.follow_title_playback_mode;
    const bool preview_loop = (!follow_title_mode && cache_settings.mode == CachePlaybackMode::Loop) || full_loop_playback_;
    const bool preview_ping_pong = !follow_title_mode && cache_settings.mode == CachePlaybackMode::PingPong;
    const bool preview_play_once = !follow_title_mode && cache_settings.mode == CachePlaybackMode::PlayOnce;
    if (preview_loop && !preview_ping_pong && full_loop_playback_) {
        t = std::fmod(playhead_ + dt, duration);
    } else if (preview_ping_pong) {
        t += (playback_reverse_ ? -dt : dt);
        if (!playback_reverse_ && t >= duration) {
            t = duration - std::fmod(t - duration, duration);
            playback_reverse_ = true;
        } else if (playback_reverse_ && t <= 0.0) {
            t = std::fmod(-t, duration);
            playback_reverse_ = false;
        }
    } else if (preview_loop) {
        t = std::fmod(playhead_ + dt, duration);
    } else if (preview_play_once) {
        t = playhead_ + dt;
        if (t >= title_->duration) {
            t = title_->duration;
            playing_ = false;
            play_timer_->stop();
            act_play_->setText("▶");
            act_play_->setIcon(obs_icon("play.svg"));
        }
    } else {
        switch (title_->playback_mode) {
        case 1: /* Loop in/out between Loop Start and Loop End */
            if (loop_end <= loop_start + 0.0001) {
                t = std::fmod(playhead_ + dt, duration);
            } else if (title_->loop_type == 1) {
                t += (playback_reverse_ ? -dt : dt);
                if (!playback_reverse_ && t >= loop_end) {
                    t = loop_end - std::fmod(t - loop_end, loop_len);
                    playback_reverse_ = true;
                } else if (playback_reverse_ && t <= loop_start) {
                    t = loop_start + std::fmod(loop_start - t, loop_len);
                    playback_reverse_ = false;
                }
            } else {
                t = playhead_ + dt;
                if (t >= loop_end)
                    t = loop_start + std::fmod(t - loop_end, loop_len);
            }
            break;
        case 2: { /* Pause at timeline position */
            double pause_time = std::clamp(title_->pause_time, 0.0, title_->duration);
            t = playhead_ + dt;
            if (t >= pause_time) {
                t = pause_time;
                playing_ = false;
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
        default: /* Play once */
            t = playhead_ + dt;
            if (t >= title_->duration) {
                t = title_->duration;
                playing_ = false;
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
    }
    const double next_playhead = snap_to_obs_frame(t);
    if (cache_settings.cached_frames_only &&
        !cached_preview_frame_available_for_playback(title_, next_playhead)) {
        /* Play after rendering means the playhead must not outrun the cache.
         * Keep the current visible frame and let the realtime queue prepare the
         * exact next frame; the next timer tick will advance as soon as it is
         * available. */
        return;
    }
    on_playhead_changed(next_playhead);
}

static bool editor_focus_accepts_text(QWidget *widget)
{
    return qobject_cast<QLineEdit *>(widget) ||
           qobject_cast<QTextEdit *>(widget) ||
           qobject_cast<QAbstractSpinBox *>(widget) ||
           qobject_cast<QComboBox *>(widget);
}

void TitleEditor::show_about()
{
    QMessageBox::about(
        this,
        obsgs_tr("OBSTitles.AboutTitle"),
        obsgs_tr("OBSTitles.AboutTextFormat").arg(QStringLiteral(PLUGIN_VERSION)));
}

void TitleEditor::show_preferences()
{
    show_preferences_dialog(this, this);
}

void TitleEditor::show_global_preferences(QWidget *parent)
{
    show_preferences_dialog(parent, nullptr);
}

void TitleEditor::show_preferences_dialog(QWidget *parent, TitleEditor *editor)
{
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(obsgs_tr("OBSTitles.PreferencesWindowTitle"));
    dialog->setModal(false);
    dialog->resize(700, 540);

    const QPalette pal = dialog->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor disabled_text = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *content = new QWidget(dialog);
    auto *content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(12);

    auto *tabs = new QListWidget(content);
    tabs->setFixedWidth(138);
    tabs->setFrameShape(QFrame::NoFrame);
    tabs->setSelectionMode(QAbstractItemView::SingleSelection);
    tabs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addItem(obsgs_tr("OBSTitles.Appearance"));
    tabs->addItem(obsgs_tr("OBSTitles.Advanced"));
    tabs->addItem(obsgs_tr("OBSTitles.Logging"));
    tabs->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1;border:1px solid %2;color:%3;outline:none;}"
        "QListWidget::item{padding:8px 10px;border-bottom:1px solid %2;}"
        "QListWidget::item:hover{background:%4;}"
        "QListWidget::item:selected{background:%5;color:%6;}")
        .arg(base.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb)));

    auto *pages = new QStackedWidget(content);
    pages->setStyleSheet(QStringLiteral("QWidget{background:%1;color:%2;}")
                             .arg(window.name(QColor::HexRgb),
                                  text.name(QColor::HexRgb)));

    auto *colors_page = new QWidget(pages);
    auto *colors_layout = new QVBoxLayout(colors_page);
    colors_layout->setContentsMargins(0, 0, 0, 0);
    colors_layout->setSpacing(10);

    auto *colors_title = new QLabel(obsgs_tr("OBSTitles.Appearance"), colors_page);
    QFont title_font = colors_title->font();
    title_font.setPointSize(title_font.pointSize() + 2);
    title_font.setBold(true);
    colors_title->setFont(title_font);
    colors_layout->addWidget(colors_title);

    auto *appearance_scroll = new QScrollArea(colors_page);
    appearance_scroll->setWidgetResizable(true);
    appearance_scroll->setFrameShape(QFrame::NoFrame);
    auto *appearance_body = new QWidget(appearance_scroll);
    auto *appearance_body_layout = new QVBoxLayout(appearance_body);
    appearance_body_layout->setContentsMargins(0, 0, 0, 0);
    appearance_body_layout->setSpacing(12);

    auto *timeline_group = new QGroupBox(obsgs_tr("OBSTitles.Timeline"), appearance_body);
    auto *timeline_group_layout = new QVBoxLayout(timeline_group);
    timeline_group_layout->setContentsMargins(10, 12, 10, 10);
    auto *timeline_grid = new QGridLayout();
    timeline_grid->setContentsMargins(0, 0, 0, 0);
    timeline_grid->setHorizontalSpacing(12);
    timeline_grid->setVerticalSpacing(8);
    timeline_group_layout->addLayout(timeline_grid);

    auto *canvas_group = new QGroupBox(obsgs_tr("OBSTitles.Canvas"), appearance_body);
    auto *canvas_group_layout = new QVBoxLayout(canvas_group);
    canvas_group_layout->setContentsMargins(10, 12, 10, 10);
    auto *canvas_grid = new QGridLayout();
    canvas_grid->setContentsMargins(0, 0, 0, 0);
    canvas_grid->setHorizontalSpacing(12);
    canvas_grid->setVerticalSpacing(8);
    canvas_group_layout->addLayout(canvas_grid);

    const QString group_style = QStringLiteral(
        "QGroupBox{color:%1;border:1px solid %2;border-radius:5px;margin-top:8px;font-weight:bold;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;background:%3;}")
        .arg(text.name(QColor::HexRgb), border.name(QColor::HexRgb), window.name(QColor::HexRgb));
    timeline_group->setStyleSheet(group_style);
    canvas_group->setStyleSheet(group_style);

    appearance_body_layout->addWidget(timeline_group);
    appearance_body_layout->addWidget(canvas_group);
    appearance_body_layout->addStretch(1);
    appearance_scroll->setWidget(appearance_body);
    colors_layout->addWidget(appearance_scroll, 1);

    auto update_appearance = [editor]() {
        if (!editor) {
            TitlePreferences::notify_changed(nullptr);
            return;
        }
        if (editor->layers_)
            editor->layers_->refresh();
        if (editor->timeline_)
            editor->timeline_->update();
        if (editor->canvas_)
            editor->canvas_->update();
    };

    auto color_to_text = [](const QColor &color) {
        return color.alpha() < 255 ? color.name(QColor::HexArgb).toUpper()
                                   : color.name(QColor::HexRgb).toUpper();
    };
    auto apply_color_button = [border, highlight, color_to_text](QPushButton *button, const QColor &color) {
        const QColor label_color = color.lightness() < 128 ? Qt::white : Qt::black;
        button->setText(color_to_text(color));
        button->setStyleSheet(QStringLiteral(
            "QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:4px 8px;text-align:left;}"
            "QPushButton:hover{border-color:%4;}")
            .arg(label_color.name(QColor::HexRgb),
                 color.name(QColor::HexArgb),
                 border.name(QColor::HexRgb),
                 highlight.name(QColor::HexRgb)));
    };
    auto add_color_button_row = [&](QGridLayout *target_grid, int row, const QString &label, std::function<QColor()> current_color,
                                    std::function<void(const QColor &)> apply_color) {
        auto *name = new QLabel(label, colors_page);
        name->setStyleSheet(QStringLiteral("color:%1;").arg(text.name(QColor::HexRgb)));
        auto *button = new QPushButton(colors_page);
        button->setMinimumWidth(116);
        button->setCursor(Qt::PointingHandCursor);
        apply_color_button(button, current_color());
        target_grid->addWidget(name, row, 0);
        target_grid->addWidget(button, row, 1);
        connect(button, &QPushButton::clicked, dialog, [dialog, button, label, apply_color_button, apply_color, current_color]() mutable {
            auto *picker = new color_widgets::ColorDialog(dialog);
            picker->setAttribute(Qt::WA_DeleteOnClose);
            picker->setWindowTitle(label);
            picker->setAlphaEnabled(true);
            picker->setButtonMode(color_widgets::ColorDialog::OkApplyCancel);
            picker->setColor(current_color());
            obsgs_apply_color_dialog_theme(picker);
            connect(picker, &color_widgets::ColorDialog::colorChanged, dialog, [button, apply_color_button, apply_color](const QColor &color) mutable {
                if (!color.isValid())
                    return;
                apply_color(color);
                apply_color_button(button, color);
            });
            picker->open();
        });
    };
    auto add_timeline_color_row = [&](int row, const QString &label, TitlePreferences::TimelineColorRole role) {
        add_color_button_row(timeline_grid, row, label, [role]() { return TitlePreferences::timeline_color(role); }, [role, update_appearance](const QColor &color) {
            TitlePreferences::set_timeline_color(role, color);
            update_appearance();
        });
    };

    int color_row = 0;
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.TextLayers"), TitlePreferences::TimelineColorRole::TextLayer);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.ClockLayers"), TitlePreferences::TimelineColorRole::ClockLayer);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.TickerLayers"), TitlePreferences::TimelineColorRole::TickerLayer);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.ObjectLayers"), TitlePreferences::TimelineColorRole::ObjectLayer);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.ImageLayers"), TitlePreferences::TimelineColorRole::ImageLayer);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.CurrentTime"), TitlePreferences::TimelineColorRole::Current);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.PauseMarker"), TitlePreferences::TimelineColorRole::Pause);
    add_timeline_color_row(color_row++, obsgs_tr("OBSTitles.LoopStartEnd"), TitlePreferences::TimelineColorRole::Loop);
    timeline_grid->setColumnStretch(2, 1);

    auto add_canvas_color_row = [&](int row, const QString &label, TitlePreferences::CanvasHelperColorRole role) {
        add_color_button_row(canvas_grid, row, label, [role]() { return TitlePreferences::canvas_helper_color(role); }, [role, update_appearance](const QColor &color) {
            TitlePreferences::set_canvas_helper_color(role, color);
            update_appearance();
        });
    };
    int canvas_row = 0;
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.CanvasGuidelines"), TitlePreferences::CanvasHelperColorRole::Guides);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.ActiveGuidelines"), TitlePreferences::CanvasHelperColorRole::ActiveGuide);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.RulerMouseIndicators"), TitlePreferences::CanvasHelperColorRole::RulerMouseIndicator);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.HoverBoundingBoxes"), TitlePreferences::CanvasHelperColorRole::HoverBoundingBox);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.SelectionBoundingBoxes"), TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.TextBoundingBoxes"), TitlePreferences::CanvasHelperColorRole::TextBoundingBox);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.CanvasSnapLines"), TitlePreferences::CanvasHelperColorRole::CanvasSnapLines);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.ObjectSnapLines"), TitlePreferences::CanvasHelperColorRole::ObjectSnapLines);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.CanvasBorderColor"), TitlePreferences::CanvasHelperColorRole::CanvasBorder);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.ActionSafeGuides"), TitlePreferences::CanvasHelperColorRole::ActionSafe);
    add_canvas_color_row(canvas_row++, obsgs_tr("OBSTitles.GraphicsSafeGuides"), TitlePreferences::CanvasHelperColorRole::GraphicsSafe);
    add_color_button_row(canvas_grid, canvas_row++, obsgs_tr("OBSTitles.SceneMaskObjects"), []() { return TitlePreferences::scene_mask_color(); }, [update_appearance](const QColor &color) {
        TitlePreferences::set_scene_mask_color(color);
        update_appearance();
    });
    canvas_grid->setColumnStretch(2, 1);

    auto *advanced_page = new QWidget(pages);
    auto *advanced_layout = new QVBoxLayout(advanced_page);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    advanced_layout->setSpacing(10);

    auto *advanced_title = new QLabel(obsgs_tr("OBSTitles.Advanced"), advanced_page);
    advanced_title->setFont(title_font);
    advanced_layout->addWidget(advanced_title);

    auto *use_gpu = new QCheckBox(obsgs_tr("OBSTitles.UseGPU"), advanced_page);
    use_gpu->setChecked(TitlePreferences::use_gpu());
    use_gpu->setToolTip(obsgs_tr("OBSTitles.UseGPUTooltip"));
    use_gpu->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                               .arg(text.name(QColor::HexRgb),
                                    disabled_text.name(QColor::HexRgb)));
    advanced_layout->addWidget(use_gpu);
    auto *cache_enabled = new QCheckBox(obsgs_tr("OBSTitles.EnableCachingPrerender"), advanced_page);
    cache_enabled->setChecked(CacheManager::instance().cacheEnabled());
    cache_enabled->setToolTip(obsgs_tr("OBSTitles.EnableCachingPrerenderTooltip"));
    cache_enabled->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                     .arg(text.name(QColor::HexRgb),
                                          disabled_text.name(QColor::HexRgb)));
    advanced_layout->addWidget(cache_enabled);
    auto *cache_form = new QFormLayout();
    cache_form->setContentsMargins(0, 0, 0, 0);
    cache_form->setSpacing(8);
    auto *ram_limit = new QSpinBox(advanced_page);
    ram_limit->setRange(64, 32768);
    ram_limit->setSingleStep(128);
    ram_limit->setSuffix(QStringLiteral(" MB"));
    ram_limit->setValue(TitlePreferences::cache_ram_limit_mb());
    cache_form->addRow(obsgs_tr("OBSTitles.RamCacheLimit"), ram_limit);

    auto *disk_path_row = new QWidget(advanced_page);
    auto *disk_path_layout = new QHBoxLayout(disk_path_row);
    disk_path_layout->setContentsMargins(0, 0, 0, 0);
    disk_path_layout->setSpacing(6);
    auto *disk_path = new QLineEdit(CacheManager::instance().diskCacheLocation(), disk_path_row);
    auto *browse_cache = new QPushButton(obsgs_tr("OBSTitles.Browse"), disk_path_row);
    disk_path_layout->addWidget(disk_path, 1);
    disk_path_layout->addWidget(browse_cache);
    cache_form->addRow(obsgs_tr("OBSTitles.DiskCacheFolder"), disk_path_row);

    auto *cache_usage = new QLabel(advanced_page);
    cache_usage->setWordWrap(true);
    cache_form->addRow(obsgs_tr("OBSTitles.CacheUsage"), cache_usage);

    auto *cache_actions_row = new QWidget(advanced_page);
    auto *cache_actions_layout = new QHBoxLayout(cache_actions_row);
    cache_actions_layout->setContentsMargins(0, 0, 0, 0);
    cache_actions_layout->setSpacing(6);
    auto *clear_cache_now = new QPushButton(obsgs_tr("OBSTitles.ClearCacheNow"), cache_actions_row);
    clear_cache_now->setToolTip(obsgs_tr("OBSTitles.ClearCacheNowTooltip"));
    cache_actions_layout->addWidget(clear_cache_now);
    cache_actions_layout->addStretch(1);
    cache_form->addRow(QString(), cache_actions_row);

    auto *clear_cache_on_exit = new QCheckBox(obsgs_tr("OBSTitles.ClearCacheOnExit"), advanced_page);
    clear_cache_on_exit->setChecked(TitlePreferences::clear_cache_on_exit());
    clear_cache_on_exit->setToolTip(obsgs_tr("OBSTitles.ClearCacheOnExitTooltip"));
    clear_cache_on_exit->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                           .arg(text.name(QColor::HexRgb),
                                                disabled_text.name(QColor::HexRgb)));
    cache_form->addRow(QString(), clear_cache_on_exit);

    advanced_layout->addLayout(cache_form);
    advanced_layout->addStretch(1);

    auto *logging_page = new QWidget(pages);
    auto *logging_layout = new QVBoxLayout(logging_page);
    logging_layout->setContentsMargins(0, 0, 0, 0);
    logging_layout->setSpacing(10);

    auto *logging_title = new QLabel(obsgs_tr("OBSTitles.Logging"), logging_page);
    logging_title->setFont(title_font);
    logging_layout->addWidget(logging_title);

    auto *logging_enabled = new QCheckBox(obsgs_tr("OBSTitles.EnableFileLogging"), logging_page);
    logging_enabled->setChecked(TitlePreferences::logging_enabled());
    logging_enabled->setToolTip(obsgs_tr("OBSTitles.FileLoggingTooltip"));
    logging_enabled->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                       .arg(text.name(QColor::HexRgb),
                                            disabled_text.name(QColor::HexRgb)));
    logging_layout->addWidget(logging_enabled);

    auto *logging_form = new QFormLayout();
    logging_form->setContentsMargins(0, 0, 0, 0);
    logging_form->setSpacing(8);

    auto *logging_level = new QComboBox(logging_page);
    logging_level->addItem(obsgs_tr("OBSTitles.Off"), 0);
    logging_level->addItem(obsgs_tr("OBSTitles.Error"), 1);
    logging_level->addItem(obsgs_tr("OBSTitles.Warning"), 2);
    logging_level->addItem(obsgs_tr("OBSTitles.Info"), 3);
    logging_level->addItem(obsgs_tr("OBSTitles.Debug"), 4);
    logging_level->addItem(obsgs_tr("OBSTitles.Trace"), 5);
    const int logging_level_index = logging_level->findData(TitlePreferences::logging_level());
    logging_level->setCurrentIndex(logging_level_index >= 0 ? logging_level_index : 3);
    logging_form->addRow(obsgs_tr("OBSTitles.Level"), logging_level);

    auto *logging_path_row = new QWidget(logging_page);
    auto *logging_path_layout = new QHBoxLayout(logging_path_row);
    logging_path_layout->setContentsMargins(0, 0, 0, 0);
    logging_path_layout->setSpacing(6);
    auto *logging_path = new QLineEdit(TitlePreferences::logging_file_path(), logging_path_row);
    auto *browse_log = new QPushButton(obsgs_tr("OBSTitles.Browse"), logging_path_row);
    logging_path_layout->addWidget(logging_path, 1);
    logging_path_layout->addWidget(browse_log);
    logging_form->addRow(obsgs_tr("OBSTitles.LogFile"), logging_path_row);

    auto *mirror_obs = new QCheckBox(obsgs_tr("OBSTitles.AlsoWriteObsLog"), logging_page);
    mirror_obs->setChecked(TitlePreferences::logging_mirror_to_obs());
    mirror_obs->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                  .arg(text.name(QColor::HexRgb),
                                       disabled_text.name(QColor::HexRgb)));
    logging_form->addRow(QString(), mirror_obs);

    auto *logging_buttons_row = new QWidget(logging_page);
    auto *logging_buttons_layout = new QHBoxLayout(logging_buttons_row);
    logging_buttons_layout->setContentsMargins(0, 0, 0, 0);
    logging_buttons_layout->setSpacing(6);
    auto *open_log_folder = new QPushButton(obsgs_tr("OBSTitles.OpenFolder"), logging_buttons_row);
    auto *clear_log = new QPushButton(obsgs_tr("OBSTitles.ClearLog"), logging_buttons_row);
    logging_buttons_layout->addWidget(open_log_folder);
    logging_buttons_layout->addWidget(clear_log);
    logging_buttons_layout->addStretch(1);
    logging_form->addRow(QString(), logging_buttons_row);

    auto *logging_hint = new QLabel(obsgs_tr("OBSTitles.LoggingHint"), logging_page);
    logging_hint->setWordWrap(true);
    logging_hint->setStyleSheet(QStringLiteral("color:%1;").arg(disabled_text.name(QColor::HexRgb)));
    logging_form->addRow(obsgs_tr("OBSTitles.Notes"), logging_hint);

    logging_layout->addLayout(logging_form);
    logging_layout->addStretch(1);

    pages->addWidget(colors_page);
    pages->addWidget(advanced_page);
    pages->addWidget(logging_page);
    content_layout->addWidget(tabs);
    content_layout->addWidget(pages, 1);
    layout->addWidget(content, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    layout->addWidget(buttons);

    tabs->setCurrentRow(0);
    connect(tabs, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(use_gpu, &QCheckBox::toggled, dialog, [editor, update_appearance](bool enabled) {
        if (editor) {
            editor->set_gpu_pipeline_enabled(enabled);
            return;
        }
        if (TitlePreferences::use_gpu() == enabled)
            return;
        TitlePreferences::set_use_gpu(enabled);
        TitlePreferences::notify_changed(nullptr);
        update_appearance();
    });
    connect(cache_enabled, &QCheckBox::toggled, dialog, [](bool enabled) {
        OGS_LOG_INFO("Preferences", QStringLiteral("Set cache enabled=%1").arg(enabled));
        CacheManager::instance().setCacheEnabled(enabled);
    });
    connect(ram_limit, QOverload<int>::of(&QSpinBox::valueChanged), dialog, [](int value) {
        OGS_LOG_INFO("Preferences", QStringLiteral("Set RAM cache limit=%1 MB").arg(value));
        CacheManager::instance().setRamCacheLimitMb(value);
    });
    connect(disk_path, &QLineEdit::editingFinished, dialog, [disk_path]() {
        CacheManager::instance().setDiskCacheLocation(disk_path->text());
        disk_path->setText(CacheManager::instance().diskCacheLocation());
    });
    connect(browse_cache, &QPushButton::clicked, dialog, [dialog, disk_path]() {
        const QString path = QFileDialog::getExistingDirectory(dialog, obsgs_tr("OBSTitles.DiskCacheFolder"),
                                                               disk_path->text());
        if (path.isEmpty())
            return;
        disk_path->setText(path);
        OGS_LOG_INFO("Preferences", QStringLiteral("Set disk cache location=%1").arg(path));
        CacheManager::instance().setDiskCacheLocation(path);
    });
    connect(clear_cache_now, &QPushButton::clicked, dialog, []() {
        OGS_LOG_INFO("Preferences", QStringLiteral("Clear cache now requested"));
        CacheManager::instance().clearAll();
    });
    connect(clear_cache_on_exit, &QCheckBox::toggled, dialog, [](bool enabled) {
        OGS_LOG_INFO("Preferences", QStringLiteral("Set clear cache on exit=%1").arg(enabled));
        TitlePreferences::set_clear_cache_on_exit(enabled);
    });
    connect(logging_enabled, &QCheckBox::toggled, dialog, [](bool enabled) {
        TitlePreferences::set_logging_enabled(enabled);
        OGS_LOG_INFO("Preferences", QStringLiteral("Set logging enabled=%1").arg(enabled));
    });
    connect(logging_level, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [logging_level](int index) {
        TitlePreferences::set_logging_level(logging_level->itemData(index).toInt());
        OGS_LOG_INFO("Preferences", QStringLiteral("Set logging level=%1").arg(logging_level->itemData(index).toInt()));
    });
    connect(logging_path, &QLineEdit::editingFinished, dialog, [logging_path]() {
        TitlePreferences::set_logging_file_path(logging_path->text());
        logging_path->setText(TitlePreferences::logging_file_path());
    });
    connect(browse_log, &QPushButton::clicked, dialog, [dialog, logging_path]() {
        const QString path = QFileDialog::getSaveFileName(dialog, obsgs_tr("OBSTitles.LogFile"),
                                                          logging_path->text(),
                                                          obsgs_tr("OBSTitles.LogFileFilter"));
        if (path.isEmpty())
            return;
        logging_path->setText(path);
        TitlePreferences::set_logging_file_path(path);
        OGS_LOG_INFO("Preferences", QStringLiteral("Set log file path=%1").arg(path));
    });
    connect(mirror_obs, &QCheckBox::toggled, dialog, [](bool enabled) {
        TitlePreferences::set_logging_mirror_to_obs(enabled);
        OGS_LOG_INFO("Preferences", QStringLiteral("Set logging mirror to OBS=%1").arg(enabled));
    });
    connect(open_log_folder, &QPushButton::clicked, dialog, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(TitlePreferences::logging_file_path()).absolutePath()));
    });
    connect(clear_log, &QPushButton::clicked, dialog, []() {
        QFile file(TitlePreferences::logging_file_path());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            file.close();
        OGS_LOG_INFO("Preferences", QStringLiteral("Cleared log file"));
    });
    auto format_cache_bytes = [](quint64 bytes) {
        const double mib = (double)bytes / 1024.0 / 1024.0;
        if (mib < 1024.0)
            return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
        return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', 2);
    };
    auto update_cache_usage = [cache_usage, format_cache_bytes]() {
        CacheManager &cache = CacheManager::instance();
        cache_usage->setText(obsgs_tr("OBSTitles.CacheUsageSummary")
                                 .arg(format_cache_bytes(cache.ramBytesUsed()),
                                      format_cache_bytes(cache.ramBytesLimit()),
                                      format_cache_bytes(cache.diskBytesUsed())));
    };
    update_cache_usage();
    connect(&CacheManager::instance(), &CacheManager::diagnosticsChanged, dialog, update_cache_usage);
    connect(&CacheManager::instance(), &CacheManager::cacheEnabledChanged, dialog, [cache_enabled](bool enabled) {
        if (cache_enabled->isChecked() == enabled) return;
        QSignalBlocker blocker(cache_enabled);
        cache_enabled->setChecked(enabled);
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}



void TitleEditor::update_display_refresh_pacing()
{
    QScreen *screen = nullptr;
    if (windowHandle())
        screen = windowHandle()->screen();
    if (!screen)
        screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    double hz = screen ? screen->refreshRate() : 60.0;
    if (!std::isfinite(hz) || hz < 24.0)
        hz = 60.0;
    hz = std::clamp(hz, 24.0, 240.0);
    display_refresh_hz_ = hz;

    // Only GUI/canvas editing follows the monitor cadence. Playback remains
    // locked to the project/OBS frame rate through play_timer_.
    if (gui_refresh_timer_) {
        gui_refresh_timer_->setInterval(
            std::max(1, static_cast<int>(std::lround(1000.0 / hz))));
        if (!gui_refresh_timer_->isActive())
            gui_refresh_timer_->start();
    }

    if (play_timer_)
        play_timer_->setInterval(editor_playback_ui_timer_interval_ms());
}

bool TitleEditor::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *watched_widget = qobject_cast<QWidget *>(watched);
    const bool editor_widget = watched_widget &&
        (watched_widget == this || isAncestorOf(watched_widget));
    if (editor_widget &&
        (event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::ParentAboutToChange ||
         event->type() == QEvent::ParentChange ||
         event->type() == QEvent::Move ||
         event->type() == QEvent::Resize)) {
        dock_layout_transition_ = true;
        if (layout_settle_timer_)
            layout_settle_timer_->start();
    }

    if (watched == this && (event->type() == QEvent::Show || event->type() == QEvent::Move ||
                            event->type() == QEvent::ScreenChangeInternal)) {
        // Defer monitor refresh recalculation until the dock/layout operation has
        // completed; querying/painting through a half-rebuilt widget hierarchy is unsafe.
        if (layout_settle_timer_)
            layout_settle_timer_->start();
        else
            QTimer::singleShot(0, this, &TitleEditor::update_display_refresh_pacing);
    }
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(watched)) {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            if (!spin->property("gspDefaultValue").isValid()) {
                if (auto *double_spin = qobject_cast<QDoubleSpinBox *>(spin))
                    spin->setProperty("gspDefaultValue", double_spin->value());
                else if (auto *int_spin = qobject_cast<QSpinBox *>(spin))
                    spin->setProperty("gspDefaultValue", int_spin->value());
            }
        }
        if (event->type() == QEvent::Wheel && spin->isEnabled()) {
            // Spin boxes intentionally accept wheel changes while hovered, without
            // stealing keyboard focus from the canvas/timeline.
            auto *wheel = static_cast<QWheelEvent *>(event);
            const int steps = wheel->angleDelta().y() / 120;
            if (steps != 0) {
                for (int i = 0; i < std::abs(steps); ++i)
                    steps > 0 ? spin->stepUp() : spin->stepDown();
                wheel->accept();
                return true;
            }
        }
    }
    if (watched == title_lbl_ && event->type() == QEvent::MouseButtonDblClick) {
        auto *mouse_event = static_cast<QMouseEvent *>(event);
        if (mouse_event->button() == Qt::LeftButton) {
            begin_title_name_edit();
            event->accept();
            return true;
        }
    }

    if (watched == title_name_edit_) {
        if (event->type() == QEvent::KeyPress) {
            auto *key_event = static_cast<QKeyEvent *>(event);
            if (key_event->key() == Qt::Key_Escape) {
                commit_title_name_edit(false);
                key_event->accept();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            commit_title_name_edit(false);
            return false;
        }
    }

    if (event->type() == QEvent::KeyPress && isActiveWindow()) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool in_editor = widget && (widget == this || isAncestorOf(widget));
        const bool editing_value = editor_focus_accepts_text(focusWidget());
        const Qt::KeyboardModifiers shortcut_modifiers =
            key_event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier |
                                      Qt::AltModifier | Qt::MetaModifier);
        const Qt::KeyboardModifiers tool_modifiers =
            shortcut_modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

        // Route history shortcuts before the focused canvas/viewport can consume
        // them. Text-entry widgets retain their own native document undo/redo.
        if (in_editor && !editing_value && !key_event->isAutoRepeat() &&
            key_event->key() == Qt::Key_Z) {
            if (shortcut_modifiers == Qt::ControlModifier) {
                if (undo_index_ > 0)
                    restore_undo_snapshot(undo_index_ - 1);
                key_event->accept();
                return true;
            }
            if (shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
                if (undo_index_ + 1 < (int)undo_stack_.size())
                    restore_undo_snapshot(undo_index_ + 1);
                key_event->accept();
                return true;
            }
        }

        if (in_editor && !editing_value && tool_modifiers == Qt::NoModifier &&
            !key_event->isAutoRepeat() && tools_sidebar_) {
            switch (key_event->key()) {
            case Qt::Key_V:
                tools_sidebar_->activate_selection_tool();
                key_event->accept();
                return true;
            case Qt::Key_A:
                tools_sidebar_->activate_direct_selection_tool();
                key_event->accept();
                return true;
            case Qt::Key_P:
                tools_sidebar_->activate_pen_tool();
                key_event->accept();
                return true;
            case Qt::Key_M:
                tools_sidebar_->activate_shape_tool(ShapeType::Rectangle);
                key_event->accept();
                return true;
            case Qt::Key_L:
                tools_sidebar_->activate_shape_tool(ShapeType::Ellipse);
                key_event->accept();
                return true;
            case Qt::Key_T:
                tools_sidebar_->activate_text_tool(LayerType::Text);
                key_event->accept();
                return true;
            case Qt::Key_I:
                tools_sidebar_->activate_color_picker_tool();
                key_event->accept();
                return true;
            case Qt::Key_G:
                tools_sidebar_->activate_gradient_tool();
                key_event->accept();
                return true;
            default:
                break;
            }
        }

        if (in_editor && key_event->key() == Qt::Key_Space && !key_event->isAutoRepeat()) {
            if (!editing_value) {
                play_pause();
                key_event->accept();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void TitleEditor::keyPressEvent(QKeyEvent *ev)
{
    QWidget *fw = focusWidget();
    bool editing_value = editor_focus_accepts_text(fw);

    // QKeyEvent::matches() only accepts QKeySequence::StandardKey in Qt 6.
    // Check the custom Undo/Redo bindings explicitly instead. Text-editing
    // widgets keep their own local undo stack.
    const Qt::KeyboardModifiers shortcut_modifiers =
        ev->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier |
                           Qt::AltModifier | Qt::MetaModifier);
    if (!editing_value && ev->key() == Qt::Key_Z &&
        shortcut_modifiers == Qt::ControlModifier) {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
        ev->accept();
        return;
    }
    if (!editing_value && ev->key() == Qt::Key_Z &&
        shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
        ev->accept();
        return;
    }

    if (!editing_value && ev->key() == Qt::Key_Escape) {
        close();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Copy) &&
        timeline_->has_selected_keyframes()) {
        timeline_->copy_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Cut) &&
        timeline_->has_selected_keyframes()) {
        timeline_->cut_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Paste) &&
        timeline_->has_keyframe_clipboard()) {
        timeline_->paste_keyframes_at_playhead();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ &&
        (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) &&
        timeline_->has_selected_keyframes()) {
        timeline_->delete_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Copy) && !sel_layer_id_.empty()) {
        copy_selected_layer();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Cut) && !sel_layer_id_.empty()) {
        cut_selected_layer();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Paste)) {
        if (!layer_clipboard_.empty()) {
            paste_layer_from_clipboard();
            ev->accept();
            return;
        }
        if (paste_external_clipboard_to_canvas()) {
            ev->accept();
            return;
        }
    }
    if (!editing_value && ev->key() == Qt::Key_Delete && !sel_layer_id_.empty()) {
        delete_selected_layer();
        ev->accept();
        return;
    }
    if (ev->key() == Qt::Key_Space && !ev->isAutoRepeat()) {
        if (!editor_focus_accepts_text(focusWidget())) {
            play_pause();
            ev->accept();
            return;
        }
    }
    QMainWindow::keyPressEvent(ev);
}


void TitleEditor::closeEvent(QCloseEvent *ev)
{
    if (confirm_save_before_close()) {
        save_editor_layout();
        editor_layout_save_suppressed_ = true;
        ev->accept();
    } else {
        ev->ignore();
    }
}

void TitleEditor::reject()
{
    close();
}

/* ── Signal handlers ─────────────────────────────────────────────── */



static void copy_editor_layer_style_fields(Layer &dst, const Layer &src)
{
    dst.font_family = src.font_family;
    dst.font_style = src.font_style;
    dst.font_size = src.font_size;
    dst.font_size_prop.static_value = src.font_size_prop.static_value;
    dst.font_bold = src.font_bold;
    dst.font_italic = src.font_italic;
    dst.font_kerning = src.font_kerning;
    dst.kerning_mode = src.kerning_mode;
    dst.manual_kerning = src.manual_kerning;
    dst.text_leading = src.text_leading;
    dst.char_tracking = src.char_tracking;
    dst.char_tracking_prop.static_value = src.char_tracking_prop.static_value;
    dst.char_scale_x = src.char_scale_x;
    dst.char_scale_y = src.char_scale_y;
    dst.char_scale_x_prop.static_value = src.char_scale_x_prop.static_value;
    dst.char_scale_y_prop.static_value = src.char_scale_y_prop.static_value;
    dst.baseline_shift = src.baseline_shift;
    dst.baseline_shift_prop.static_value = src.baseline_shift_prop.static_value;
    dst.text_style = src.text_style;
    dst.text_underline = src.text_underline;
    dst.text_strikethrough = src.text_strikethrough;
    dst.text_ligatures = src.text_ligatures;
    dst.text_stylistic_alternates = src.text_stylistic_alternates;
    dst.text_fractions = src.text_fractions;
    dst.text_opentype_features = src.text_opentype_features;
    dst.text_overflow_mode = src.text_overflow_mode;
    dst.text_fit_min_scale = src.text_fit_min_scale;
    dst.text_box_width_to_text = src.text_box_width_to_text;
    dst.text_box_height_to_text = src.text_box_height_to_text;
    dst.max_text_box_width = src.max_text_box_width;
    dst.max_text_box_height = src.max_text_box_height;

    dst.text_color = src.text_color;
    dst.fill_color = src.fill_color;
    dst.fill_type = src.fill_type;
    dst.gradient_type = src.gradient_type;
    dst.gradient_start_color = src.gradient_start_color;
    dst.gradient_end_color = src.gradient_end_color;
    dst.gradient_start_pos = src.gradient_start_pos;
    dst.gradient_end_pos = src.gradient_end_pos;
    dst.gradient_start_opacity = src.gradient_start_opacity;
    dst.gradient_end_opacity = src.gradient_end_opacity;
    dst.gradient_opacity = src.gradient_opacity;
    dst.gradient_angle = src.gradient_angle;
    dst.gradient_center_x = src.gradient_center_x;
    dst.gradient_center_y = src.gradient_center_y;
    dst.gradient_scale = src.gradient_scale;
    dst.gradient_focal_x = src.gradient_focal_x;
    dst.gradient_focal_y = src.gradient_focal_y;
    dst.gradient_stops = src.gradient_stops;

    dst.outline_enabled = src.outline_enabled;
    dst.stroke_fill_type = src.stroke_fill_type;
    dst.stroke_color = src.stroke_color;
    dst.stroke_width = src.stroke_width;
    dst.outline_opacity = src.outline_opacity;
    dst.outline_join_style = src.outline_join_style;
    dst.outline_on_front = src.outline_on_front;
    dst.outline_alignment = src.outline_alignment;
    dst.outline_antialias = src.outline_antialias;
    dst.scale_stroke_with_shape = src.scale_stroke_with_shape;
    dst.scale_corners_with_shape = src.scale_corners_with_shape;
    dst.stroke_gradient_type = src.stroke_gradient_type;
    dst.stroke_gradient_start_color = src.stroke_gradient_start_color;
    dst.stroke_gradient_end_color = src.stroke_gradient_end_color;
    dst.stroke_gradient_start_pos = src.stroke_gradient_start_pos;
    dst.stroke_gradient_end_pos = src.stroke_gradient_end_pos;
    dst.stroke_gradient_start_opacity = src.stroke_gradient_start_opacity;
    dst.stroke_gradient_end_opacity = src.stroke_gradient_end_opacity;
    dst.stroke_gradient_opacity = src.stroke_gradient_opacity;
    dst.stroke_gradient_angle = src.stroke_gradient_angle;
    dst.stroke_gradient_center_x = src.stroke_gradient_center_x;
    dst.stroke_gradient_center_y = src.stroke_gradient_center_y;
    dst.stroke_gradient_scale = src.stroke_gradient_scale;
    dst.stroke_gradient_focal_x = src.stroke_gradient_focal_x;
    dst.stroke_gradient_focal_y = src.stroke_gradient_focal_y;
    dst.stroke_gradient_stops = src.stroke_gradient_stops;

    dst.background_enabled = src.background_enabled;
    dst.background_color = src.background_color;
    dst.background_opacity = src.background_opacity;
    dst.background_padding_x = src.background_padding_x;
    dst.background_padding_y = src.background_padding_y;
    dst.background_padding_left = src.background_padding_left;
    dst.background_padding_right = src.background_padding_right;
    dst.background_padding_top = src.background_padding_top;
    dst.background_padding_bottom = src.background_padding_bottom;
    dst.background_corner_radius = src.background_corner_radius;
    dst.background_corner_radius_tl = src.background_corner_radius_tl;
    dst.background_corner_radius_tr = src.background_corner_radius_tr;
    dst.background_corner_radius_br = src.background_corner_radius_br;
    dst.background_corner_radius_bl = src.background_corner_radius_bl;
    dst.background_corner_type = src.background_corner_type;
    dst.background_fill_type = src.background_fill_type;
    dst.background_stroke_color = src.background_stroke_color;
    dst.background_stroke_width = src.background_stroke_width;
    dst.background_stroke_opacity = src.background_stroke_opacity;
    dst.background_stroke_fill_type = src.background_stroke_fill_type;
    dst.background_gradient_type = src.background_gradient_type;
    dst.background_gradient_start_color = src.background_gradient_start_color;
    dst.background_gradient_end_color = src.background_gradient_end_color;
    dst.background_gradient_start_opacity = src.background_gradient_start_opacity;
    dst.background_gradient_end_opacity = src.background_gradient_end_opacity;
    dst.background_gradient_opacity = src.background_gradient_opacity;

    dst.align_h = src.align_h;
    dst.align_v = src.align_v;
    dst.paragraph_indent_left = src.paragraph_indent_left;
    dst.paragraph_indent_right = src.paragraph_indent_right;
    dst.paragraph_indent_first_line = src.paragraph_indent_first_line;
    dst.paragraph_space_before = src.paragraph_space_before;
    dst.paragraph_space_after = src.paragraph_space_after;
    dst.paragraph_hyphenate = src.paragraph_hyphenate;
    dst.paragraph_indent_left_prop.static_value = src.paragraph_indent_left_prop.static_value;
    dst.paragraph_indent_right_prop.static_value = src.paragraph_indent_right_prop.static_value;
    dst.paragraph_indent_first_line_prop.static_value = src.paragraph_indent_first_line_prop.static_value;
    dst.paragraph_space_before_prop.static_value = src.paragraph_space_before_prop.static_value;
    dst.paragraph_space_after_prop.static_value = src.paragraph_space_after_prop.static_value;

    // New-layer defaults should persist visual base styling only. Do not copy
    // the layer effect stack or mask/effect interaction flags into defaults.
    dst.effects.clear();
    dst.effect_stack_respects_masks = false;
}

void TitleEditor::load_sidebar_default_colors()
{
    if (g_editor_session_sidebar_colors_initialized) {
        default_foreground_color_ = g_editor_session_foreground_color;
        default_background_color_ = g_editor_session_background_color;
        return;
    }

    g_editor_session_foreground_color = default_foreground_color_;
    g_editor_session_background_color = default_background_color_;
    g_editor_session_sidebar_colors_initialized = true;
}

void TitleEditor::save_sidebar_default_colors() const
{
    g_editor_session_foreground_color = default_foreground_color_;
    g_editor_session_background_color = default_background_color_;
    g_editor_session_sidebar_colors_initialized = true;
    if (title_) {
        title_->editor_default_style_enabled = true;
        title_->editor_default_foreground_color = argb_from_color(default_foreground_color_);
        title_->editor_default_background_color = argb_from_color(default_background_color_);
    }
}



void TitleEditor::copy_layer_style_to_new_layer_defaults(const Layer &layer)
{
    copy_editor_layer_style_fields(default_new_layer_style_, layer);
    save_new_layer_defaults();
}

void TitleEditor::apply_new_layer_defaults(Layer &layer) const
{
    copy_editor_layer_style_fields(layer, default_new_layer_style_);
    const uint32_t fg_argb = argb_from_color(default_foreground_color_);
    const uint32_t bg_argb = argb_from_color(default_background_color_);
    const bool text_like = layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker;
    if (text_like) {
        // Text, Clock and Ticker layers use text_color for solid fill rendering,
        // but they also share the generic fill_type / gradient fields with shape
        // layers. Keep those generic fill defaults intact so the sidebar
        // foreground swatch behaves like Photoshop/Illustrator for every new
        // drawable object, not only shapes.
        layer.fill_type = default_new_layer_style_.fill_type;

        const uint32_t default_solid_fill =
            (default_new_layer_style_.fill_color != 0) ? default_new_layer_style_.fill_color :
            ((default_new_layer_style_.text_color != 0) ? default_new_layer_style_.text_color : fg_argb);

        if (layer.fill_type == 1) {
            // Gradient text still needs a valid solid fallback/selection color.
            layer.text_color = (default_new_layer_style_.text_color != 0)
                ? default_new_layer_style_.text_color
                : default_new_layer_style_.gradient_start_color;
            if (layer.text_color == 0)
                layer.text_color = fg_argb;
        } else {
            layer.fill_type = 0;
            layer.text_color = default_solid_fill;
            layer.fill_color = default_solid_fill;
        }

        layer.stroke_color = (default_new_layer_style_.stroke_color != 0)
            ? default_new_layer_style_.stroke_color
            : bg_argb;

        RichTextCharFormat fmt = layer_char_format_for_editor(layer);
        layer.rich_text.default_format = fmt;
        layer.rich_text.default_paragraph_format.align_h = layer.align_h;
        layer.rich_text.default_paragraph_format.align_v = layer.align_v;
        apply_rich_text_format_to_layer_range(layer, fmt, (RichTextCharFontFamily | RichTextCharFontStyle | RichTextCharFontSize | RichTextCharBold | RichTextCharItalic | RichTextCharUnderline | RichTextCharStrikethrough | RichTextCharTracking | RichTextCharScaleX | RichTextCharScaleY | RichTextCharBaselineShift | RichTextCharFillColor | RichTextCharKerning | RichTextCharTextStyle | RichTextCharLigatures | RichTextCharStylisticAlternates | RichTextCharFractions | RichTextCharOpenTypeFeatures | RichTextCharLanguage), false);
    } else if (layer.type == LayerType::Shape || layer.type == LayerType::SolidRect) {
        layer.fill_color = (default_new_layer_style_.fill_color != 0) ? default_new_layer_style_.fill_color : fg_argb;
        layer.stroke_color = (default_new_layer_style_.stroke_color != 0) ? default_new_layer_style_.stroke_color : bg_argb;
    }
}

void TitleEditor::load_new_layer_defaults()
{
    if (title_ && title_->editor_default_style_enabled) {
        default_new_layer_style_ = title_->editor_default_layer_style;
        default_new_layer_style_.effects.clear();
        default_new_layer_style_.effect_stack_respects_masks = false;
        default_foreground_color_ = color_from_argb(title_->editor_default_foreground_color);
        default_background_color_ = color_from_argb(title_->editor_default_background_color);
        return;
    }

    if (g_editor_session_new_layer_defaults_initialized) {
        default_new_layer_style_ = g_editor_session_new_layer_style;
        default_new_layer_style_.effects.clear();
        default_new_layer_style_.effect_stack_respects_masks = false;
        return;
    }

    g_editor_session_new_layer_style = default_new_layer_style_;
    g_editor_session_new_layer_style.effects.clear();
    g_editor_session_new_layer_style.effect_stack_respects_masks = false;
    g_editor_session_new_layer_defaults_initialized = true;
}


void TitleEditor::save_new_layer_defaults() const
{
    Layer persisted = default_new_layer_style_;
    persisted.effects.clear();
    persisted.effect_stack_respects_masks = false;

    g_editor_session_new_layer_style = persisted;
    g_editor_session_new_layer_defaults_initialized = true;

    if (title_) {
        title_->editor_default_style_enabled = true;
        title_->editor_default_layer_style = persisted;
        title_->editor_default_foreground_color = argb_from_color(default_foreground_color_);
        title_->editor_default_background_color = argb_from_color(default_background_color_);
    }
}


void TitleEditor::set_default_sidebar_colors_from_layer(const Layer &layer)
{
    const double lt = std::clamp(playhead_ - layer.in_time, 0.0,
                                 std::max(0.0, layer.out_time - layer.in_time));
    const bool is_text_like = layer.type == LayerType::Text || layer.type == LayerType::Clock ||
                              layer.type == LayerType::Ticker;
    default_foreground_color_ = color_from_argb(is_text_like ? eval_text_color(layer, lt) : eval_fill_color(layer, lt));
    default_background_color_ = color_from_argb(eval_outline_color(layer, lt));
    copy_layer_style_to_new_layer_defaults(layer);

    // For text-like layers the visible fill color lives in text_color, while
    // shape-like layers use fill_color. Mirror the solid text fill into the
    // generic fill default so the next Shape/Text/Clock/Ticker starts with the
    // same foreground fill regardless of which layer type changed it.
    if (is_text_like) {
        default_new_layer_style_.fill_type = layer.fill_type;
        if (layer.fill_type == 0)
            default_new_layer_style_.fill_color = is_text_like ? eval_text_color(layer, lt) : eval_fill_color(layer, lt);
        default_new_layer_style_.text_color = eval_text_color(layer, lt);
    } else {
        default_new_layer_style_.text_color = eval_fill_color(layer, lt);
    }

    save_sidebar_default_colors();
    save_new_layer_defaults();
}

void TitleEditor::update_sidebar_color_swatches(std::shared_ptr<Layer> layer)
{
    if (!tools_sidebar_) return;
    if (layer) {
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        const bool is_text_like = layer->type == LayerType::Text || layer->type == LayerType::Clock ||
                                  layer->type == LayerType::Ticker;
        const uint32_t fg = is_text_like ? eval_text_color(*layer, lt) : eval_fill_color(*layer, lt);
        const uint32_t bg = eval_outline_color(*layer, lt);
        // Show the actual active fill in the sidebar swatch for every layer type that
        // supports gradients, including Text / Clock / Ticker. Previously text-like
        // layers were forced to a solid color fallback, so their gradient state was
        // stored on the layer but never visible in the sidebar swatch.
        if (layer->fill_type == 1) {
            tools_sidebar_->set_foreground_gradient(color_from_argb(layer->gradient_start_color),
                                                    color_from_argb(layer->gradient_end_color),
                                                    layer->gradient_type);
        } else {
            tools_sidebar_->set_foreground_color(color_from_argb(fg));
        }
        if (layer->stroke_fill_type == 2) {
            tools_sidebar_->set_background_gradient(color_from_argb(layer->stroke_gradient_start_color),
                                                    color_from_argb(layer->stroke_gradient_end_color),
                                                    layer->stroke_gradient_type);
        } else {
            tools_sidebar_->set_background_color(color_from_argb(bg));
        }
    } else {
        if (default_new_layer_style_.fill_type == 1) {
            tools_sidebar_->set_foreground_gradient(color_from_argb(default_new_layer_style_.gradient_start_color),
                                                    color_from_argb(default_new_layer_style_.gradient_end_color),
                                                    default_new_layer_style_.gradient_type);
        } else {
            tools_sidebar_->set_foreground_color(default_foreground_color_);
        }
        if (default_new_layer_style_.stroke_fill_type == 2) {
            tools_sidebar_->set_background_gradient(color_from_argb(default_new_layer_style_.stroke_gradient_start_color),
                                                    color_from_argb(default_new_layer_style_.stroke_gradient_end_color),
                                                    default_new_layer_style_.stroke_gradient_type);
        } else {
            tools_sidebar_->set_background_color(default_background_color_);
        }
    }
}


void TitleEditor::open_default_sidebar_color_popup(bool foreground)
{
    QColor &target = foreground ? default_foreground_color_ : default_background_color_;

    QDialog popup(this, Qt::Popup | Qt::FramelessWindowHint);
    popup.setModal(true);
    popup.setMinimumWidth(320);
    popup.setStyleSheet(QStringLiteral(
        "QDialog{background:%1;border:1px solid %2;}"
        "QTabWidget::pane{border:1px solid %2;background:%1;}"
        "QTabBar::tab{color:%3;background:%4;border:1px solid %2;padding:5px 10px;}"
        "QTabBar::tab:selected{background:%5;color:%6;}"
        "QLabel{color:%3;background:transparent;}"
        "QLineEdit{color:%3;background:%7;border:1px solid %2;border-radius:2px;padding:3px 5px;}"
        "QPushButton{color:%3;background:%4;border:1px solid %2;border-radius:2px;padding:4px 8px;}"
        "QPushButton:hover{background:%8;}"
    ).arg(qApp->palette().color(QPalette::Window).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Mid).name(QColor::HexRgb),
          qApp->palette().color(QPalette::WindowText).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Button).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Highlight).name(QColor::HexRgb),
          qApp->palette().color(QPalette::HighlightedText).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Base).name(QColor::HexRgb),
          (qApp->palette().color(QPalette::Button).lightness() < 128
              ? qApp->palette().color(QPalette::Button).lighter(125)
              : qApp->palette().color(QPalette::Button).darker(108)).name(QColor::HexRgb)));

    auto *root = new QVBoxLayout(&popup);
    root->setContentsMargins(8, 8, 8, 8);
    auto *tabs = new QTabWidget(&popup);
    root->addWidget(tabs);

    auto *color_tab = new QWidget(tabs);
    auto *color_layout = new QVBoxLayout(color_tab);
    color_layout->setContentsMargins(8, 8, 8, 8);
    color_layout->setSpacing(8);
    auto *label = new QLabel(foreground ? obsgs_tr("OBSTitles.DefaultForegroundColor")
                                         : obsgs_tr("OBSTitles.DefaultBackgroundColor"), color_tab);
    auto *swatch = new QPushButton(color_tab);
    swatch->setFixedSize(90, 52);
    auto *hex = new QLineEdit(color_tab);
    auto update_controls = [&]() {
        swatch->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid %2;border-radius:2px;}")
            .arg(target.name(QColor::HexArgb), qApp->palette().color(QPalette::Mid).name(QColor::HexRgb)));
        hex->setText(target.alpha() < 255 ? target.name(QColor::HexArgb).toUpper()
                                          : target.name(QColor::HexRgb).toUpper());
        update_sidebar_color_swatches(nullptr);
    };
    auto apply_color = [&](const QColor &color) {
        if (!color.isValid()) return;
        target = color;
        const uint32_t argb = argb_from_color(color);
        if (foreground) {
            default_new_layer_style_.fill_type = 0;
            default_new_layer_style_.fill_color = argb;
            default_new_layer_style_.text_color = argb;
        } else {
            default_new_layer_style_.stroke_fill_type = 1;
            default_new_layer_style_.stroke_color = argb;
        }
        save_sidebar_default_colors();
        save_new_layer_defaults();
        update_controls();
    };
    connect(swatch, &QPushButton::clicked, &popup, [&]() {
        QColor picked = obsgs_pick_color(target, &popup,
            foreground ? obsgs_tr("OBSTitles.ForegroundColor") : obsgs_tr("OBSTitles.BackgroundColor"));
        apply_color(picked);
    });
    connect(hex, &QLineEdit::editingFinished, &popup, [&]() {
        QColor parsed(hex->text().trimmed());
        if (parsed.isValid()) apply_color(parsed);
        else update_controls();
    });
    color_layout->addWidget(label);
    color_layout->addWidget(swatch, 0, Qt::AlignLeft);
    color_layout->addWidget(hex);
    tabs->addTab(color_tab, obsgs_tr("OBSTitles.Color"));

    auto *gradient_tab = new QWidget(tabs);
    auto *gradient_layout = new QVBoxLayout(gradient_tab);
    auto *gradient_label = new QLabel(obsgs_tr("OBSTitles.GradientDefaultsHint"), gradient_tab);
    gradient_label->setWordWrap(true);
    gradient_layout->addWidget(gradient_label);
    gradient_layout->addStretch(1);
    tabs->addTab(gradient_tab, obsgs_tr("OBSTitles.Gradient"));

    auto *swatches_tab = new QWidget(tabs);
    auto *swatches_layout = new QVBoxLayout(swatches_tab);
    auto *swatches_label = new QLabel(obsgs_tr("OBSTitles.SavedDefaultsHint"), swatches_tab);
    swatches_label->setWordWrap(true);
    swatches_layout->addWidget(swatches_label);
    swatches_layout->addStretch(1);
    tabs->addTab(swatches_tab, obsgs_tr("OBSTitles.Swatches"));

    update_controls();
    popup.adjustSize();
    const QPoint cursor_pos = QCursor::pos();
    const QPoint desired_pos(cursor_pos.x() + 14, cursor_pos.y() - popup.height() / 2);
    popup.move(clamp_popup_position_to_screen(desired_pos, popup.size(), this));
    popup.exec();
}

void TitleEditor::update_layer_panels(std::shared_ptr<Layer> layer, double playhead)
{
    QScopedValueRollback<bool> panel_update_guard(updating_layer_panels_, true);
    if (layer_props_dock_) {
        layer_props_dock_->setWindowTitle(layer
            ? obsgs_tr("OBSTitles.PropertiesNamed").arg(QString::fromStdString(layer->name))
            : obsgs_tr("OBSTitles.PropertiesNoSelection"));
    }
    if (props_) props_->set_layer(layer, playhead);
    update_sidebar_color_swatches(layer);
    if (effects_panel_) effects_panel_->set_layer(layer, playhead);
    update_corner_toolbar();
}

void TitleEditor::on_layer_selected(const std::string &lid)
{
    sel_layer_id_ = lid;
    layers_->set_selected_layer(lid);
    canvas_->set_selected_layer(lid);
    timeline_->set_selected_layer(lid);

    if (!title_ || lid.empty()) {
        update_layer_panels(nullptr, playhead_);
        return;
    }
    auto layer = title_->find_layer(lid);
    if (layer) update_layer_panels(layer, playhead_);
}

void TitleEditor::on_playhead_changed(double t)
{
    t = title_ ? std::clamp(snap_to_obs_frame(t), 0.0, title_->duration) : snap_to_obs_frame(t);
    if (std::abs(t - playhead_) < 1e-9)
        return;
    playhead_ = t;
    canvas_->set_playhead(t);
    timeline_->set_playhead(t);
    if (prerender_panel_) prerender_panel_->setPlayhead(t);
    if (!cache_reprioritize_clock_.isValid() || cache_reprioritize_clock_.elapsed() >= 80) {
        CacheManager::instance().reprioritize(title_, t);
        cache_reprioritize_clock_.restart();
    }

    if (!sel_layer_id_.empty() && title_) {
        auto l = title_->find_layer(sel_layer_id_);
        if (l) {
            QScopedValueRollback<bool> panel_update_guard(updating_layer_panels_, true);
            if (props_)
                props_->update_playhead(t);
            if (effects_panel_)
                effects_panel_->update_playhead(t);
            update_sidebar_color_swatches(l);
        }
    }

    if (time_lbl_)
        time_lbl_->setText(obsgs_tr("OBSTitles.TimeFpsFormat").arg(format_timecode(t)).arg(obs_frame_rate(), 0, 'f', 2));
}

void TitleEditor::on_title_modified(bool push_undo)
{
    const bool force_visual_update = force_next_visual_update_;
    force_next_visual_update_ = false;
    /* Several Qt controls emit value-changed signals while a newly selected
     * layer is being reflected into the properties/effects panels. Selection
     * itself is UI state and must never dirty, save, invalidate, or rerender
     * the title. Compare against the last rendered/restored visual identity
     * before entering the modification pipeline. Genuine edits have already
     * changed the model here and therefore continue normally. */
    if (!force_visual_update && title_ && CacheManager::instance().visualStateCurrent(*title_)) {
        OGS_LOG_TRACE("Editor", QStringLiteral("Ignored selection-only property notification title=%1")
                                   .arg(QString::fromStdString(title_->id)));
        return;
    }
    if (title_) set_dirty(true);
    schedule_cache_invalidation();
    canvas_->refresh_preview();
    if (title_props_) title_props_->set_title(title_);
    if (timeline_) timeline_->set_title(title_);
    if (title_ && !sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) {
            set_default_sidebar_colors_from_layer(*layer);
            update_sidebar_color_swatches(layer);
        }
    } else {
        update_sidebar_color_swatches(nullptr);
    }
    if (push_undo)
        push_undo_snapshot();
    save_live_edit();
}

void TitleEditor::force_next_title_visual_update()
{
    force_next_visual_update_ = true;
}

void TitleEditor::schedule_cache_invalidation()
{
    if (!title_)
        return;
    CacheManager::instance().setInteractiveBypass(true);
    if (cache_invalidation_timer_)
        cache_invalidation_timer_->start();
}

/* ══════════════════════════════════════════════════════════════════
 *  CanvasPreview
 * ══════════════════════════════════════════════════════════════════ */
