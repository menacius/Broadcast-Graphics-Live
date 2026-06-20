/*
 * title-dock.cpp
 */

#include "title-dock.h"
#include "title-editor.h"
#include "title-data.h"
#include "title-source.h"
#include "title-assets.h"
#include "timecode-spinbox.h"
#include "title-localization.h"
#include "cache-manager.h"
#include "title-logger.h"
#include "image-layer-utils.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QIODevice>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMimeData>
#include <QTextEdit>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QIcon>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QApplication>
#include <QToolButton>
#include <QToolBar>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QWidgetAction>
#include <QPushButton>
#include <QListView>
#include <QListWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QFocusEvent>
#include <QSizePolicy>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSpinBox>
#include <QTextOption>
#include <QTextDocument>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolTip>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QRegularExpression>
#include <memory>
#include <algorithm>
#include <functional>
#include <numeric>
#include <iterator>
#include <array>
#include <cmath>
#include <cstring>

namespace {

constexpr int TemplateCategoryNameRole = Qt::UserRole + 1;
constexpr const char *kDockSettingsGroup = "TitleDock";
constexpr const char *kDockSplitterStateKey = "sectionSplitterState";
constexpr const char *kTemplateIconViewKey = "templateIconView";
constexpr const char *kVisibilityFilterKey = "visibilityFilterActive";
constexpr const char *kLastSelectedTitleIdKey = "lastSelectedTitleId";
constexpr const char *kTitleSourceId = "obs_graphics_studio_pro_source";
constexpr const char *kPlaylistLoopKey = "playlistLoop";
constexpr const char *kPlaylistReverseKey = "playlistReverse";
constexpr const char *kPlaylistHoldSecondsKey = "playlistHoldSeconds";
constexpr const char *kBackgroundPersistenceKey = "backgroundPersistence";
constexpr const char *kTextPersistenceKey = "textPersistence";
constexpr const char *kLiveTextLinesPerRowKey = "liveTextLinesPerRow";
constexpr int kMinLiveTextLinesPerRow = 1;
constexpr int kMaxLiveTextLinesPerRow = 8;
constexpr int kTitleListIconExtent = 16;
constexpr int kTitleIconViewThumbWidth = 144;
constexpr int kTitleIconViewThumbHeight = 82;
constexpr int kTitleIconViewItemWidth = 172;
constexpr int kTitleIconViewItemHeight = 126;
constexpr int kTitleIconViewTextLines = 2;
constexpr int kTitlePlaylistActiveRole = Qt::UserRole + 20;
constexpr int kLiveCueColumn = 0;
constexpr int kLiveCacheColumn = 1;
constexpr int kLiveSelectColumn = 2;
constexpr int kLiveFirstValueColumn = 3;
constexpr int kStaticCueColumn = 0;
constexpr int kStaticCacheColumn = 1;
constexpr int kStaticTitleColumn = 2;

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

static std::vector<std::shared_ptr<Layer>> order_exposed_text_layers(
    const std::vector<std::shared_ptr<Layer>> &exposed,
    const std::vector<std::string> &column_order)
{
    if (column_order.empty())
        return exposed;

    std::vector<std::shared_ptr<Layer>> ordered;
    ordered.reserve(exposed.size());
    for (const auto &layer_id : column_order) {
        auto it = std::find_if(exposed.begin(), exposed.end(),
                               [&](const std::shared_ptr<Layer> &layer) {
                                   return layer && layer->id == layer_id;
                               });
        if (it != exposed.end())
            ordered.push_back(*it);
    }
    for (const auto &layer : exposed) {
        if (!layer) continue;
        auto it = std::find_if(ordered.begin(), ordered.end(),
                               [&](const std::shared_ptr<Layer> &ordered_layer) {
                                   return ordered_layer && ordered_layer->id == layer->id;
                               });
        if (it == ordered.end())
            ordered.push_back(layer);
    }
    return ordered;
}

static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    if (!title) return exposed;
    for (const auto &layer : title->layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker || layer->type == LayerType::Image) &&
            layer->expose_text)
            exposed.push_back(layer);
    }
    return order_exposed_text_layers(exposed, title->live_text_column_order);
}

static bool layer_uses_image_cue_value(const std::shared_ptr<Layer> &layer)
{
    return layer && layer->type == LayerType::Image;
}

static std::string live_cue_layer_value(const std::shared_ptr<Layer> &layer)
{
    if (!layer)
        return {};
    return layer_uses_image_cue_value(layer) ? layer->image_path : layer->text_content;
}

static void apply_live_cue_layer_value(const std::shared_ptr<Layer> &target, const std::string &value)
{
    if (!target)
        return;
    target->live_cue_hidden_if_empty = target->exposed_hide_if_empty && value.empty();
    if (target->type == LayerType::Image) {
        gsp::apply_exposed_image_cue_value(*target, value);
        return;
    }

    target->text_content = value;
    if (target->rich_text.empty())
        target->rich_text = rich_text_document_from_layer_defaults(*target);
    RichTextCharFormat insertion_format = target->rich_text.has_typing_format
        ? target->rich_text.typing_format
        : target->rich_text.default_format;
    rich_text_document_replace_text(target->rich_text, target->text_content, &insertion_format);
    target->rich_text_html.clear();
}

static QString live_cue_image_file_name(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return QString();
    const QFileInfo info(trimmed);
    const QString file_name = info.fileName();
    return file_name.isEmpty() ? trimmed : file_name;
}

static QString current_scene_collection_titles_label()
{
    char *collection_name = obs_frontend_get_current_scene_collection();
    QString name = QString::fromUtf8(collection_name ? collection_name : "").trimmed();
    bfree(collection_name);

    if (name.isEmpty())
        name = obsgs_tr("OBSTitles.SceneCollectionFallback");

    return obsgs_tr("OBSTitles.SceneCollectionTitlesAndGraphicsFormat").arg(name);
}

static QString live_text_layer_header(const std::shared_ptr<Layer> &layer)
{
    if (!layer) return obsgs_tr("OBSTitles.Text");
    QString name = QString::fromStdString(layer->name).trimmed();
    if (!name.isEmpty()) return name;
    if (layer_uses_image_cue_value(layer)) {
        name = live_cue_image_file_name(QString::fromStdString(layer->image_path)).trimmed();
        return name.isEmpty() ? obsgs_tr("OBSTitles.Image") : name;
    }
    name = QString::fromStdString(layer->text_content).trimmed();
    return name.isEmpty() ? obsgs_tr("OBSTitles.Text") : name;
}

static void normalize_live_text_rows(const std::shared_ptr<Title> &title,
                                     const std::vector<std::shared_ptr<Layer>> &exposed)
{
    if (!title || exposed.empty()) return;

    std::vector<std::string> new_order;
    new_order.reserve(exposed.size());
    for (const auto &layer : exposed)
        new_order.push_back(layer ? layer->id : std::string());

    const std::vector<std::string> old_order = title->live_text_column_order;
    if (!old_order.empty() && old_order != new_order) {
        for (auto &row : title->live_text_rows) {
            std::vector<std::string> remapped;
            remapped.reserve(exposed.size());
            for (size_t new_col = 0; new_col < new_order.size(); ++new_col) {
                auto it = std::find(old_order.begin(), old_order.end(), new_order[new_col]);
                if (it != old_order.end()) {
                    const size_t old_col = (size_t)std::distance(old_order.begin(), it);
                    remapped.push_back(old_col < row.size() ? row[old_col] : live_cue_layer_value(exposed[new_col]));
                } else {
                    remapped.push_back(live_cue_layer_value(exposed[new_col]));
                }
            }
            row = std::move(remapped);
        }
    }
    title->live_text_column_order = std::move(new_order);

    if (title->live_text_rows.empty()) {
        std::vector<std::string> row;
        for (const auto &layer : exposed)
            row.push_back(live_cue_layer_value(layer));
        title->live_text_rows.push_back(std::move(row));
    }
    for (auto &row : title->live_text_rows) {
        size_t old_size = row.size();
        row.resize(exposed.size());
        for (size_t i = old_size; i < exposed.size(); ++i)
            row[i] = live_cue_layer_value(exposed[i]);
    }
    for (size_t col = 0; col < exposed.size(); ++col) {
        if (!exposed[col] || !exposed[col]->exposed_single_value || title->live_text_rows.empty())
            continue;
        const std::string shared_value = col < title->live_text_rows.front().size()
            ? title->live_text_rows.front()[col]
            : live_cue_layer_value(exposed[col]);
        for (auto &row : title->live_text_rows) {
            if (col < row.size())
                row[col] = shared_value;
        }
    }
    ensure_live_text_row_ids(*title);
}

static QString live_cue_cache_text(FrameCacheState state)
{
    switch (state) {
    case FrameCacheState::Queued: return obsgs_tr("OBSTitles.Queued");
    case FrameCacheState::Rendering: return obsgs_tr("OBSTitles.Caching");
    case FrameCacheState::CachedRam: return obsgs_tr("OBSTitles.ReadyRam");
    case FrameCacheState::CachedDisk: return obsgs_tr("OBSTitles.ReadyDisk");
    case FrameCacheState::Stale: return obsgs_tr("OBSTitles.CacheErrorOrStale");
    case FrameCacheState::Disabled: return obsgs_tr("OBSTitles.CachingDisabled");
    case FrameCacheState::NotCached:
    default: return obsgs_tr("OBSTitles.NotCached");
    }
}

static QString live_cue_cache_tooltip(FrameCacheState state)
{
    switch (state) {
    case FrameCacheState::Queued:
        return obsgs_tr("OBSTitles.LiveCueQueuedTooltip");
    case FrameCacheState::Rendering:
        return obsgs_tr("OBSTitles.LiveCueCachingTooltip");
    case FrameCacheState::CachedRam:
        return obsgs_tr("OBSTitles.LiveCueCachedRamTooltip");
    case FrameCacheState::CachedDisk:
        return obsgs_tr("OBSTitles.LiveCueCachedDiskTooltip");
    case FrameCacheState::Stale:
        return obsgs_tr("OBSTitles.LiveCueStaleTooltip");
    case FrameCacheState::Disabled:
        return obsgs_tr("OBSTitles.LiveCueCacheDisabledTooltip");
    case FrameCacheState::NotCached:
    default:
        return obsgs_tr("OBSTitles.LiveCueNotCachedTooltip");
    }
}

static QColor live_cue_cache_color(FrameCacheState state)
{
    switch (state) {
    /* Queued and Rendering are a single visual progress phase.  Giving them
     * different tints made the same bucket flicker orange/yellow every time
     * ownership moved between the queue and worker.  The four SVG shapes, not
     * the scheduler's transient state, now communicate prerender progress. */
    case FrameCacheState::Queued:
    case FrameCacheState::Rendering: return QColor(255, 202, 74);
    case FrameCacheState::CachedRam: return QColor(39, 186, 103);
    case FrameCacheState::CachedDisk: return QColor(74, 144, 226);
    case FrameCacheState::Stale: return QColor(214, 90, 90);
    case FrameCacheState::Disabled: return QColor(130, 130, 130);
    case FrameCacheState::NotCached:
    default: return QColor(130, 130, 130);
    }
}

static const char *live_cue_cache_progress_icon_name(int progress_percent)
{
    const int progress = std::clamp(progress_percent, 0, 100);
    if (progress >= 100)
        return "cache-ok.svg";
    if (progress >= 75)
        return "cache-progress-75-100.svg";
    if (progress >= 50)
        return "cache-progress-50-75.svg";
    if (progress >= 25)
        return "cache-progress-25-50.svg";
    return "cache-queued-0-25.svg";
}

static const char *live_cue_cache_icon_name(FrameCacheState state, int progress_percent)
{
    switch (state) {
    case FrameCacheState::Queued:
    case FrameCacheState::Rendering:
        return live_cue_cache_progress_icon_name(progress_percent);
    case FrameCacheState::CachedRam:
    case FrameCacheState::CachedDisk:
    case FrameCacheState::Disabled:
        return "cache-ok.svg";
    case FrameCacheState::Stale:
        return "cache-error.svg";
    case FrameCacheState::NotCached:
    default:
        return "cache-queued-0-25.svg";
    }
}

static QIcon obs_icon(const char *file_name, const QColor &color);
static QIcon title_list_status_icon(bool has_exposed_text, bool has_scene_mask);

struct LiveCueAggregateCacheStatus {
    FrameCacheState state = FrameCacheState::NotCached;
    int progress_percent = 0;
    int ready_rows = 0;
    int total_rows = 0;
};

static LiveCueAggregateCacheStatus aggregate_live_cue_cache_status(const std::shared_ptr<Title> &title)
{
    LiveCueAggregateCacheStatus status;
    if (!title || title->live_text_rows.empty()) {
        status.state = CacheManager::instance().cacheEnabled()
            ? FrameCacheState::NotCached
            : FrameCacheState::Disabled;
        return status;
    }

    status.total_rows = static_cast<int>(title->live_text_rows.size());
    if (!CacheManager::instance().cacheEnabled()) {
        status.state = FrameCacheState::Disabled;
        return status;
    }

    for (int row = 0; row < status.total_rows; ++row) {
        const FrameCacheState row_state = CacheManager::instance().liveCueState(title, row);
        if (row_state == FrameCacheState::CachedRam || row_state == FrameCacheState::CachedDisk)
            ++status.ready_rows;
    }

    /* Aggregate progress includes steady row frames and the independent
     * Background Persistence/outro transition-pair states. Row icons remain
     * stable when a new row is added; only the title-level badge reflects the
     * newly required transition work. */
    status.progress_percent = CacheManager::instance().liveCueAggregateProgressPercent(title);
    status.state = CacheManager::instance().liveCueAggregateState(title);
    return status;
}


static QString aggregate_live_cue_cache_tooltip(const LiveCueAggregateCacheStatus &status)
{
    if (status.total_rows <= 0)
        return obsgs_tr("OBSTitles.LiveCueNotCachedTooltip");
    return QStringLiteral("%1 (%2%, %3/%4 cues)")
        .arg(live_cue_cache_text(status.state))
        .arg(status.progress_percent)
        .arg(status.ready_rows)
        .arg(status.total_rows);
}

static QIcon title_list_combined_status_icon(bool has_exposed_text, bool has_scene_mask,
                                             const LiveCueAggregateCacheStatus &cache_status)
{
    constexpr int gap = 3;
    const int width = kTitleListIconExtent * 2 + gap;
    QPixmap pixmap(width, kTitleListIconExtent);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    const QPixmap type_pixmap = title_list_status_icon(has_exposed_text, has_scene_mask).pixmap(kTitleListIconExtent, kTitleListIconExtent);
    const QPixmap cache_pixmap = obs_icon(live_cue_cache_icon_name(cache_status.state, cache_status.progress_percent),
                                          live_cue_cache_color(cache_status.state)).pixmap(kTitleListIconExtent, kTitleListIconExtent);
    painter.drawPixmap(0, 0, type_pixmap);
    painter.drawPixmap(kTitleListIconExtent + gap, 0, cache_pixmap);
    painter.end();
    return QIcon(pixmap);
}

static QToolButton *centered_cell_tool_button(QTableWidget *table, int row, int col, const char *object_name)
{
    if (!table || row < 0 || row >= table->rowCount() || col < 0 || col >= table->columnCount())
        return nullptr;

    if (auto *cell = table->cellWidget(row, col)) {
        if (auto *button = cell->findChild<QToolButton *>(QString::fromUtf8(object_name)))
            return button;
        if (auto *button = qobject_cast<QToolButton *>(cell))
            return button;
    }

    auto *holder = new QWidget(table);
    auto *layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignCenter);

    auto *button = new QToolButton(holder);
    button->setObjectName(QString::fromUtf8(object_name));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(28, 24);
    layout->addWidget(button, 0, Qt::AlignCenter);

    table->setCellWidget(row, col, holder);
    return button;
}

static QToolButton *live_cue_cache_button(QTableWidget *table, int row)
{
    if (!table)
        return nullptr;
    const int col = table->columnCount() <= 3 ? kStaticCacheColumn : kLiveCacheColumn;
    return centered_cell_tool_button(table, row, col, "liveCueCacheButton");
}

static QToolButton *live_cue_button(QTableWidget *table, int row, int col)
{
    auto *button = centered_cell_tool_button(table, row, col, "liveCueButton");
    if (button) {
        button->setIconSize(QSize(20, 20));
        /* The cue control owns the complete table cell, while the icon remains
         * centered by QToolButton.  Clicking whitespace in the cue column now
         * triggers the same action as clicking the icon itself. */
        button->setMinimumSize(0, 0);
        button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (auto *layout = button->parentWidget() ? button->parentWidget()->layout() : nullptr) {
            layout->setAlignment(Qt::Alignment{});
            layout->setAlignment(button, Qt::Alignment{});
        }
    }
    return button;
}

static QColor live_cue_state_color(bool current, bool queued)
{
    if (current)
        return QColor(208, 48, 48);
    if (queued)
        return QColor(40, 184, 79);
    return QColor(150, 150, 150);
}

static QColor live_cue_select_cell_color(bool current, bool queued)
{
    if (current)
        return QColor(208, 48, 48, 72);
    if (queued)
        return QColor(40, 184, 79, 72);
    return QColor();
}

static QIcon obs_icon(const char *file_name)
{
    return obsgs_icon(file_name);
}

static QIcon obs_icon(const char *file_name, const QColor &color)
{
    return obsgs_icon(file_name, color);
}

static std::string obs_text_std(const char *key)
{
    return obsgs_tr(key).toStdString();
}

static int obs_toolbar_icon_extent(QWidget *widget)
{
    int size = widget ? widget->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, widget) : 0;
    return size > 0 ? size : 16;
}

static int obs_layout_spacing(QWidget *widget)
{
    int spacing = widget ? widget->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, widget) : -1;
    return spacing >= 0 ? spacing : 4;
}

static QToolBar *make_obs_dock_toolbar(QWidget *parent)
{
    auto *toolbar = new QToolBar(parent);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setOrientation(Qt::Horizontal);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return toolbar;
}

static QToolButton *make_obs_dock_tool_button(QWidget *parent, const QString &text,
                                              const QIcon &icon, const QString &tooltip)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setIcon(icon);
    button->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return button;
}

static QString dock_highlight_button_style(bool hide_menu_indicator = false,
                                           bool include_persistence_state = false)
{
    QString style;
    if (include_persistence_state) {
        style += QStringLiteral(
            "QToolButton[persistenceState=\"background\"]{"
            "background:palette(midlight);color:palette(text);"
            "border:1px solid palette(highlight);border-radius:3px;}"
            "QToolButton[persistenceState=\"background\"]:hover{background:palette(mid);}");
    }
    style += QStringLiteral(
        "QToolButton:checked{"
        "background:palette(highlight);color:palette(highlighted-text);"
        "border:1px solid palette(highlight);border-radius:3px;}"
        "QToolButton:checked:hover{background:palette(highlight);color:palette(highlighted-text);}");
    if (hide_menu_indicator)
        style += QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}");
    return style;
}

static LongPressToolButton *make_obs_dock_long_press_tool_button(QWidget *parent, const QString &text,
                                                                 const QIcon &icon, const QString &tooltip)
{
    auto *button = new LongPressToolButton(parent);
    button->setText(text);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setIcon(icon);
    button->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    button->setPopupMode(QToolButton::DelayedPopup);
    return button;
}


static QWidget *toolbar_spacer(QWidget *parent)
{
    auto *spacer = new QWidget(parent);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return spacer;
}

static QIcon globe_status_icon(bool enabled)
{
    const QColor color = enabled ? QColor(38, 184, 79) : QColor(145, 145, 145);
    return obs_icon("globe.svg", color);
}

static QJsonArray live_text_rows_to_json(const std::vector<std::vector<std::string>> &rows)
{
    QJsonArray json_rows;
    for (const auto &row : rows) {
        QJsonArray json_row;
        for (const auto &cell : row)
            json_row.append(QString::fromStdString(cell));
        json_rows.append(json_row);
    }
    return json_rows;
}

static bool live_text_rows_from_json(const QJsonDocument &doc,
                                     std::vector<std::vector<std::string>> &rows,
                                     QString *error)
{
    QJsonArray json_rows;
    if (doc.isArray()) {
        json_rows = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonValue rows_value = root.value(QStringLiteral("rows"));
        const QJsonValue live_rows_value = root.value(QStringLiteral("live_text_rows"));
        if (rows_value.isArray())
            json_rows = rows_value.toArray();
        else if (live_rows_value.isArray())
            json_rows = live_rows_value.toArray();
        else {
            if (error) *error = obsgs_tr("OBSTitles.LiveCueJsonRowsError");
            return false;
        }
    } else {
        if (error) *error = obsgs_tr("OBSTitles.LiveCueJsonInvalidError");
        return false;
    }

    rows.clear();
    rows.reserve(json_rows.size());
    for (const QJsonValue &row_value : json_rows) {
        if (!row_value.isArray()) {
            if (error) *error = obsgs_tr("OBSTitles.LiveCueJsonRowArrayError");
            return false;
        }
        std::vector<std::string> row;
        const QJsonArray json_row = row_value.toArray();
        row.reserve(json_row.size());
        for (const QJsonValue &cell_value : json_row)
            row.push_back(cell_value.toVariant().toString().toStdString());
        rows.push_back(std::move(row));
    }
    return true;
}

static void set_bold_label(QLabel *label)
{
    if (!label) return;
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
}


class LiveTextCueField : public QPlainTextEdit {
public:
    explicit LiveTextCueField(const QString &text, QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
    {
        setPlainText(text);
        update_tooltip(text);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTabChangesFocus(true);
        document()->setDocumentMargin(0.0);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(this, &QPlainTextEdit::textChanged, this, [this]() {
            update_tooltip(toPlainText());
        });
    }

    void update_tooltip(const QString &text)
    {
        setToolTip(text);
        if (viewport())
            viewport()->setToolTip(text);
    }

    std::function<void(const QString &)> editing_finished;
    std::function<void(bool)> focus_changed;

protected:
    void focusInEvent(QFocusEvent *event) override
    {
        QPlainTextEdit::focusInEvent(event);
        if (focus_changed)
            focus_changed(true);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            !event->modifiers().testFlag(Qt::ShiftModifier)) {
            if (editing_finished)
                editing_finished(toPlainText());
            focusNextChild();
            event->accept();
            return;
        }

        // A literal newline is intentionally available only through Shift+Enter.
        QPlainTextEdit::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QPlainTextEdit::focusOutEvent(event);
        if (editing_finished)
            editing_finished(toPlainText());
        if (focus_changed) {
            const auto callback = focus_changed;
            QTimer::singleShot(0, qApp, [callback]() { callback(false); });
        }
    }
};

class LiveImageCueField : public QWidget {
public:
    explicit LiveImageCueField(const QString &path, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 2, 3, 2);
        layout->setSpacing(4);

        label_ = new QLabel(this);
        label_->setTextInteractionFlags(Qt::NoTextInteraction);
        label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        label_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        button_ = new QToolButton(this);
        button_->setAutoRaise(true);
        button_->setCursor(Qt::ArrowCursor);
        button_->setIcon(obs_icon("import.svg", palette().color(QPalette::ButtonText)));
        button_->setToolTip(obsgs_tr("OBSTitles.Browse"));
        button_->setFixedSize(22, 22);

        layout->addWidget(label_, 1);
        layout->addWidget(button_, 0);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        label_->installEventFilter(this);
        button_->installEventFilter(this);
        set_path(path);
        connect(button_, &QToolButton::clicked, this, [this]() {
            const QString initial_dir = path_.isEmpty()
                ? QString()
                : QFileInfo(path_).absolutePath();
            const QString selected = QFileDialog::getOpenFileName(
                this, obsgs_tr("OBSTitles.ChooseImage"), initial_dir,
                obsgs_tr("OBSTitles.ImageFileFilter"));
            if (selected.isEmpty())
                return;
            set_path(selected);
            if (path_changed)
                path_changed(selected);
        });
    }

    QString path() const { return path_; }

    void set_path(const QString &path)
    {
        path_ = path;
        const QString file_name = live_cue_image_file_name(path_);
        label_->setText(file_name.isEmpty() ? QStringLiteral("-") : file_name);
        preview_tooltip_ = path_.isEmpty() ? QString() : path_.toHtmlEscaped();
        preview_tooltip_dirty_ = true;
        setToolTip(preview_tooltip_);
        label_->setToolTip(preview_tooltip_);
    }

    std::function<void(const QString &)> path_changed;

protected:
    bool event(QEvent *event) override
    {
        if (event && event->type() == QEvent::ToolTip)
            return show_preview_tooltip(static_cast<QHelpEvent *>(event));
        return QWidget::event(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((watched == label_ || watched == button_) &&
            event && event->type() == QEvent::ToolTip)
            return show_preview_tooltip(static_cast<QHelpEvent *>(event));
        return QWidget::eventFilter(watched, event);
    }

private:
    bool show_preview_tooltip(QHelpEvent *event)
    {
        ensure_preview_tooltip();
        if (!event || preview_tooltip_.isEmpty())
            return false;
        QToolTip::showText(event->globalPos(), preview_tooltip_, this);
        return true;
    }

    void ensure_preview_tooltip()
    {
        if (!preview_tooltip_dirty_)
            return;
        preview_tooltip_dirty_ = false;

        const QFileInfo info(path_);
        if (!info.exists() || !info.isFile()) {
            preview_tooltip_ = path_.isEmpty() ? QString() : path_.toHtmlEscaped();
            setToolTip(preview_tooltip_);
            if (label_) label_->setToolTip(preview_tooltip_);
            return;
        }

        QImage image(path_);
        if (image.isNull()) {
            preview_tooltip_ = path_.toHtmlEscaped();
            setToolTip(preview_tooltip_);
            if (label_) label_->setToolTip(preview_tooltip_);
            return;
        }

        const QSize preview_size = image.size().scaled(QSize(240, 180), Qt::KeepAspectRatio);
        preview_tooltip_ = QStringLiteral(
            "<div style=\"white-space:nowrap;\"><b>%1</b><br/>%2 x %3<br/>"
            "<img src=\"%4\" width=\"%5\" height=\"%6\"/></div>")
            .arg(info.fileName().toHtmlEscaped())
            .arg(image.width())
            .arg(image.height())
            .arg(QUrl::fromLocalFile(info.absoluteFilePath()).toString().toHtmlEscaped())
            .arg(preview_size.width())
            .arg(preview_size.height());
        setToolTip(preview_tooltip_);
        if (label_) label_->setToolTip(preview_tooltip_);
    }

    QLabel *label_ = nullptr;
    QToolButton *button_ = nullptr;
    QString path_;
    QString preview_tooltip_;
    bool preview_tooltip_dirty_ = true;
};

class LiveTextCueTable : public QTableWidget {
public:
    explicit LiveTextCueTable(QWidget *parent = nullptr)
        : QTableWidget(parent)
    {
        setMouseTracking(false);
        viewport()->setMouseTracking(false);
    }

    std::function<QColor(int)> cue_row_border_color;
    std::function<void()> layout_requested;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTableWidget::resizeEvent(event);
        if (layout_requested)
            layout_requested();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event && event->buttons() == Qt::NoButton)
            return;
        QTableWidget::mouseMoveEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTableWidget::paintEvent(event);

        if (!cue_row_border_color || rowCount() <= 0)
            return;

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (int row = 0; row < rowCount(); ++row) {
            if (isRowHidden(row))
                continue;
            const QColor color = cue_row_border_color(row);
            if (!color.isValid() || color.alpha() == 0)
                continue;

            const int y = rowViewportPosition(row);
            const int h = rowHeight(row);
            if (y + h < 0 || y > viewport()->height())
                continue;

            QColor border = color;
            border.setAlpha(220);
            painter.setPen(QPen(border, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRect(1, y + 1, viewport()->width() - 3, h - 3));
        }
    }
};

class TitleListWidget : public QListWidget {
public:
    explicit TitleListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        setAcceptDrops(true);
        setDragEnabled(false);
        setDragDropMode(QAbstractItemView::DropOnly);
        setDefaultDropAction(Qt::CopyAction);
        setDropIndicatorShown(false);
        setDragDropOverwriteMode(false);

        if (viewport()) {
            viewport()->setAcceptDrops(true);
            viewport()->installEventFilter(this);
        }
    }

    std::function<void()> delete_requested;
    std::function<void(const QStringList &)> files_dropped;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event && event->key() == Qt::Key_Delete && hasFocus() && !selectedItems().empty()) {
            if (delete_requested)
                delete_requested();
            event->accept();
            return;
        }

        QListWidget::keyPressEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (handle_drag_enter_or_move(event))
            return;
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (handle_drag_enter_or_move(event))
            return;
        QListWidget::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        set_drag_import_active(false);
        QListWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (handle_drop(event))
            return;
        QListWidget::dropEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == viewport() && event) {
            switch (event->type()) {
            case QEvent::DragEnter:
                if (handle_drag_enter_or_move(static_cast<QDragEnterEvent *>(event)))
                    return true;
                break;
            case QEvent::DragMove:
                if (handle_drag_enter_or_move(static_cast<QDragMoveEvent *>(event)))
                    return true;
                break;
            case QEvent::DragLeave:
                set_drag_import_active(false);
                break;
            case QEvent::Drop:
                if (handle_drop(static_cast<QDropEvent *>(event)))
                    return true;
                break;
            default:
                break;
            }
        }
        return QListWidget::eventFilter(watched, event);
    }

private:
    template <typename DragEvent>
    bool handle_drag_enter_or_move(DragEvent *event)
    {
        const QStringList paths = accepted_template_paths(event ? event->mimeData() : nullptr);
        if (paths.isEmpty()) {
            if (event) event->ignore();
            set_drag_import_active(false);
            return false;
        }

        set_drag_import_active(true);
        if (event) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
        return true;
    }

    bool handle_drop(QDropEvent *event)
    {
        const QStringList paths = accepted_template_paths(event ? event->mimeData() : nullptr);
        set_drag_import_active(false);
        if (paths.isEmpty()) {
            if (event) event->ignore();
            return false;
        }

        if (files_dropped)
            files_dropped(paths);

        if (event) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
        return true;
    }

    static bool is_supported_template_file(const QFileInfo &info)
    {
        if (!info.exists() || !info.isFile() || !info.isReadable())
            return false;

        const QString suffix = info.suffix().toLower();
        return suffix == QStringLiteral("ogspt") ||
               suffix == QStringLiteral("otpt") ||
               suffix == QStringLiteral("json");
    }

    static void append_path_if_supported(QStringList &paths, const QString &raw_path)
    {
        const QString cleaned = raw_path.trimmed();
        if (cleaned.isEmpty())
            return;

        const QFileInfo info(cleaned);
        if (is_supported_template_file(info))
            paths.push_back(info.absoluteFilePath());
    }

    static QStringList accepted_template_paths(const QMimeData *mime)
    {
        QStringList paths;
        if (!mime)
            return paths;

        if (mime->hasUrls()) {
            const auto urls = mime->urls();
            for (const QUrl &url : urls) {
                if (!url.isLocalFile())
                    continue;
                append_path_if_supported(paths, url.toLocalFile());
            }
        }

        // Some external apps expose file paths as plain text rather than text/uri-list.
        if (paths.isEmpty() && mime->hasText()) {
            const QStringList lines = mime->text().split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                QString candidate = line.trimmed();
                if (candidate.startsWith(QStringLiteral("file:///"))) {
                    const QUrl url(candidate);
                    if (url.isLocalFile())
                        candidate = url.toLocalFile();
                }
                append_path_if_supported(paths, candidate);
            }
        }

        paths.removeDuplicates();
        return paths;
    }

    void set_drag_import_active(bool active)
    {
        if (drag_import_active_ == active)
            return;
        drag_import_active_ = active;
        setProperty("obsGspTitleImportDropActive", active);
        if (viewport())
            viewport()->setProperty("obsGspTitleImportDropActive", active);
        if (active) {
            if (saved_style_sheet_.isNull())
                saved_style_sheet_ = styleSheet();
            setStyleSheet(saved_style_sheet_ + QStringLiteral("\nQListWidget { border: 1px solid palette(highlight); }") );
        } else if (!saved_style_sheet_.isNull()) {
            setStyleSheet(saved_style_sheet_);
        }
    }

    bool drag_import_active_ = false;
    QString saved_style_sheet_;
};

class LiveTextCueHeader : public QHeaderView {
public:
    explicit LiveTextCueHeader(QWidget *parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent)
    {
        setSectionsMovable(true);
        setSectionsClickable(true);
        setSectionResizeMode(QHeaderView::Interactive);
    }

    void set_select_all_checked(bool checked)
    {
        if (select_all_checked_ == checked) return;
        select_all_checked_ = checked;
        viewport()->update();
    }

    void set_select_all_visible(bool visible)
    {
        if (select_all_visible_ == visible) return;
        select_all_visible_ = visible;
        viewport()->update();
    }

    std::function<void(bool)> select_all_toggled;

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override
    {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        if (logicalIndex != kLiveSelectColumn || !select_all_visible_) return;

        QStyleOptionButton option;
        option.state = QStyle::State_Enabled | (select_all_checked_ ? QStyle::State_On : QStyle::State_Off);
        option.rect = checkbox_rect(rect);
        style()->drawControl(QStyle::CE_CheckBox, &option, painter, this);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (select_all_visible_ && event && event->button() == Qt::LeftButton &&
            logicalIndexAt(event->pos()) == kLiveSelectColumn) {
            const QRect section_rect(sectionViewportPosition(kLiveSelectColumn), 0, sectionSize(kLiveSelectColumn), height());
            if (checkbox_rect(section_rect).contains(event->pos())) {
                select_all_checked_ = !select_all_checked_;
                viewport()->update();
                if (select_all_toggled)
                    select_all_toggled(select_all_checked_);
                return;
            }
        }

        QHeaderView::mousePressEvent(event);
    }

private:
    QRect checkbox_rect(const QRect &section_rect) const
    {
        const int indicator_width = style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, this);
        const int indicator_height = style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, this);
        return QRect(section_rect.x() + (section_rect.width() - indicator_width) / 2,
                     section_rect.y() + (section_rect.height() - indicator_height) / 2,
                     indicator_width,
                     indicator_height);
    }

    bool select_all_checked_ = false;
    bool select_all_visible_ = true;
};

static LiveTextCueHeader *live_text_cue_header(QTableWidget *table)
{
    return table ? dynamic_cast<LiveTextCueHeader *>(table->horizontalHeader()) : nullptr;
}

class TemplateListWidget : public QListWidget {
public:
    explicit TemplateListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        setDragEnabled(true);
        setAcceptDrops(false);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::MoveAction);
    }

protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
    {
        auto *mime = new QMimeData();
        QList<QUrl> urls;
        for (const auto *item : items) {
            if (!item) continue;
            const QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                urls << QUrl::fromLocalFile(path);
        }
        mime->setUrls(urls);
        return mime;
    }

    void startDrag(Qt::DropActions supported_actions) override
    {
        (void)supported_actions;
        const auto items = selectedItems();
        if (items.empty())
            return;

        std::unique_ptr<QMimeData> mime(mimeData(items));
        if (!mime || !mime->hasUrls())
            return;

        auto *drag = new QDrag(this);
        drag->setMimeData(mime.release());
        if (auto *item = currentItem()) {
            QPixmap pixmap = item->icon().pixmap(iconSize());
            if (!pixmap.isNull())
                drag->setPixmap(pixmap);
        }
        drag->exec(Qt::MoveAction);
    }
};

class TemplateCategoryTree : public QTreeWidget {
public:
    explicit TemplateCategoryTree(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::DropOnly);
        setDefaultDropAction(Qt::MoveAction);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }

    std::function<void()> templates_moved;
    std::function<void(QTreeWidgetItem *)> category_delete_requested;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (accept_template_drag(event))
            return;
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (accept_template_drag(event))
            return;
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event || !event->mimeData() || !event->mimeData()->hasUrls()) {
            QTreeWidget::dropEvent(event);
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto *target = itemAt(event->position().toPoint());
#else
        auto *target = itemAt(event->pos());
#endif
        if (!target)
            target = currentItem();
        if (!target) return;

        const QString target_dir = target->data(0, Qt::UserRole).toString();
        if (target_dir.isEmpty()) return;
        QDir().mkpath(target_dir);

        bool moved_any = false;
        for (const QUrl &url : event->mimeData()->urls()) {
            const QString source_path = url.toLocalFile();
            if (source_path.isEmpty()) continue;
            QFileInfo source_info(source_path);
            if (!source_info.exists() || !source_info.isFile()) continue;
            if (QDir::cleanPath(source_info.absolutePath()) == QDir::cleanPath(target_dir)) continue;

            QString dest_path = QDir(target_dir).filePath(source_info.fileName());
            if (QFileInfo::exists(dest_path)) {
                const QString base = source_info.completeBaseName();
                const QString suffix = source_info.suffix().isEmpty() ? QString() : QStringLiteral(".") + source_info.suffix();
                int copy = 2;
                do {
                    dest_path = QDir(target_dir).filePath(QStringLiteral("%1 %2%3").arg(base).arg(copy++).arg(suffix));
                } while (QFileInfo::exists(dest_path));
            }

            if (move_template_file(source_path, dest_path))
                moved_any = true;
        }

        if (moved_any && templates_moved)
            templates_moved();
        accept_template_drag(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (auto *item = delete_item_at(event ? event->pos() : QPoint())) {
            setCurrentItem(item);
            if (category_delete_requested)
                category_delete_requested(item);
            return;
        }
        QTreeWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (delete_item_at(event ? event->pos() : QPoint()))
            return;
        QTreeWidget::mouseDoubleClickEvent(event);
    }

private:
    QTreeWidgetItem *delete_item_at(const QPoint &pos) const
    {
        auto *item = itemAt(pos);
        if (!item)
            return nullptr;

        const QRect rect = visualRect(indexFromItem(item, 0));
        const int delete_width = fontMetrics().horizontalAdvance(QStringLiteral("×")) + 12;
        return QRect(rect.left(), rect.top(), delete_width, rect.height()).contains(pos) ? item : nullptr;
    }

    template <typename DragEvent>
    bool accept_template_drag(DragEvent *event)
    {
        if (!event || !event->mimeData() || !event->mimeData()->hasUrls())
            return false;

        if (event->possibleActions() & Qt::MoveAction) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->acceptProposedAction();
        }
        return true;
    }

    static bool move_template_file(const QString &source_path, const QString &dest_path)
    {
        if (QFile::rename(source_path, dest_path))
            return true;

        if (!QFile::copy(source_path, dest_path))
            return false;

        if (QFile::remove(source_path))
            return true;

        QFile::remove(dest_path);
        return false;
    }
};

static double title_export_screenshot_time(const Title &title)
{
    const double source_time = title.playback_mode == 2 ? title.pause_time : title.duration * 0.5;
    return std::clamp(source_time, 0.0, std::max(0.0, title.duration));
}

static QImage title_screenshot_image(const Title &title)
{
    return CacheManager::instance().requestFrame(std::make_shared<Title>(title),
                                                 title_export_screenshot_time(title));
}

static QIcon title_cached_screenshot_icon(const Title &title, const QSize &size)
{
    if (title.preview_screenshot_png_base64.empty())
        return QIcon();

    const QByteArray png = QByteArray::fromBase64(
        QByteArray(title.preview_screenshot_png_base64.data(),
                   (int)title.preview_screenshot_png_base64.size()));
    QPixmap pixmap;
    if (!pixmap.loadFromData(png, "PNG"))
        return QIcon();

    return QIcon(pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

static bool title_has_exposed_text(const std::shared_ptr<Title> &title)
{
    return !exposed_text_layers(title).empty();
}

static bool title_has_scene_mask(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    for (const auto &layer : title->layers) {
        if (layer && layer->use_as_scene_mask)
            return true;
    }
    return false;
}

static QString font_awesome_family()
{
    static const QString family = []() {
        const QStringList preferred = {
            QStringLiteral("Font Awesome 6 Free"),
            QStringLiteral("Font Awesome 5 Free"),
            QStringLiteral("FontAwesome")
        };
        const QStringList available = QFontDatabase().families();
        for (const QString &candidate : preferred) {
            if (available.contains(candidate, Qt::CaseInsensitive))
                return candidate;
        }
        return QString();
    }();
    return family;
}

static QString font_awesome_glyph(ushort codepoint, const QString &fallback)
{
    return font_awesome_family().isEmpty() ? fallback : QString(QChar(codepoint));
}

static QIcon title_list_status_icon(bool has_exposed_text, bool has_scene_mask)
{
    const int dpr_size = kTitleListIconExtent;
    QPixmap pixmap(dpr_size, dpr_size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QColor icon_color(205, 205, 205);
    const QRect icon_rect = pixmap.rect();

    QString glyph;
    int glyph_size = 12;
    if (has_exposed_text) {
        glyph = font_awesome_glyph(0xf031, QStringLiteral("T")); // fa-font
        glyph_size = 11;
        if (has_scene_mask) {
            painter.setPen(QPen(QColor(160, 160, 160), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(icon_rect.adjusted(1, 1, -2, -2), 2, 2);
        }
    } else if (has_scene_mask) {
        glyph = font_awesome_glyph(0xf6fa, obsgs_tr("OBSTitles.M")); // fa-mask
    } else {
        glyph = font_awesome_glyph(0xf61f, obsgs_tr("OBSTitles.G")); // fa-shapes
    }

    QFont font;
    const QString fa_family = font_awesome_family();
    if (!fa_family.isEmpty()) {
        font.setFamily(fa_family);
        font.setWeight(QFont::Black);
    } else {
        font.setBold(true);
    }
    font.setPixelSize(glyph_size);
    painter.setFont(font);
    painter.setPen(icon_color);
    painter.drawText(icon_rect, Qt::AlignCenter, glyph);
    painter.end();

    return QIcon(pixmap);
}

static void paint_playlist_badge(QPainter &painter, const QRect &rect,
                                 bool draw_background = true,
                                 const QColor &icon_color = QColor(255, 255, 255))
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (draw_background) {
        painter.setBrush(QColor(39, 186, 103));
        painter.setPen(QPen(QColor(0, 0, 0, 120), 1));
        painter.drawRoundedRect(rect, 4, 4);
    }
    const QPixmap play = obs_icon("play.svg", icon_color).pixmap(QSize(12, 12));
    if (!play.isNull())
        painter.drawPixmap(rect.center() - QPoint(play.width() / 2, play.height() / 2), play);
    painter.restore();
}

static QPixmap title_preview_pixmap(const Title &title, const QSize &size)
{
    QPixmap pixmap;

    if (!title.preview_screenshot_png_base64.empty()) {
        const QByteArray png = QByteArray::fromBase64(
            QByteArray(title.preview_screenshot_png_base64.data(),
                       (int)title.preview_screenshot_png_base64.size()));
        pixmap.loadFromData(png, "PNG");
    }

    if (pixmap.isNull()) {
        const QImage screenshot = title_screenshot_image(title);
        if (!screenshot.isNull())
            pixmap = QPixmap::fromImage(screenshot);
    }

    if (pixmap.isNull())
        return QPixmap();

    return pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

static void paint_transparency_grid(QPainter &painter, const QRect &rect, int cell = 8)
{
    const QColor light(196, 196, 196);
    const QColor dark(144, 144, 144);
    for (int y = rect.top(); y <= rect.bottom(); y += cell) {
        for (int x = rect.left(); x <= rect.right(); x += cell) {
            const bool alternate = (((x - rect.left()) / cell) + ((y - rect.top()) / cell)) % 2;
            painter.fillRect(QRect(x, y, cell, cell).intersected(rect), alternate ? dark : light);
        }
    }
}

static QString elide_to_two_lines(const QString &text, const QFontMetrics &fm, int width)
{
    const QString simplified = text.simplified();
    if (simplified.isEmpty())
        return simplified;

    const QString first_try = fm.elidedText(simplified, Qt::ElideRight, width);
    if (first_try == simplified)
        return simplified;

    QString first;
    QString rest = simplified;
    const QStringList words = simplified.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() > 1) {
        for (const QString &word : words) {
            const QString candidate = first.isEmpty() ? word : first + QLatin1Char(' ') + word;
            if (fm.horizontalAdvance(candidate) > width)
                break;
            first = candidate;
        }
        if (!first.isEmpty())
            rest = simplified.mid(first.length()).trimmed();
    }

    if (first.isEmpty()) {
        for (int i = 1; i <= simplified.size(); ++i) {
            const QString candidate = simplified.left(i);
            if (fm.horizontalAdvance(candidate) > width)
                break;
            first = candidate;
        }
        rest = simplified.mid(first.length()).trimmed();
    }

    if (rest.isEmpty())
        return first;

    return first + QLatin1Char(' ') + fm.elidedText(rest, Qt::ElideRight, width);
}

static QIcon title_icon_view_icon(const Title &title, bool has_exposed_text, bool has_scene_mask,
                                  const LiveCueAggregateCacheStatus &cache_status,
                                  bool playlist_active)
{
    const QSize thumb_size(kTitleIconViewThumbWidth, kTitleIconViewThumbHeight);
    QPixmap composed(thumb_size);
    composed.fill(Qt::transparent);

    QPainter painter(&composed);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect full_rect(QPoint(0, 0), thumb_size);
    paint_transparency_grid(painter, full_rect, 8);

    const QPixmap preview = title_preview_pixmap(title, thumb_size);
    if (!preview.isNull()) {
        const QPoint preview_pos((thumb_size.width() - preview.width()) / 2,
                                 (thumb_size.height() - preview.height()) / 2);
        painter.drawPixmap(preview_pos, preview);
    }

    painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(full_rect.adjusted(0, 0, -1, -1));

    const QPixmap type_pixmap = title_list_status_icon(has_exposed_text, has_scene_mask).pixmap(QSize(18, 18));
    QRect badge_rect(5, 5, 24, 24);
    if (!type_pixmap.isNull())
        painter.drawPixmap(badge_rect.center() - QPoint(type_pixmap.width() / 2, type_pixmap.height() / 2), type_pixmap);

    const QPixmap cache_pixmap = obs_icon(live_cue_cache_icon_name(cache_status.state, cache_status.progress_percent),
                                          live_cue_cache_color(cache_status.state)).pixmap(QSize(18, 18));
    QRect cache_badge_rect(thumb_size.width() - 29, 5, 24, 24);
    if (!cache_pixmap.isNull())
        painter.drawPixmap(cache_badge_rect.center() - QPoint(cache_pixmap.width() / 2, cache_pixmap.height() / 2), cache_pixmap);
    if (playlist_active)
        paint_playlist_badge(painter, QRect(thumb_size.width() - 29, thumb_size.height() - 29, 24, 24));
    painter.end();

    return QIcon(composed);
}

class TitleIconViewDelegate : public QStyledItemDelegate {
public:
    explicit TitleIconViewDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto *list_view = qobject_cast<const QListView *>(option.widget);
        if (list_view && list_view->viewMode() == QListView::ListMode)
            return QStyledItemDelegate::sizeHint(option, index);
        return QSize(kTitleIconViewItemWidth, kTitleIconViewItemHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!painter)
            return;

        const auto *list_view = qobject_cast<const QListView *>(option.widget);
        if (list_view && list_view->viewMode() == QListView::ListMode) {
            QStyledItemDelegate::paint(painter, option, index);
            if (index.data(kTitlePlaylistActiveRole).toBool()) {
                const QRect badge_rect(option.rect.right() - 28,
                                       option.rect.center().y() - 10,
                                       20, 20);
                const bool selected = option.state.testFlag(QStyle::State_Selected);
                const QColor icon_color = selected
                    ? option.palette.color(QPalette::HighlightedText)
                    : QColor(39, 186, 103);
                paint_playlist_badge(*painter, badge_rect, false, icon_color);
            }
            return;
        }

        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();

        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

        const QRect item_rect = option.rect.adjusted(6, 6, -6, -6);
        const int thumb_x = item_rect.x() + (item_rect.width() - kTitleIconViewThumbWidth) / 2;
        const QRect thumb_rect(thumb_x, item_rect.y(), kTitleIconViewThumbWidth, kTitleIconViewThumbHeight);
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const QPixmap pixmap = icon.pixmap(thumb_rect.size());
        if (!pixmap.isNull())
            painter->drawPixmap(thumb_rect.topLeft(), pixmap);

        QRect text_rect(item_rect.x(), thumb_rect.bottom() + 6, item_rect.width(),
                        option.fontMetrics.height() * kTitleIconViewTextLines + 4);
        painter->setFont(option.font);
        painter->setPen(option.palette.color((option.state & QStyle::State_Selected)
                                                 ? QPalette::HighlightedText
                                                 : QPalette::Text));
        const QString display = elide_to_two_lines(index.data(Qt::DisplayRole).toString(), option.fontMetrics, text_rect.width());
        painter->drawText(text_rect, Qt::AlignHCenter | Qt::AlignTop, display);
    }
};

struct DeleteTitleConfirmationResult {
    QMessageBox::StandardButton button = QMessageBox::No;
    bool delete_sources = false;
};

static DeleteTitleConfirmationResult confirm_delete_single_title(QWidget *parent, const Title &title)
{
    QMessageBox box(QMessageBox::Question,
                    obsgs_tr("OBSTitles.DeleteTitle"),
                    obsgs_tr("OBSTitles.DeleteTitleQuestionFormat").arg(QString::fromStdString(title.name)),
                    QMessageBox::Yes | QMessageBox::No,
                    parent);

    const QPixmap preview = title_preview_pixmap(title, QSize(240, 135));
    if (!preview.isNull())
        box.setIconPixmap(preview);

    auto *delete_sources_checkbox = new QCheckBox(obsgs_tr("OBSTitles.DeleteSourcesContainingTitle"), &box);
    delete_sources_checkbox->setToolTip(obsgs_tr("OBSTitles.DeleteSourcesContainingTitleTooltip"));
    delete_sources_checkbox->setChecked(false);
    box.setCheckBox(delete_sources_checkbox);

    box.setDefaultButton(QMessageBox::No);

    DeleteTitleConfirmationResult result;
    result.button = static_cast<QMessageBox::StandardButton>(box.exec());
    result.delete_sources = delete_sources_checkbox->isChecked();
    return result;
}

static DeleteTitleConfirmationResult confirm_delete_multiple_titles(QWidget *parent, int count)
{
    QMessageBox box(QMessageBox::Question,
                    obsgs_tr("OBSTitles.DeleteTitle"),
                    obsgs_tr("OBSTitles.DeleteSelectedTitlesQuestionFormat").arg(count),
                    QMessageBox::Yes | QMessageBox::No,
                    parent);

    auto *delete_sources_checkbox = new QCheckBox(obsgs_tr("OBSTitles.DeleteSourcesContainingTitles"), &box);
    delete_sources_checkbox->setToolTip(obsgs_tr("OBSTitles.DeleteSourcesContainingTitleTooltip"));
    delete_sources_checkbox->setChecked(false);
    box.setCheckBox(delete_sources_checkbox);
    box.setDefaultButton(QMessageBox::No);

    DeleteTitleConfirmationResult result;
    result.button = static_cast<QMessageBox::StandardButton>(box.exec());
    result.delete_sources = delete_sources_checkbox->isChecked();
    return result;
}

static int delete_title_sources_for_ids(const std::vector<std::string> &title_ids)
{
    if (title_ids.empty())
        return 0;

    QSet<QString> ids;
    for (const auto &id : title_ids) {
        const QString qid = QString::fromStdString(id).trimmed();
        if (!qid.isEmpty())
            ids.insert(qid);
    }
    if (ids.isEmpty())
        return 0;

    struct SourceDeleteLookup {
        QSet<QString> ids;
        QStringList source_names;
    } lookup{ids, {}};

    obs_enum_sources([](void *param, obs_source_t *source) {
        auto *lookup = static_cast<SourceDeleteLookup *>(param);
        if (!lookup || !source)
            return true;

        const char *source_id = obs_source_get_id(source);
        if (!source_id || strcmp(source_id, kTitleSourceId) != 0)
            return true;

        obs_data_t *settings = obs_source_get_settings(source);
        const char *title_id = settings ? obs_data_get_string(settings, PROP_TITLE_ID) : nullptr;
        const QString qid = QString::fromUtf8(title_id ? title_id : "").trimmed();
        if (settings)
            obs_data_release(settings);

        if (!qid.isEmpty() && lookup->ids.contains(qid)) {
            const char *source_name = obs_source_get_name(source);
            if (source_name && *source_name)
                lookup->source_names << QString::fromUtf8(source_name);
        }

        return true;
    }, &lookup);

    int removed_count = 0;
    for (const QString &source_name : lookup.source_names) {
        obs_source_t *source = obs_get_source_by_name(source_name.toUtf8().constData());
        if (!source)
            continue;
        obs_source_remove(source);
        obs_source_release(source);
        ++removed_count;
    }

    return removed_count;
}

static QString title_screenshot_png_base64(const QImage &screenshot)
{
    if (screenshot.isNull())
        return QString();

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!screenshot.save(&buffer, "PNG"))
        return QString();
    return QString::fromLatin1(png.toBase64());
}

static void populate_metadata_from_title(const Title &title, TitleTemplateExportMetadata &metadata)
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
}

static bool prompt_template_metadata(QWidget *parent, const Title &title,
                                     const QImage &screenshot,
                                     TitleTemplateExportMetadata &metadata,
                                     bool *save_in_template_library = nullptr,
                                     const QString &window_title = obsgs_tr("OBSTitles.ExportTemplateDetails"))
{
    populate_metadata_from_title(title, metadata);

    QDialog dialog(parent);
    dialog.setWindowTitle(window_title);
    dialog.setModal(true);
    dialog.resize(560, 500);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(&dialog));

    auto *preview_label = new QLabel(obsgs_tr("OBSTitles.TemplateScreenshotPreviewLabel"), &dialog);
    set_bold_label(preview_label);
    layout->addWidget(preview_label);

    auto *preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(160);
    if (!screenshot.isNull()) {
        preview->setPixmap(QPixmap::fromImage(screenshot).scaled(
            QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else if (!metadata.screenshot_png_base64.empty()) {
        QPixmap pixmap;
        const QByteArray png = QByteArray::fromBase64(QByteArray::fromStdString(metadata.screenshot_png_base64));
        if (pixmap.loadFromData(png, "PNG"))
            preview->setPixmap(pixmap.scaled(QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    } else {
        preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    }
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
    auto validate_and_accept = [&dialog, title_edit, save_in_template_library](bool save_to_library) {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.ExportTemplateDetails"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        if (save_in_template_library)
            *save_in_template_library = save_to_library;
        dialog.accept();
    };
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     [&validate_and_accept]() { validate_and_accept(false); });
    if (save_in_template_library) {
        auto *library_button = buttons->addButton(obsgs_tr("OBSTitles.SaveInTemplateLibrary"), QDialogButtonBox::ActionRole);
        QObject::connect(library_button, &QPushButton::clicked, &dialog,
                         [&validate_and_accept]() { validate_and_accept(true); });
    }
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

struct TemplateLibraryEntry {
    int id;
    const char *name_key;
    const char *description_key;
    const char *default_name_key;
    const char *category_folder;
    const char *file_name;
};

struct TemplateFileMetadata {
    QString title;
    QString description;
    QString creator;
    QString creation_date;
    QIcon screenshot_icon;
    QPixmap screenshot_pixmap;
};

static const std::array<const char *, 5> template_library_category_folders{{
    "Lower Thirds",
    "Tickers",
    "Clocks",
    "Centered",
    "Full Screen",
}};

static const std::array<TemplateLibraryEntry, 5> template_library_entries{{
    {1, "OBSTitles.TemplateLowerThird", "OBSTitles.TemplateLowerThirdDescription", "OBSTitles.TemplateSpeakerName",
     "Lower Thirds", "lower-third.ogspt"},
    {3, "OBSTitles.TemplateTickerStrap", "OBSTitles.TemplateTickerStrapDescription", "OBSTitles.TemplateBreakingNews",
     "Tickers", "ticker-strap.ogspt"},
    {4, "OBSTitles.TemplateClock", "OBSTitles.TemplateClockDescription", "OBSTitles.TemplateClockTitle",
     "Clocks", "clock.ogspt"},
    {2, "OBSTitles.TemplateCenteredTitle", "OBSTitles.TemplateCenteredTitleDescription", "OBSTitles.TemplateProgramTitle",
     "Centered", "centered-title.ogspt"},
    {5, "OBSTitles.TemplateFullScreen", "OBSTitles.TemplateFullScreenDescription", "OBSTitles.TemplateFullScreenTitle",
     "Full Screen", "full-screen.ogspt"},
}};

static const TemplateLibraryEntry *template_library_entry_by_id(int id)
{
    for (const auto &entry : template_library_entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

static QString template_library_root_path()
{
    char *path = obs_module_config_path("template-library");
    QString root = path ? QString::fromUtf8(path) : QDir::homePath() + QStringLiteral("/OBS Graphics Studio Pro Templates");
    bfree(path);
    QDir().mkpath(root);
    return root;
}

static QString template_file_filter()
{
    return QStringLiteral("*.ogspt *.otpt *.json");
}

static bool is_title_template_file_info(const QFileInfo &info)
{
    if (!info.exists() || !info.isFile() || !info.isReadable())
        return false;

    const QString suffix = info.suffix().toLower();
    return suffix == QStringLiteral("ogspt") ||
           suffix == QStringLiteral("otpt") ||
           suffix == QStringLiteral("json");
}

static void append_title_template_path(QStringList &paths, const QString &raw_path)
{
    QString candidate = raw_path.trimmed();
    if (candidate.isEmpty())
        return;

    if (candidate.startsWith(QStringLiteral("file:"))) {
        const QUrl url(candidate);
        if (url.isLocalFile())
            candidate = url.toLocalFile();
    }

    candidate.remove(QChar('"'));
    const QFileInfo info(candidate);
    if (is_title_template_file_info(info))
        paths.push_back(info.absoluteFilePath());
}

static QStringList title_template_paths_from_mime(const QMimeData *mime)
{
    QStringList paths;
    if (!mime)
        return paths;

    if (mime->hasUrls()) {
        const auto urls = mime->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile())
                append_title_template_path(paths, url.toLocalFile());
        }
    }

    // Fallback for sources that expose newline-separated paths or file:// URLs as text.
    if (mime->hasText()) {
        const QStringList lines = mime->text().split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString &line : lines)
            append_title_template_path(paths, line);
    }

    paths.removeDuplicates();
    return paths;
}

static bool mime_has_title_templates(const QMimeData *mime)
{
    return !title_template_paths_from_mime(mime).isEmpty();
}

static TemplateFileMetadata read_template_file_metadata(const QString &path)
{
    TemplateFileMetadata metadata;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        metadata.title = QFileInfo(path).completeBaseName();
        return metadata;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = doc.object();
    const QJsonObject meta = root.value(QStringLiteral("metadata")).toObject();
    metadata.title = meta.value(QStringLiteral("title")).toString(
        root.value(QStringLiteral("template_title")).toString(QFileInfo(path).completeBaseName()));
    metadata.description = meta.value(QStringLiteral("description")).toString(root.value(QStringLiteral("description")).toString());
    metadata.creator = meta.value(QStringLiteral("creator")).toString(root.value(QStringLiteral("creator")).toString());
    metadata.creation_date = meta.value(QStringLiteral("creation_date")).toString(root.value(QStringLiteral("creation_date")).toString());

    QJsonObject screenshot = meta.value(QStringLiteral("screenshot")).toObject();
    if (screenshot.isEmpty())
        screenshot = root.value(QStringLiteral("screenshot")).toObject();
    const QByteArray png = QByteArray::fromBase64(screenshot.value(QStringLiteral("data_base64")).toString().toLatin1());
    QPixmap pixmap;
    if (!png.isEmpty() && pixmap.loadFromData(png, "PNG")) {
        metadata.screenshot_pixmap = pixmap;
        metadata.screenshot_icon = QIcon(pixmap.scaled(QSize(96, 54), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    return metadata;
}

static bool write_template_file_metadata(const QString &path, const TemplateFileMetadata &metadata, QString *error = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditReadFailed");
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditReadFailed");
        return false;
    }

    QJsonObject root = doc.object();
    QJsonObject meta = root.value(QStringLiteral("metadata")).toObject();
    meta[QStringLiteral("title")] = metadata.title;
    meta[QStringLiteral("description")] = metadata.description;
    meta[QStringLiteral("creator")] = metadata.creator;
    meta[QStringLiteral("creation_date")] = metadata.creation_date;
    if (!root.value(QStringLiteral("screenshot")).isUndefined())
        meta[QStringLiteral("screenshot")] = root.value(QStringLiteral("screenshot"));

    root[QStringLiteral("template_title")] = metadata.title;
    root[QStringLiteral("description")] = metadata.description;
    root[QStringLiteral("creator")] = metadata.creator;
    root[QStringLiteral("creation_date")] = metadata.creation_date;
    root[QStringLiteral("metadata")] = meta;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditWriteFailed");
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.flush()) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditWriteFailed");
        return false;
    }
    return true;
}

static bool prompt_edit_template_file_metadata(QWidget *parent, const QString &path)
{
    TemplateFileMetadata metadata = read_template_file_metadata(path);

    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.EditTemplateMetadata"));
    dialog.setModal(true);
    dialog.resize(520, 360);

    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *title_edit = new QLineEdit(metadata.title, &dialog);
    auto *description_edit = new QTextEdit(&dialog);
    description_edit->setAcceptRichText(false);
    description_edit->setPlainText(metadata.description);
    description_edit->setMinimumHeight(110);
    auto *creator_edit = new QLineEdit(metadata.creator, &dialog);
    auto *date_edit = new QLineEdit(metadata.creation_date, &dialog);

    form->addRow(obsgs_tr("OBSTitles.TemplateExportTitleLabel"), title_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportDescriptionLabel"), description_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportCreatorLabel"), creator_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateCreationDateLabel"), date_edit);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.EditTemplateMetadata"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    metadata.title = title_edit->text().trimmed();
    metadata.description = description_edit->toPlainText().trimmed();
    metadata.creator = creator_edit->text().trimmed();
    metadata.creation_date = date_edit->text().trimmed();

    QString error;
    if (!write_template_file_metadata(path, metadata, &error)) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.EditTemplateMetadata"), error);
        return false;
    }
    return true;
}

static QTreeWidgetItem *add_template_category_item(QTreeWidget *tree, QTreeWidgetItem *parent,
                                                   const QFileInfo &dir_info)
{
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, QStringLiteral("×  ") + dir_info.fileName());
    item->setToolTip(0, dir_info.absoluteFilePath());
    item->setData(0, Qt::UserRole, dir_info.absoluteFilePath());
    item->setData(0, TemplateCategoryNameRole, dir_info.fileName());
    item->setFlags(item->flags() | Qt::ItemIsDropEnabled);

    QDir dir(dir_info.absoluteFilePath());
    const auto children = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &child : children)
        add_template_category_item(tree, item, child);
    return item;
}

static void populate_template_categories(QTreeWidget *tree, const QString &root_path)
{
    tree->clear();
    tree->setColumnCount(1);
    tree->setHeaderHidden(true);
    for (const char *folder : template_library_category_folders)
        QDir(root_path).mkpath(QString::fromUtf8(folder));

    QDir root(root_path);
    std::vector<QString> known;
    known.reserve(template_library_category_folders.size());
    for (const char *folder : template_library_category_folders) {
        const QString name = QString::fromUtf8(folder);
        known.push_back(name);
        add_template_category_item(tree, nullptr, QFileInfo(root.filePath(name)));
    }
    for (const QFileInfo &dir : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (std::find(known.begin(), known.end(), dir.fileName()) == known.end())
            add_template_category_item(tree, nullptr, dir);
    }

    tree->expandAll();
    if (tree->topLevelItemCount() > 0)
        tree->setCurrentItem(tree->topLevelItem(0));
}

static void collect_template_category_paths(const QDir &root, const QString &relative_dir, QStringList &categories)
{
    const QString scan_path = relative_dir.isEmpty() ? root.absolutePath() : root.filePath(relative_dir);
    QDir dir(scan_path);
    const auto children = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &child : children) {
        const QString relative_child = relative_dir.isEmpty()
            ? child.fileName()
            : relative_dir + QStringLiteral("/") + child.fileName();
        categories << relative_child;
        collect_template_category_paths(root, relative_child, categories);
    }
}

static QStringList template_library_category_paths(const QString &root_path)
{
    for (const char *folder : template_library_category_folders)
        QDir(root_path).mkpath(QString::fromUtf8(folder));

    QStringList categories;
    QDir root(root_path);
    collect_template_category_paths(root, QString(), categories);
    categories.removeDuplicates();
    categories.sort(Qt::CaseInsensitive);
    return categories;
}

static QString sanitized_template_category_path(QString category)
{
    category.replace('\\', '/');
    QStringList safe_parts;
    for (QString part : category.split('/', Qt::SkipEmptyParts)) {
        part = part.trimmed();
        part.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
        if (!part.isEmpty() && part != QStringLiteral(".") && part != QStringLiteral(".."))
            safe_parts << part;
    }
    return safe_parts.join(QStringLiteral("/"));
}

static bool rename_template_category(QWidget *parent, QTreeWidgetItem *item)
{
    if (!item) return false;

    const QString category_path = item->data(0, Qt::UserRole).toString();
    if (category_path.isEmpty()) return false;

    QFileInfo category_info(category_path);
    bool ok = false;
    const QString current_name = item->data(0, TemplateCategoryNameRole).toString().isEmpty()
        ? category_info.fileName()
        : item->data(0, TemplateCategoryNameRole).toString();
    QString name = QInputDialog::getText(
        parent, obsgs_tr("OBSTitles.RenameCategory"), obsgs_tr("OBSTitles.CategoryNamePrompt"),
        QLineEdit::Normal, current_name, &ok);
    if (!ok) return false;

    name = sanitized_template_category_path(name);
    if (name.isEmpty() || name.contains('/')) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.RenameCategory"),
                             obsgs_tr("OBSTitles.TemplateLibraryCategoryRequired"));
        return false;
    }

    const QString parent_path = category_info.dir().absolutePath();
    const QString new_path = QDir(parent_path).filePath(name);
    if (QDir::cleanPath(new_path) == QDir::cleanPath(category_path))
        return false;

    if (QFileInfo::exists(new_path) || !QDir(parent_path).rename(category_info.fileName(), name)) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.RenameCategory"),
                             obsgs_tr("OBSTitles.RenameCategoryFailed"));
        return false;
    }

    return true;
}

static bool delete_template_category(QWidget *parent, QTreeWidgetItem *item, const QString &root_path)
{
    if (!item) return false;

    const QString category_path = item->data(0, Qt::UserRole).toString();
    if (category_path.isEmpty()) return false;
    if (QDir::cleanPath(category_path) == QDir::cleanPath(root_path)) return false;

    const auto reply = QMessageBox::question(
        parent, obsgs_tr("OBSTitles.DeleteCategory"),
        obsgs_tr("OBSTitles.DeleteCategoryQuestionFormat").arg(item->data(0, TemplateCategoryNameRole).toString()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return false;

    if (!QDir(category_path).removeRecursively()) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.DeleteCategory"),
                             obsgs_tr("OBSTitles.DeleteCategoryFailed"));
        return false;
    }

    return true;
}

static bool prompt_template_library_category(QWidget *parent, QString &category)
{
    const QString root_path = template_library_root_path();
    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(&dialog));

    auto *prompt = new QLabel(obsgs_tr("OBSTitles.TemplateLibraryCategoryPrompt"), &dialog);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    auto *combo = new QComboBox(&dialog);
    combo->setEditable(true);
    combo->addItems(template_library_category_paths(root_path));
    layout->addWidget(combo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString safe_category = sanitized_template_category_path(combo->currentText());
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

} // namespace

/* ══════════════════════════════════════════════════════════════════
 *  Constructor
 * ══════════════════════════════════════════════════════════════════ */
TitleDock::TitleDock(QWidget *parent)
    : QDockWidget(obsgs_tr("OBSTitles.DockName"), parent)
{
    setFeatures(QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    build_ui();

    // Accept external template file drops anywhere over the dock/template list.
    // QListWidget uses a viewport child for DnD, so installing the filter on
    // every child avoids lost drops when the cursor is over labels, toolbars,
    // empty list areas, or the viewport.
    setAcceptDrops(true);
    installEventFilter(this);
    const auto dock_drop_widgets = findChildren<QWidget *>();
    for (QWidget *w : dock_drop_widgets) {
        if (!w)
            continue;
        w->setAcceptDrops(true);
        w->installEventFilter(this);
    }

    /* React to external data changes.  Always marshal back to the dock's
     * Qt thread so background/source playback changes cannot touch widgets.
     */
    change_callback_id_ = TitleDataStore::instance().on_change([this]() {
        QTimer::singleShot(0, this, [this]() {
            if (!updating_exposed_text_)
                refresh();
        });
    });

    install_obs_state_callbacks();
    populate_list();
    seen_store_revision_ = TitleDataStore::instance().revision();
    live_refresh_timer_ = new QTimer(this);
    live_refresh_timer_->setInterval(100);
    connect(live_refresh_timer_, &QTimer::timeout, this, [this]() {
        uint64_t revision = TitleDataStore::instance().revision();
        if (revision == seen_store_revision_ || updating_exposed_text_) return;
        seen_store_revision_ = revision;
        sync_playlist_runtime_state();

        bool playlist_active = false;
        for (const auto &candidate : TitleDataStore::instance().titles()) {
            if (candidate && candidate->playlist_active) {
                playlist_active = true;
                break;
            }
        }
        if (playlist_active) {
            /* Playlist cue changes are runtime-only revisions. Rebuilding the
             * complete table here destroys every cell editor and steals focus on
             * each step. Update only cue/cache decoration while any playlist runs. */
            auto selected_title = TitleDataStore::instance().get_title(selected_id());
            if (selected_title)
                update_live_text_runtime_status(selected_title);
            update_playlist_countdown_label();
            return;
        }
        populate_exposed_text();
    });
    live_refresh_timer_->start();
    connect(&CacheManager::instance(), &CacheManager::liveCueStateChanged, this,
            [this](const QString &title_id, int row) {
                if (title_id.isEmpty() && row < 0) {
                    QTimer::singleShot(0, this, [this]() {
                        populate_list();
                        populate_exposed_text();
                    });
                    return;
                }
                if (title_id != QString::fromStdString(selected_id()) || updating_exposed_text_) {
                    schedule_title_list_cache_icon_update(title_id);
                    return;
                }
                auto title = TitleDataStore::instance().get_title(selected_id());
                if (!title || !text_table_ || row < 0 || row >= text_table_->rowCount()) {
                    populate_exposed_text();
                    return;
                }
                update_live_text_cache_cell(title, row);
                schedule_title_list_cache_icon_update(title_id);
                if (cache_waiting_title_id_ == title_id && cache_waiting_cue_row_ == row &&
                    CacheManager::instance().prepareLiveCueForPlayback(title, row)) {
                    OGS_LOG_INFO("LiveCueUI", QStringLiteral("Armed cue row prerender complete; cueing title=%1 row=%2")
                                                .arg(title_id).arg(row));
                    cache_waiting_cue_row_ = -1;
                    cache_waiting_title_id_.clear();
                    cue_live_text_row_for_title(title, row, false, true);
                }
            });
    connect(&CacheManager::instance(), &CacheManager::cacheEnabledChanged, this,
            [this](bool) {
                if (!updating_exposed_text_) {
                    populate_list();
                    populate_exposed_text();
                }
            });
}


bool TitleDock::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);

    if (!event)
        return QObject::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (mime_has_title_templates(drag->mimeData())) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (mime_has_title_templates(drag->mimeData())) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        break;
    }
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        const QStringList paths = title_template_paths_from_mime(drop->mimeData());
        if (!paths.isEmpty()) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            import_title_paths(paths);
            return true;
        }
        break;
    }
    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

void TitleDock::update_live_text_cache_cell(const std::shared_ptr<Title> &title, int row)
{
    if (!title || !text_table_ || row < 0 || row >= text_table_->rowCount())
        return;

    update_live_text_select_cell_status(row);

    const FrameCacheState state = CacheManager::instance().liveCueState(title, row);
    auto *button = live_cue_cache_button(text_table_, row);
    if (!button)
        return;

    const int progress_percent = CacheManager::instance().liveCueProgressPercent(title, row);
    const QColor color = live_cue_cache_color(state);
    const char *icon_name = live_cue_cache_icon_name(state, progress_percent);
    const QString visual_key = QStringLiteral("%1:%2")
                                   .arg(QString::fromUtf8(icon_name))
                                   .arg(color.rgba());
    if (button->property("ogsCacheVisualKey").toString() != visual_key) {
        OGS_LOG_DEBUG("LiveCueUI", QStringLiteral("Update cache cell title=%1 row=%2 state=%3 progress=%4 icon=%5")
                                         .arg(QString::fromStdString(title->id))
                                         .arg(row)
                                         .arg((int)state)
                                         .arg(progress_percent)
                                         .arg(QString::fromUtf8(icon_name)));
        button->setIcon(obs_icon(icon_name, color));
        button->setProperty("ogsCacheVisualKey", visual_key);
    }
    const QString tooltip = QStringLiteral("%1\n%2")
                                .arg(state == FrameCacheState::Queued || state == FrameCacheState::Rendering
                                         ? QStringLiteral("%1 (%2%)").arg(live_cue_cache_text(state)).arg(progress_percent)
                                         : live_cue_cache_text(state),
                                     live_cue_cache_tooltip(state));
    if (button->toolTip() != tooltip)
        button->setToolTip(tooltip);
    button->setEnabled(true);
    button->setCursor(state == FrameCacheState::Disabled ? Qt::ArrowCursor : Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;padding:0;}"
        "QToolButton:hover{background:transparent;border:none;}"));
    button->disconnect();
    connect(button, &QToolButton::clicked, this, [this, row]() {
        auto current_title = TitleDataStore::instance().get_title(selected_id());
        if (!current_title || row < 0 || row >= (int)current_title->live_text_rows.size())
            return;
        if (!CacheManager::instance().cacheEnabled())
            return;
        CacheManager::instance().cacheLiveCueNow(current_title, row);
        update_live_text_cache_cell(current_title, row);
    });
}

void TitleDock::update_live_text_cache_cells()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_)
        return;

    for (int row = 0; row < text_table_->rowCount(); ++row) {
        update_live_text_cache_cell(title, row);
        update_live_text_select_cell_status(row);
    }
}


void TitleDock::update_live_text_runtime_status_fast(const std::shared_ptr<Title> &title, int primary_row, int previous_row)
{
    if (!title || !text_table_)
        return;

    const QString title_id = QString::fromStdString(title->id);
    const int cue_col = kLiveCueColumn;

    auto update_row = [this, title, title_id, cue_col](int row) {
        if (row < 0 || !text_table_ || row >= text_table_->rowCount())
            return;
        update_live_text_select_cell_status(row);
        update_live_text_cache_cell(title, row);
        if (auto *cue = live_cue_button(text_table_, row, cue_col)) {
            const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
                cache_waiting_cue_row_ == row;
            cue->setIcon(obs_icon("cue.svg", live_cue_state_color(row == title->current_cue_row,
                                                                   row == title->pending_cue_row || waiting_for_prerender)));
        }
    };

    update_row(primary_row);
    update_row(previous_row);
    if (title->pending_cue_row >= 0 && title->pending_cue_row != primary_row && title->pending_cue_row != previous_row)
        update_row(title->pending_cue_row);

    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_))
        cue_table->viewport()->update();
    text_table_->viewport()->update();

    // The aggregate cache badge can be comparatively expensive on titles with many cue rows
    // because it queries every cue.  Defer it so row/program feedback stays immediate.
    schedule_title_list_cache_icon_update(title_id);
}


void TitleDock::update_live_text_runtime_status(const std::shared_ptr<Title> &title)
{
    if (!title || !text_table_)
        return;

    const QString title_id = QString::fromStdString(title->id);
    const int cue_col = kLiveCueColumn;

    for (int row = 0; row < text_table_->rowCount(); ++row) {
        update_live_text_select_cell_status(row);
        update_live_text_cache_cell(title, row);

        auto *cue = live_cue_button(text_table_, row, cue_col);
        if (!cue)
            continue;
        const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
            cache_waiting_cue_row_ == row;
        cue->setIcon(obs_icon("cue.svg", live_cue_state_color(row == title->current_cue_row,
                                                               row == title->pending_cue_row || waiting_for_prerender)));
    }

    schedule_title_list_cache_icon_update(title_id);

    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_))
        cue_table->viewport()->update();
    text_table_->viewport()->update();
}

void TitleDock::schedule_title_list_cache_icon_update(const QString &title_id)
{
    if (!list_ || title_id.isEmpty())
        return;

    pending_title_cache_icon_updates_.insert(title_id);
    if (title_cache_icon_update_scheduled_)
        return;

    title_cache_icon_update_scheduled_ = true;
    QTimer::singleShot(25, this, [this]() { flush_title_list_cache_icon_updates(); });
}

void TitleDock::flush_title_list_cache_icon_updates()
{
    title_cache_icon_update_scheduled_ = false;
    const QSet<QString> pending = pending_title_cache_icon_updates_;
    pending_title_cache_icon_updates_.clear();
    for (const QString &title_id : pending)
        update_title_list_cache_icon(title_id);
}

void TitleDock::update_title_list_cache_icon(const QString &title_id)
{
    if (!list_ || title_id.isEmpty())
        return;

    auto title = TitleDataStore::instance().get_title(title_id.toStdString());
    if (!title)
        return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    const bool has_exposed_text = !exposed.empty();
    const bool has_scene_mask = title_has_scene_mask(title);
    const LiveCueAggregateCacheStatus cache_status = aggregate_live_cue_cache_status(title);
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (!item || item->data(Qt::UserRole).toString() != title_id)
            continue;
        if (template_icon_view_) {
            item->setIcon(title_icon_view_icon(*title, has_exposed_text, has_scene_mask, cache_status, title->playlist_active));
            item->setSizeHint(QSize(kTitleIconViewItemWidth, kTitleIconViewItemHeight));
        } else {
            item->setIcon(title_list_combined_status_icon(has_exposed_text, has_scene_mask, cache_status));
        }
        item->setData(kTitlePlaylistActiveRole, title->playlist_active);
        item->setToolTip(QStringLiteral("%1\n%2")
                             .arg(obsgs_tr("OBSTitles.LayerCountTooltipFormat").arg(title->layers.size()).arg(title->duration),
                                  aggregate_live_cue_cache_tooltip(cache_status)));
        break;
    }
}

void TitleDock::update_live_text_select_cell_status(int row)
{
    if (!text_table_ || row < 0 || row >= text_table_->rowCount())
        return;

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title)
        return;

    const QString title_id = QString::fromStdString(title->id);
    const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
        cache_waiting_cue_row_ == row;
    const bool current = row == title->current_cue_row;
    const bool queued = row == title->pending_cue_row || waiting_for_prerender;
    const QColor status_color = live_cue_select_cell_color(current, queued);
    const QString background_style = status_color.isValid()
        ? QStringLiteral("background-color: rgba(%1, %2, %3, %4);")
              .arg(status_color.red()).arg(status_color.green()).arg(status_color.blue()).arg(status_color.alpha())
        : QString();
    const QString editor_style = status_color.isValid()
        ? QStringLiteral("QPlainTextEdit{padding:3px;%1}").arg(background_style)
        : QStringLiteral("QPlainTextEdit{padding:3px;}");

    for (int col = 0; col < text_table_->columnCount(); ++col) {
        if (auto *cell_item = text_table_->item(row, col)) {
            if (status_color.isValid())
                cell_item->setBackground(QBrush(status_color));
            else
                cell_item->setBackground(QBrush());
        }

        auto *widget = text_table_->cellWidget(row, col);
        if (!widget)
            continue;
        widget->setStyleSheet(background_style);
        if (auto *field = dynamic_cast<LiveTextCueField *>(widget))
            field->setStyleSheet(editor_style);
        else if (auto *image = dynamic_cast<LiveImageCueField *>(widget))
            image->setStyleSheet(background_style);
    }

    if (text_table_->viewport())
        text_table_->viewport()->update();
}

void TitleDock::adjust_live_text_table_columns(bool fill_to_viewport)
{
    if (!text_table_ || text_table_->columnCount() <= 0)
        return;

    auto *header = text_table_->horizontalHeader();
    if (!header)
        return;

    const int last_col = text_table_->columnCount() - 1;
    const int cache_col = text_table_->columnCount() > 2 ? kLiveCacheColumn : -1;
    const int first_text_col = text_table_->columnCount() > 3 ? kLiveFirstValueColumn : -1;
    const int last_text_col = text_table_->columnCount() > 3 ? last_col : -1;

    QSignalBlocker header_block(header);
    auto resize_to_contents = [&](int col) {
        if (col >= 0 && col < text_table_->columnCount()) {
            header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
            text_table_->resizeColumnToContents(col);
        }
    };

    if (text_table_->columnCount() <= 3) {
        resize_to_contents(kStaticCueColumn);
        resize_to_contents(kStaticCacheColumn);
        header->setSectionResizeMode(kStaticTitleColumn, QHeaderView::Interactive);
        const int viewport_width = text_table_->viewport() ? text_table_->viewport()->width() : text_table_->width();
        const int remaining = viewport_width - header->sectionSize(kStaticCueColumn) - header->sectionSize(kStaticCacheColumn);
        if (remaining > 0)
            header->resizeSection(kStaticTitleColumn, remaining);
        return;
    }

    resize_to_contents(kLiveCueColumn);
    resize_to_contents(kLiveSelectColumn);
    resize_to_contents(cache_col);

    if (first_text_col >= 0) {
        for (int col = first_text_col; col <= last_text_col; ++col)
            header->setSectionResizeMode(col, QHeaderView::Interactive);
    }

    int total_width = 0;
    for (int col = 0; col < text_table_->columnCount(); ++col)
        total_width += header->sectionSize(col);

    const int viewport_width = text_table_->viewport() ? text_table_->viewport()->width() : text_table_->width();
    if (fill_to_viewport && first_text_col >= 0 && viewport_width > 0 && total_width < viewport_width) {
        const int missing = viewport_width - total_width;
        const int text_cols = std::max(1, last_text_col - first_text_col + 1);
        const int per_col = missing / text_cols;
        int remainder = missing % text_cols;
        for (int col = first_text_col; col <= last_text_col; ++col) {
            const int add = per_col + (remainder-- > 0 ? 1 : 0);
            header->resizeSection(col, header->sectionSize(col) + add);
        }
    }
}

TitleDock::~TitleDock()
{
    remove_obs_state_callbacks();

    TitleDataStore::instance().remove_change_callback(change_callback_id_);
    change_callback_id_ = 0;

    stop_playlist();
    if (live_refresh_timer_)
        live_refresh_timer_->stop();

    if (editor_) {
        TitleEditor *editor = editor_;
        editor_ = nullptr;
        disconnect(editor, nullptr, this, nullptr);
        delete editor;
    }
}


void TitleDock::load_dock_settings()
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));

    template_icon_view_ = settings.value(QString::fromUtf8(kTemplateIconViewKey), template_icon_view_).toBool();
    visibility_filter_active_ = settings.value(QString::fromUtf8(kVisibilityFilterKey), visibility_filter_active_).toBool();
    last_selected_title_id_ = settings.value(QString::fromUtf8(kLastSelectedTitleIdKey)).toString();
    playlist_loop_ = settings.value(QString::fromUtf8(kPlaylistLoopKey), playlist_loop_).toBool();
    playlist_reverse_ = settings.value(QString::fromUtf8(kPlaylistReverseKey), playlist_reverse_).toBool();
    playlist_hold_seconds_ = std::clamp(settings.value(QString::fromUtf8(kPlaylistHoldSecondsKey),
                                                       playlist_hold_seconds_).toDouble(),
                                        0.0, 3600.0);
    background_persistence_ = settings.value(QString::fromUtf8(kBackgroundPersistenceKey),
                                             background_persistence_).toBool();
    text_persistence_ = settings.value(QString::fromUtf8(kTextPersistenceKey),
                                       text_persistence_).toBool();
    live_text_lines_per_row_ = std::clamp(settings.value(QString::fromUtf8(kLiveTextLinesPerRowKey),
                                                         live_text_lines_per_row_).toInt(),
                                          kMinLiveTextLinesPerRow,
                                          kMaxLiveTextLinesPerRow);

    const QByteArray splitter_state = settings.value(QString::fromUtf8(kDockSplitterStateKey)).toByteArray();
    if (!splitter_state.isEmpty() && sections_)
        sections_->restoreState(splitter_state);

    settings.endGroup();
}

void TitleDock::save_dock_settings() const
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));

    if (sections_)
        settings.setValue(QString::fromUtf8(kDockSplitterStateKey), sections_->saveState());
    settings.setValue(QString::fromUtf8(kTemplateIconViewKey), template_icon_view_);
    settings.setValue(QString::fromUtf8(kVisibilityFilterKey), visibility_filter_active_);
    settings.setValue(QString::fromUtf8(kLastSelectedTitleIdKey), last_selected_title_id_);
    settings.setValue(QString::fromUtf8(kPlaylistLoopKey), playlist_loop_);
    settings.setValue(QString::fromUtf8(kPlaylistReverseKey), playlist_reverse_);
    settings.setValue(QString::fromUtf8(kPlaylistHoldSecondsKey), playlist_hold_seconds_);
    settings.setValue(QString::fromUtf8(kBackgroundPersistenceKey), background_persistence_);
    settings.setValue(QString::fromUtf8(kTextPersistenceKey), text_persistence_);
    settings.setValue(QString::fromUtf8(kLiveTextLinesPerRowKey), live_text_lines_per_row_);

    settings.endGroup();
}

void TitleDock::update_scene_collection_title()
{
    if (template_lbl_)
        template_lbl_->setText(current_scene_collection_titles_label());
}

void TitleDock::install_obs_state_callbacks()
{
    if (obs_state_callbacks_installed_) return;

    signal_handler_t *handler = obs_get_signal_handler();
    if (!handler) return;

    signal_handler_connect(handler, "source_activate", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_deactivate", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_show", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_hide", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_create", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_destroy", &TitleDock::on_obs_source_state_signal, this);
    obs_frontend_add_event_callback(&TitleDock::on_obs_frontend_event, this);
    obs_state_callbacks_installed_ = true;
}

void TitleDock::remove_obs_state_callbacks()
{
    if (!obs_state_callbacks_installed_) return;

    signal_handler_t *handler = obs_get_signal_handler();
    if (handler) {
        signal_handler_disconnect(handler, "source_activate", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_deactivate", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_show", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_hide", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_create", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_destroy", &TitleDock::on_obs_source_state_signal, this);
    }
    obs_frontend_remove_event_callback(&TitleDock::on_obs_frontend_event, this);
    obs_state_callbacks_installed_ = false;
}

void TitleDock::refresh_for_obs_source_state_change()
{
    if (!visibility_filter_active_) return;
    populate_list();
}

void TitleDock::on_obs_source_state_signal(void *priv, calldata_t *)
{
    auto *dock = static_cast<TitleDock *>(priv);
    if (!dock) return;

    QTimer::singleShot(0, dock, [dock]() { dock->refresh_for_obs_source_state_change(); });
}

void TitleDock::on_obs_frontend_event(obs_frontend_event event, void *priv)
{
    auto *dock = static_cast<TitleDock *>(priv);
    if (!dock) return;

    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        QTimer::singleShot(0, dock, [dock]() { dock->refresh_for_obs_source_state_change(); });
        break;
    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  UI construction
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::build_ui()
{
    container_ = new QWidget(this);
    auto *root = new QVBoxLayout(container_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    sections_ = new QSplitter(Qt::Vertical, container_);
    sections_->setChildrenCollapsible(false);
    root->addWidget(sections_, 1);

    auto *template_section = new QWidget(sections_);
    auto *template_layout = new QVBoxLayout(template_section);
    template_layout->setContentsMargins(0, 0, 0, 0);
    template_layout->setSpacing(obs_layout_spacing(template_section));

    /* ── header toolbar ── */
    auto *template_toolbar = make_obs_dock_toolbar(template_section);

    btn_add_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Add"), obs_icon("add.svg"),
                                         obsgs_tr("OBSTitles.AddTooltip"));
    btn_dup_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Duplicate"), obs_icon("duplicate.svg"),
                                         obsgs_tr("OBSTitles.Duplicate"));
    btn_del_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Delete"), obs_icon("delete.svg"),
                                         obsgs_tr("OBSTitles.Delete"));
    btn_rename_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.EditDetails"), obs_icon("rename.svg"),
                                            obsgs_tr("OBSTitles.EditDetailsTooltip"));
    btn_export_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Export"), obs_icon("export.svg"),
                                            obsgs_tr("OBSTitles.ExportTooltip"));
    btn_edit_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Edit"), obs_icon("edit.svg"),
                                          obsgs_tr("OBSTitles.EditTooltip"));
    btn_scene_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.AddToScene"), obs_icon("add-to-scene.svg"),
                                           obsgs_tr("OBSTitles.AddToSceneTooltip"));
    btn_view_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.IconView"), obs_icon("icon-view.svg"),
                                          obsgs_tr("OBSTitles.IconViewTooltip"));
    btn_visibility_filter_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.VisibleSourceFilter"),
                                                       obs_icon("layer-visible.svg"),
                                                       obsgs_tr("OBSTitles.VisibleSourceFilterTooltip"));
    btn_visibility_filter_->setCheckable(true);
    btn_visibility_filter_->setStyleSheet(dock_highlight_button_style());

    template_toolbar->addWidget(btn_add_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_dup_);
    template_toolbar->addWidget(btn_del_);
    template_toolbar->addWidget(toolbar_spacer(template_toolbar));
    template_toolbar->addWidget(btn_view_);
    template_toolbar->addWidget(btn_visibility_filter_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_rename_);
    template_toolbar->addWidget(btn_export_);
    template_toolbar->addWidget(btn_edit_);
    template_toolbar->addWidget(btn_scene_);

    /* ── template/title section ── */
    auto *template_header = new QHBoxLayout();
    template_header->setContentsMargins(0, 0, 0, 0);
    template_header->setSpacing(0);

    template_lbl_ = new QLabel(current_scene_collection_titles_label(), template_section);
    set_bold_label(template_lbl_);
    template_header->addWidget(template_lbl_);
    template_header->addStretch();
    template_layout->addLayout(template_header);
    template_layout->addWidget(template_toolbar);

    auto *title_list = new TitleListWidget(template_section);
    title_list->delete_requested = [this]() { on_delete(); };
    title_list->files_dropped = [this](const QStringList &paths) { import_title_paths(paths); };
    list_ = title_list;
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setMinimumHeight(120);
    list_->setItemDelegate(new TitleIconViewDelegate(list_));
    template_layout->addWidget(list_, 1);

    auto *live_section = new QWidget(sections_);
    auto *live_layout = new QVBoxLayout(live_section);
    live_layout->setContentsMargins(0, 0, 0, 0);
    live_layout->setSpacing(obs_layout_spacing(live_section));

    auto *live_header = new QHBoxLayout();
    live_header->setContentsMargins(0, 0, 0, 0);
    live_header->setSpacing(0);

    /* ── exposed text section ── */
    text_editor_lbl_ = new QLabel(obsgs_tr("OBSTitles.LiveText"), live_section);
    set_bold_label(text_editor_lbl_);

    auto *live_toolbar = make_obs_dock_toolbar(live_section);
    btn_add_text_row_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.AddRow"), obs_icon("add.svg"),
                                                  obsgs_tr("OBSTitles.AddCueRowTooltip"));
    btn_delete_text_row_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.Delete"), obs_icon("delete.svg"),
                                                     obsgs_tr("OBSTitles.DeleteCueTooltip"));
    btn_row_up_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.MoveUp"), obs_icon("move-up.svg"),
                                            obsgs_tr("OBSTitles.MoveCueUpTooltip"));
    btn_row_down_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.MoveDown"), obs_icon("move-down.svg"),
                                              obsgs_tr("OBSTitles.MoveCueDownTooltip"));
    btn_data_sources_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.DataSources"),
                                                  obs_icon("import.svg"),
                                                  obsgs_tr("OBSTitles.DataSourcesTooltip"));
    auto *data_sources_menu = new QMenu(btn_data_sources_);
    data_sources_menu->addAction(obs_icon("import.svg"), obsgs_tr("OBSTitles.ImportData"),
                                 this, &TitleDock::on_import_live_text_data);
    data_sources_menu->addAction(obs_icon("import.svg"), obsgs_tr("OBSTitles.ImportAppendData"),
                                 this, &TitleDock::on_import_append_live_text_data);
    data_sources_menu->addAction(obs_icon("export.svg"), obsgs_tr("OBSTitles.ExportData"),
                                 this, &TitleDock::on_export_live_text_data);
    btn_data_sources_->setMenu(data_sources_menu);
    btn_data_sources_->setPopupMode(QToolButton::InstantPopup);
    btn_data_sources_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));
    btn_live_text_settings_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.Settings"),
                                                         obs_icon("settings.svg"),
                                                         obsgs_tr("OBSTitles.LiveTextSettingsTooltip"));
    btn_live_text_settings_->setText(obsgs_tr("OBSTitles.Settings"));
    btn_live_text_settings_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn_external_refresh_ = make_obs_dock_long_press_tool_button(live_toolbar, obsgs_tr("OBSTitles.RefreshExternalData"),
                                                                 globe_status_icon(false),
                                                                 obsgs_tr("OBSTitles.RefreshExternalDataTooltip"));
    btn_external_refresh_->setCheckable(true);
    btn_external_refresh_->setMinimumWidth(obs_toolbar_icon_extent(live_toolbar) + 10);
    btn_external_refresh_->setStyleSheet(dock_highlight_button_style(true));
    btn_playlist_ = make_obs_dock_long_press_tool_button(live_toolbar, obsgs_tr("OBSTitles.Playlist"), obs_icon("play.svg"),
                                                         obsgs_tr("OBSTitles.PlaylistTooltip"));
    btn_playlist_->setCheckable(true);
    btn_playlist_->setStyleSheet(dock_highlight_button_style(true));
    playlist_countdown_lbl_ = new QLabel(QStringLiteral("--"), live_toolbar);
    playlist_countdown_lbl_->setToolTip(obsgs_tr("OBSTitles.PlaylistNextCueTooltip"));
    playlist_countdown_lbl_->setMinimumWidth(44);
    playlist_countdown_lbl_->setAlignment(Qt::AlignCenter);
    btn_persistence_settings_ = make_obs_dock_long_press_tool_button(live_toolbar, obsgs_tr("OBSTitles.Persistence"),
                                                                     obs_icon("persistence.svg"),
                                                                     obsgs_tr("OBSTitles.PersistenceTooltip"));
    btn_persistence_settings_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn_persistence_settings_->setMinimumWidth(obs_toolbar_icon_extent(live_toolbar) + 10);
    btn_persistence_settings_->setCheckable(true);
    btn_persistence_settings_->setStyleSheet(dock_highlight_button_style(true, true));
    live_toolbar->addWidget(btn_add_text_row_);
    live_toolbar->addWidget(btn_delete_text_row_);
    live_toolbar->addWidget(btn_data_sources_);
    live_toolbar->addWidget(btn_row_up_);
    live_toolbar->addWidget(btn_row_down_);
    live_toolbar->addSeparator();
    live_toolbar->addWidget(btn_playlist_);
    live_toolbar->addWidget(playlist_countdown_lbl_);
    live_toolbar->addWidget(btn_persistence_settings_);
    live_toolbar->addWidget(toolbar_spacer(live_toolbar));
    live_toolbar->addWidget(btn_live_text_settings_);
    live_toolbar->addWidget(btn_external_refresh_);

    live_header->addWidget(text_editor_lbl_);
    live_header->addStretch();
    live_layout->addLayout(live_header);

    text_table_ = new LiveTextCueTable(live_section);
    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_))
        cue_table->layout_requested = [this]() { adjust_live_text_table_columns(); };
    auto *live_text_header = new LiveTextCueHeader(text_table_);
    live_text_header->select_all_toggled = [this](bool checked) { set_all_live_text_rows_checked(checked); };
    text_table_->setHorizontalHeader(live_text_header);
    text_table_->setMinimumHeight(96);
    text_table_->setAlternatingRowColors(false);
    text_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->verticalHeader()->setDefaultSectionSize(30);
    text_table_->horizontalHeader()->setStretchLastSection(false);
    text_table_->horizontalHeader()->setSectionsMovable(true);
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    text_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    text_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    live_layout->addWidget(text_table_, 1);
    live_layout->addWidget(live_toolbar);

    sections_->addWidget(template_section);
    sections_->addWidget(live_section);
    sections_->setStretchFactor(0, 2);
    sections_->setStretchFactor(1, 1);

    /* ── status ── */
    status_lbl_ = new QLabel(obsgs_tr("OBSTitles.NoTitleSelected"), container_);
    status_lbl_->setAlignment(Qt::AlignCenter);
    QFont sf = status_lbl_->font();
    sf.setPointSize(std::max(1, sf.pointSize() - 1));
    status_lbl_->setFont(sf);
    template_layout->addWidget(status_lbl_);
    template_layout->addWidget(template_toolbar);
    update_template_view_mode();

    setWidget(container_);

    /* ── connections ── */
    auto *add_menu = new QMenu(btn_add_);
    add_menu->addAction(obsgs_tr("OBSTitles.AddBlankTitle"), this, &TitleDock::on_add);
    add_menu->addAction(obsgs_tr("OBSTitles.AddFromTemplatesLibrary"), this, &TitleDock::on_add_from_templates_library);
    add_menu->addAction(obsgs_tr("OBSTitles.Import"), this, &TitleDock::on_import);
    btn_add_->setMenu(add_menu);
    btn_add_->setPopupMode(QToolButton::InstantPopup);
    btn_add_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    connect(btn_dup_,   &QToolButton::clicked, this, &TitleDock::on_duplicate);
    connect(btn_rename_, &QToolButton::clicked, this, &TitleDock::on_rename);
    connect(btn_del_,   &QToolButton::clicked, this, &TitleDock::on_delete);
    connect(btn_export_, &QToolButton::clicked, this, &TitleDock::on_export);
    connect(btn_edit_,  &QToolButton::clicked, this, &TitleDock::on_edit);
    connect(btn_scene_, &QToolButton::clicked, this, &TitleDock::on_add_to_scene);
    connect(btn_view_, &QToolButton::clicked, this, &TitleDock::on_toggle_template_view);
    connect(btn_visibility_filter_, &QToolButton::toggled, this, &TitleDock::on_toggle_visibility_filter);
    connect(btn_add_text_row_, &QToolButton::clicked, this, &TitleDock::on_add_live_text_row);
    connect(btn_delete_text_row_, &QToolButton::clicked, this, &TitleDock::on_delete_live_text_rows);
    connect(btn_row_up_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_up);
    auto *persistence_menu = new QMenu(btn_persistence_settings_);
    act_background_persistence_ = persistence_menu->addAction(obsgs_tr("OBSTitles.BackgroundPersistence"));
    act_background_persistence_->setCheckable(true);
    act_text_persistence_ = persistence_menu->addAction(obsgs_tr("OBSTitles.TextPersistence"));
    act_text_persistence_->setCheckable(true);
    btn_persistence_settings_->setMenu(persistence_menu);

    auto *live_text_settings_menu = new QMenu(btn_live_text_settings_);
    auto *lines_widget = new QWidget(live_text_settings_menu);
    auto *lines_layout = new QHBoxLayout(lines_widget);
    lines_layout->setContentsMargins(8, 4, 8, 4);
    lines_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.LiveTextLinesPerRow"), lines_widget));
    spin_live_text_lines_per_row_ = new QSpinBox(lines_widget);
    spin_live_text_lines_per_row_->setRange(kMinLiveTextLinesPerRow, kMaxLiveTextLinesPerRow);
    spin_live_text_lines_per_row_->setValue(live_text_lines_per_row_);
    lines_layout->addWidget(spin_live_text_lines_per_row_);
    auto *lines_action = new QWidgetAction(live_text_settings_menu);
    lines_action->setDefaultWidget(lines_widget);
    live_text_settings_menu->addAction(lines_action);
    auto *external_data_menu = new QMenu(btn_external_refresh_);
    act_external_data_source_ = external_data_menu->addAction(obsgs_tr("OBSTitles.EnableExternalDataSource"),
                                                              this, &TitleDock::on_toggle_external_data_source);
    act_external_data_settings_ = external_data_menu->addAction(obsgs_tr("OBSTitles.ExternalDataSourceSettings"),
                                                                this, &TitleDock::on_show_external_data_settings);
    btn_external_refresh_->setMenu(external_data_menu);
    auto *playlist_menu = new QMenu(btn_playlist_);
    act_playlist_loop_ = playlist_menu->addAction(obsgs_tr("OBSTitles.PlaylistLoop"));
    act_playlist_loop_->setCheckable(true);
    act_playlist_reverse_ = playlist_menu->addAction(obsgs_tr("OBSTitles.PlaylistReverseOrder"));
    act_playlist_reverse_->setCheckable(true);
    act_playlist_restart_on_active_ = playlist_menu->addAction(obsgs_tr("OBSTitles.PlaylistRestartOnSourceActive"));
    act_playlist_restart_on_active_->setCheckable(true);
    act_playlist_stop_on_inactive_ = playlist_menu->addAction(obsgs_tr("OBSTitles.PlaylistStopOnSourceInactive"));
    act_playlist_stop_on_inactive_->setCheckable(true);
    playlist_menu->addSeparator();
    auto *hold_widget = new QWidget(playlist_menu);
    auto *hold_layout = new QHBoxLayout(hold_widget);
    hold_layout->setContentsMargins(8, 4, 8, 4);
    hold_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.PlaylistHoldSeconds"), hold_widget));
    auto *hold_spin = new TimecodeSpinBox(hold_widget);
    hold_spin->setRange(0.0, 3600.0);
    hold_spin->setValue(playlist_hold_seconds_);
    hold_layout->addWidget(hold_spin);
    auto *hold_action = new QWidgetAction(playlist_menu);
    hold_action->setDefaultWidget(hold_widget);
    act_playlist_hold_ = hold_action;
    playlist_menu->addAction(hold_action);
    btn_playlist_->setMenu(playlist_menu);
    btn_live_text_settings_->setMenu(live_text_settings_menu);
    btn_live_text_settings_->setPopupMode(QToolButton::InstantPopup);
    btn_live_text_settings_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    connect(btn_row_down_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_down);
    connect(btn_external_refresh_, &QToolButton::clicked, this, &TitleDock::on_refresh_external_data);
    connect(btn_playlist_, &QToolButton::toggled, this, &TitleDock::on_toggle_playlist);
    connect(btn_persistence_settings_, &QToolButton::clicked, this, [this]() {
        const auto title = TitleDataStore::instance().get_title(selected_id());
        if (!title || exposed_text_layers(title).empty()) {
            update_persistence_controls();
            return;
        }
        const bool enable_full_persistence = !(background_persistence_ || text_persistence_);
        background_persistence_ = enable_full_persistence;
        text_persistence_ = enable_full_persistence;
        if (act_background_persistence_) {
            QSignalBlocker block(act_background_persistence_);
            act_background_persistence_->setChecked(enable_full_persistence);
        }
        if (act_text_persistence_) {
            QSignalBlocker block(act_text_persistence_);
            act_text_persistence_->setChecked(enable_full_persistence);
        }
        save_dock_settings();
        apply_persistence_settings_to_title(title);
        CacheManager::instance().refreshLiveCueStructure(title);
        update_persistence_controls();
        populate_exposed_text();
    });
    connect(act_playlist_loop_, &QAction::toggled, this, [this](bool checked) {
        playlist_loop_ = checked;
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            title->playlist_loop = checked;
            TitleDataStore::instance().save_async();
        }
        save_dock_settings();
    });
    connect(act_playlist_reverse_, &QAction::toggled, this, [this](bool checked) {
        playlist_reverse_ = checked;
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            title->playlist_reverse = checked;
            TitleDataStore::instance().save_async();
        }
        save_dock_settings();
    });
    connect(act_playlist_restart_on_active_, &QAction::toggled, this, [this](bool checked) {
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            title->playlist_restart_on_source_active = checked;
            TitleDataStore::instance().save_async();
        }
    });
    connect(act_playlist_stop_on_inactive_, &QAction::toggled, this, [this](bool checked) {
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            title->playlist_stop_on_source_inactive = checked;
            TitleDataStore::instance().save_async();
        }
    });
    connect(hold_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        playlist_hold_seconds_ = value;
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            title->playlist_hold_seconds = value;
            TitleDataStore::instance().save_async();
        }
        save_dock_settings();
    });
    connect(spin_live_text_lines_per_row_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &TitleDock::on_live_text_lines_per_row_changed);
    connect(act_background_persistence_, &QAction::toggled, this, [this](bool checked) {
        background_persistence_ = checked;
        if (!background_persistence_) {
            text_persistence_ = false;
            if (act_text_persistence_) {
                QSignalBlocker block(act_text_persistence_);
                act_text_persistence_->setChecked(false);
            }
        }
        save_dock_settings();
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            apply_persistence_settings_to_title(title);
            CacheManager::instance().refreshLiveCueStructure(title);
        }
        update_persistence_controls();
        populate_exposed_text();
    });
    connect(act_text_persistence_, &QAction::toggled, this, [this](bool checked) {
        text_persistence_ = background_persistence_ && checked;
        if (act_text_persistence_ && act_text_persistence_->isChecked() != text_persistence_) {
            QSignalBlocker block(act_text_persistence_);
            act_text_persistence_->setChecked(text_persistence_);
        }
        save_dock_settings();
        if (auto title = TitleDataStore::instance().get_title(selected_id())) {
            apply_persistence_settings_to_title(title);
            CacheManager::instance().refreshLiveCueStructure(title);
        }
        update_persistence_controls();
        populate_exposed_text();
    });
    connect(text_table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item && item->column() == 0)
            update_live_text_select_all_state();
    });
    connect(sections_, &QSplitter::splitterMoved, this, [this](int, int) { save_dock_settings(); });
    connect(text_table_->horizontalHeader(), &QHeaderView::sectionMoved,
            this, [this](int, int, int) { save_live_text_header_state(); });
    connect(text_table_->horizontalHeader(), &QHeaderView::sectionResized,
            this, [this](int, int, int) { save_live_text_header_state(); });
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &TitleDock::on_selection_changed);
    connect(list_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { on_edit(); });

    playlist_timer_ = new QTimer(this);
    playlist_timer_->setInterval(250);
    connect(playlist_timer_, &QTimer::timeout, this, &TitleDock::on_playlist_tick);

    load_dock_settings();
    if (btn_visibility_filter_) btn_visibility_filter_->setChecked(visibility_filter_active_);
    if (act_playlist_loop_) act_playlist_loop_->setChecked(playlist_loop_);
    if (act_playlist_reverse_) act_playlist_reverse_->setChecked(playlist_reverse_);
    if (act_background_persistence_) act_background_persistence_->setChecked(background_persistence_);
    if (act_text_persistence_) act_text_persistence_->setChecked(text_persistence_);
    hold_spin->setValue(playlist_hold_seconds_);
    if (spin_live_text_lines_per_row_) {
        QSignalBlocker block(spin_live_text_lines_per_row_);
        spin_live_text_lines_per_row_->setValue(live_text_lines_per_row_);
    }
    update_template_view_mode();
    update_playlist_controls();
    update_persistence_controls();
    update_playlist_countdown_label();
    on_selection_changed();
}

/* ══════════════════════════════════════════════════════════════════
 *  List population
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::update_template_view_mode()
{
    if (!list_) return;

    if (template_icon_view_) {
        list_->setViewMode(QListView::IconMode);
        list_->setIconSize(QSize(kTitleIconViewThumbWidth, kTitleIconViewThumbHeight));
        list_->setGridSize(QSize(kTitleIconViewItemWidth, kTitleIconViewItemHeight));
        list_->setResizeMode(QListView::Adjust);
        list_->setMovement(QListView::Static);
        list_->setSpacing(6);
        list_->setUniformItemSizes(true);
        list_->setWordWrap(true);
        list_->setTextElideMode(Qt::ElideRight);
        if (btn_view_) {
            btn_view_->setText(obsgs_tr("OBSTitles.ListView"));
            btn_view_->setAccessibleName(obsgs_tr("OBSTitles.ListView"));
            btn_view_->setToolTip(obsgs_tr("OBSTitles.ListViewTooltip"));
            btn_view_->setIcon(obs_icon("list-view.svg"));
        }
    } else {
        list_->setViewMode(QListView::ListMode);
        list_->setIconSize(QSize(kTitleListIconExtent * 2 + 3, kTitleListIconExtent));
        list_->setGridSize(QSize());
        list_->setResizeMode(QListView::Fixed);
        list_->setMovement(QListView::Static);
        list_->setSpacing(0);
        list_->setUniformItemSizes(true);
        list_->setWordWrap(false);
        list_->setTextElideMode(Qt::ElideRight);
        if (btn_view_) {
            btn_view_->setText(obsgs_tr("OBSTitles.IconView"));
            btn_view_->setAccessibleName(obsgs_tr("OBSTitles.IconView"));
            btn_view_->setToolTip(obsgs_tr("OBSTitles.IconViewTooltip"));
            btn_view_->setIcon(obs_icon("icon-view.svg"));
        }
    }
}

void TitleDock::on_toggle_template_view()
{
    template_icon_view_ = !template_icon_view_;
    update_template_view_mode();
    populate_list();
    save_dock_settings();
}

void TitleDock::on_toggle_visibility_filter(bool enabled)
{
    visibility_filter_active_ = enabled;
    populate_list();
    save_dock_settings();
}

QSet<QString> TitleDock::active_title_source_ids() const
{
    QSet<QString> ids;

    obs_enum_sources([](void *param, obs_source_t *source) {
        auto *result = static_cast<QSet<QString> *>(param);
        if (!result || !source) return true;

        const char *source_id = obs_source_get_id(source);
        if (!source_id || strcmp(source_id, kTitleSourceId) != 0)
            return true;

        if (!obs_source_active(source) && !obs_source_showing(source))
            return true;

        obs_data_t *settings = obs_source_get_settings(source);
        const char *title_id = settings ? obs_data_get_string(settings, PROP_TITLE_ID) : nullptr;
        const QString id = QString::fromUtf8(title_id ? title_id : "").trimmed();
        if (!id.isEmpty())
            result->insert(id);
        if (settings)
            obs_data_release(settings);

        return true;
    }, &ids);

    return ids;
}

bool TitleDock::should_show_title(const std::shared_ptr<Title> &title, const QSet<QString> &active_ids) const
{
    if (!visibility_filter_active_) return true;
    if (!title) return false;
    return active_ids.contains(QString::fromStdString(title->id));
}

void TitleDock::populate_list()
{
    QString prev_id = QString::fromStdString(selected_id());
    if (prev_id.isEmpty())
        prev_id = last_selected_title_id_;
    list_->blockSignals(true);
    list_->clear();

    const QSet<QString> active_ids = visibility_filter_active_ ? active_title_source_ids() : QSet<QString>();

    for (auto &t : TitleDataStore::instance().titles()) {
        if (!should_show_title(t, active_ids))
            continue;
        const QString title_id = QString::fromStdString(t->id);
        auto exposed = exposed_text_layers(t);
        normalize_live_text_rows(t, exposed);
        CacheManager::instance().restoreDiskStates(t);

        auto *item = new QListWidgetItem(QString::fromStdString(t->name));
        const bool has_exposed_text = !exposed.empty();
        const bool has_scene_mask = title_has_scene_mask(t);
        const LiveCueAggregateCacheStatus cache_status = aggregate_live_cue_cache_status(t);
        if (template_icon_view_) {
            item->setIcon(title_icon_view_icon(*t, has_exposed_text, has_scene_mask, cache_status, t->playlist_active));
            item->setSizeHint(QSize(kTitleIconViewItemWidth, kTitleIconViewItemHeight));
            item->setTextAlignment(Qt::AlignHCenter);
        } else {
            item->setIcon(title_list_combined_status_icon(has_exposed_text, has_scene_mask, cache_status));
            item->setSizeHint(QSize());
            item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }
        item->setData(Qt::UserRole, title_id);
        item->setData(kTitlePlaylistActiveRole, t->playlist_active);
        // Layer count hint as tooltip
        item->setToolTip(QStringLiteral("%1\n%2")
            .arg(obsgs_tr("OBSTitles.LayerCountTooltipFormat").arg(t->layers.size()).arg(t->duration),
                 aggregate_live_cue_cache_tooltip(cache_status)));
        list_->addItem(item);
    }

    /* Restore selection, or select the first title when the dock/list loads. */
    bool restored_selection = false;
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == prev_id) {
            list_->setCurrentRow(i);
            restored_selection = true;
            break;
        }
    }
    if (!restored_selection && list_->count() > 0)
        list_->setCurrentRow(0);

    list_->blockSignals(false);
    on_selection_changed();
}

void TitleDock::refresh()
{
    populate_list();
    populate_exposed_text();
}

/* ══════════════════════════════════════════════════════════════════
 *  Selection helper
 * ══════════════════════════════════════════════════════════════════ */
std::string TitleDock::selected_id() const
{
    auto *item = list_->currentItem();
    if (!item) return {};
    return item->data(Qt::UserRole).toString().toStdString();
}

std::vector<std::string> TitleDock::selected_title_ids() const
{
    std::vector<std::string> ids;
    if (!list_) return ids;

    const auto items = list_->selectedItems();
    ids.reserve((size_t)items.size());
    for (const auto *item : items) {
        if (!item) continue;
        const std::string id = item->data(Qt::UserRole).toString().toStdString();
        if (!id.empty())
            ids.push_back(id);
    }

    return ids;
}

void TitleDock::on_selection_changed()
{
    const auto ids = selected_title_ids();
    const int selected_count = (int)ids.size();
    const bool has = selected_count > 0;
    const bool single = selected_count == 1;
    const QString current_title_id = QString::fromStdString(selected_id());
    if (!current_title_id.isEmpty()) {
        last_selected_title_id_ = current_title_id;
        save_dock_settings();
    }

    btn_dup_->setEnabled(single);
    btn_rename_->setEnabled(single);
    btn_del_->setEnabled(has);
    btn_export_->setEnabled(single);
    btn_edit_->setEnabled(single);
    btn_scene_->setEnabled(single);

    if (single) {
        auto t = TitleDataStore::instance().get_title(ids.front());
        if (t)
            status_lbl_->setText(
                obsgs_tr("OBSTitles.StatusLayerCountFormat")
                    .arg(t->layers.size())
                    .arg(t->duration, 0, 'f', 1));
    } else if (has) {
        status_lbl_->setText(obsgs_tr("OBSTitles.SelectedTitlesStatusFormat").arg(selected_count));
    } else {
        status_lbl_->setText(list_->count() == 0
            ? obsgs_tr("OBSTitles.UseAddHint")
            : obsgs_tr("OBSTitles.NoTitleSelected"));
    }
    populate_exposed_text();
}

void TitleDock::save_live_text_header_state()
{
    if (!text_table_ || text_table_->columnCount() <= 0) return;
    const QByteArray state = text_table_->horizontalHeader()->saveState();
    live_text_header_states_[text_table_->columnCount()] = state;

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    const QByteArray encoded = state.toBase64();
    title->live_text_header_state.assign(encoded.constData(), (size_t)encoded.size());
    TitleDataStore::instance().save();
}

bool TitleDock::restore_live_text_header_state()
{
    if (!text_table_ || text_table_->columnCount() <= 0) return false;

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (title && !title->live_text_header_state.empty()) {
        const QByteArray encoded(title->live_text_header_state.data(), (int)title->live_text_header_state.size());
        const QByteArray state = QByteArray::fromBase64(encoded);
        if (!state.isEmpty() && text_table_->horizontalHeader()->restoreState(state)) {
            live_text_header_states_[text_table_->columnCount()] = state;
            return true;
        }
    }

    auto it = live_text_header_states_.find(text_table_->columnCount());
    if (it == live_text_header_states_.end()) return false;
    return text_table_->horizontalHeader()->restoreState(it->second);
}

bool TitleDock::has_checked_live_text_rows() const
{
    if (!text_table_) return false;
    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, kLiveSelectColumn);
        if (item && item->checkState() == Qt::Checked)
            return true;
    }
    return false;
}

void TitleDock::apply_live_text_row_selection(const std::vector<int> &rows, bool checked)
{
    if (!text_table_) return;

    QSignalBlocker block(text_table_);
    text_table_->clearSelection();
    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, kLiveSelectColumn);
        if (item)
            item->setCheckState(Qt::Unchecked);
    }

    auto *selection_model = text_table_->selectionModel();
    for (int row : rows) {
        if (row < 0 || row >= text_table_->rowCount()) continue;
        if (checked) {
            auto *item = text_table_->item(row, kLiveSelectColumn);
            if (item)
                item->setCheckState(Qt::Checked);
        }
        if (selection_model && text_table_->columnCount() > 0) {
            const QModelIndex left = text_table_->model()->index(row, 0);
            const QModelIndex right = text_table_->model()->index(row, text_table_->columnCount() - 1);
            selection_model->select(QItemSelection(left, right),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    if (!rows.empty() && selection_model)
        selection_model->setCurrentIndex(text_table_->model()->index(rows.front(), kLiveSelectColumn), QItemSelectionModel::NoUpdate);
    update_live_text_select_all_state();
}

void TitleDock::set_all_live_text_rows_checked(bool checked)
{
    if (!text_table_) return;

    QSignalBlocker block(text_table_);
    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, kLiveSelectColumn);
        if (item)
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    update_live_text_select_all_state();
}

void TitleDock::update_live_text_select_all_state()
{
    auto *header = live_text_cue_header(text_table_);
    if (!header || !text_table_) return;

    const int row_count = text_table_->rowCount();
    bool all_checked = row_count > 0;
    for (int row = 0; row < row_count; ++row) {
        auto *item = text_table_->item(row, kLiveSelectColumn);
        if (!item || item->checkState() != Qt::Checked) {
            all_checked = false;
            break;
        }
    }
    header->set_select_all_checked(all_checked);
}

std::vector<int> TitleDock::selected_live_text_rows() const
{
    std::vector<int> rows;
    if (!text_table_) return rows;

    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, kLiveSelectColumn);
        if (item && item->checkState() == Qt::Checked)
            rows.push_back(row);
    }

    if (rows.empty()) {
        if (auto *selection = text_table_->selectionModel()) {
            for (const QModelIndex &index : selection->selectedRows()) {
                const int row = index.row();
                if (std::find(rows.begin(), rows.end(), row) == rows.end())
                    rows.push_back(row);
            }
        }
    }

    if (rows.empty()) {
        for (const auto *item : text_table_->selectedItems()) {
            if (!item) continue;
            int row = item->row();
            if (std::find(rows.begin(), rows.end(), row) == rows.end())
                rows.push_back(row);
        }
    }

    std::sort(rows.begin(), rows.end());
    return rows;
}

void TitleDock::commit_live_text_cell_edit(const std::shared_ptr<Title> &title,
                                           int row, int col, const QString &text)
{
    if (!title || !text_table_) return;
    if (row < 0 || row >= (int)title->live_text_rows.size()) return;
    if (col < 0 || col >= (int)title->live_text_rows[row].size()) return;

    auto exposed = exposed_text_layers(title);
    const bool single_value_column = col >= 0 && col < (int)exposed.size() &&
        exposed[col] && exposed[col]->exposed_single_value;
    std::vector<int> target_rows = selected_live_text_rows();
    if (single_value_column) {
        target_rows.clear();
        target_rows.reserve(title->live_text_rows.size());
        for (int i = 0; i < (int)title->live_text_rows.size(); ++i)
            target_rows.push_back(i);
    } else if (target_rows.size() <= 1 ||
        std::find(target_rows.begin(), target_rows.end(), row) == target_rows.end()) {
        target_rows = {row};
    }

    const std::string new_value = text.toStdString();
    bool changed = false;
    for (int target_row : target_rows) {
        if (target_row < 0 || target_row >= (int)title->live_text_rows.size())
            continue;
        auto &live_row = title->live_text_rows[target_row];
        if (col < 0 || col >= (int)live_row.size())
            continue;
        if (live_row[col] == new_value)
            continue;

        live_row[col] = new_value;
        changed = true;
    }
    if (!changed) return;

    updating_exposed_text_ = true;
    for (int target_row : target_rows) {
        if (target_row < 0 || target_row >= text_table_->rowCount())
            continue;
        QWidget *widget = text_table_->cellWidget(target_row, col + kLiveFirstValueColumn);
        if (auto *edit = dynamic_cast<LiveTextCueField *>(widget)) {
            if (edit->toPlainText() == text)
                continue;
            QSignalBlocker block(edit);
            edit->setPlainText(text);
            edit->update_tooltip(text);
        } else if (auto *image = dynamic_cast<LiveImageCueField *>(widget)) {
            if (image->path() == text)
                continue;
            image->set_path(text);
        }
    }

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    /* A row can also be the previous/outgoing side of another row's persistence
     * transition, so refresh the pairwise requirement sets once. Stable keys
     * ensure that only variants affected by the edited text are regenerated. */
    CacheManager::instance().refreshLiveCueStructureAsync(title);
    seen_store_revision_ = TitleDataStore::instance().revision();
    updating_exposed_text_ = false;
}

void TitleDock::set_live_text_row_render_paused(const std::shared_ptr<Title> &title, int row, bool paused)
{
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return;
    ensure_live_text_row_ids(*title);
    const QString row_key = QStringLiteral("%1:%2")
        .arg(QString::fromStdString(title->id),
             QString::fromStdString(live_text_row_id(*title, row)));
    if (row_key.endsWith(QStringLiteral(":")))
        return;

    if (paused) {
        int &count = focused_live_text_row_render_counts_[row_key];
        ++count;
        if (count == 1)
            CacheManager::instance().setLiveCueRowRenderPaused(title, row, true);
        return;
    }

    auto it = focused_live_text_row_render_counts_.find(row_key);
    if (it == focused_live_text_row_render_counts_.end())
        return;
    it.value() = std::max(0, it.value() - 1);
    if (it.value() > 0)
        return;
    focused_live_text_row_render_counts_.erase(it);
    CacheManager::instance().setLiveCueRowRenderPaused(title, row, false);
}

void TitleDock::apply_persistence_settings_to_title(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    const bool has_exposed = !exposed.empty();
    title->cue_background_persistence = background_persistence_ && has_exposed;
    title->cue_text_persistence = title->cue_background_persistence && text_persistence_;
    if (!title->cue_background_persistence)
        title->cue_persistence_transition = false;
    if (!title->cue_text_persistence)
        title->cue_persistent_text_columns.clear();
}

void TitleDock::update_persistence_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool has_exposed = title && !exposed_text_layers(title).empty();
    if (btn_persistence_settings_) {
        btn_persistence_settings_->setEnabled(has_exposed);
        QSignalBlocker block(btn_persistence_settings_);
        const bool background_only = has_exposed && background_persistence_ && !text_persistence_;
        const bool full_persistence = has_exposed && background_persistence_ && text_persistence_;
        btn_persistence_settings_->setProperty("persistenceState",
                                               background_only ? QStringLiteral("background") : QString());
        btn_persistence_settings_->style()->unpolish(btn_persistence_settings_);
        btn_persistence_settings_->style()->polish(btn_persistence_settings_);
        btn_persistence_settings_->setChecked(full_persistence);
    }
    if (act_background_persistence_)
        act_background_persistence_->setEnabled(has_exposed);
    if (act_text_persistence_) {
        act_text_persistence_->setEnabled(has_exposed && background_persistence_);
        if (!background_persistence_ && act_text_persistence_->isChecked()) {
            QSignalBlocker block(act_text_persistence_);
            act_text_persistence_->setChecked(false);
        }
    }
    if (title)
        apply_persistence_settings_to_title(title);
}

int TitleDock::live_text_row_height() const
{
    const QWidget *font_source = text_table_ ? text_table_->viewport() : nullptr;
    const QFontMetrics metrics(font_source ? font_source->font() : font());
    const int line_count = std::clamp(live_text_lines_per_row_,
                                      kMinLiveTextLinesPerRow,
                                      kMaxLiveTextLinesPerRow);
    const int text_height = metrics.lineSpacing() * line_count;
    const int padding = 12;
    return std::max(30, text_height + padding);
}

void TitleDock::apply_live_text_row_heights()
{
    if (!text_table_) return;

    const int height = live_text_row_height();
    text_table_->verticalHeader()->setDefaultSectionSize(height);
    for (int row = 0; row < text_table_->rowCount(); ++row)
        text_table_->setRowHeight(row, height);
}

void TitleDock::on_live_text_lines_per_row_changed(int lines)
{
    const int clamped_lines = std::clamp(lines, kMinLiveTextLinesPerRow, kMaxLiveTextLinesPerRow);
    if (spin_live_text_lines_per_row_ && spin_live_text_lines_per_row_->value() != clamped_lines) {
        QSignalBlocker block(spin_live_text_lines_per_row_);
        spin_live_text_lines_per_row_->setValue(clamped_lines);
    }

    if (live_text_lines_per_row_ == clamped_lines)
        return;

    live_text_lines_per_row_ = clamped_lines;
    apply_live_text_row_heights();
    save_dock_settings();
}

void TitleDock::update_external_data_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool has_title = (bool)title;
    const bool has_exposed = title && !exposed_text_layers(title).empty();
    const bool external_enabled = title && title->external_data_enabled;

    if (btn_data_sources_)
        btn_data_sources_->setEnabled(has_title && has_exposed);
    if (btn_live_text_settings_)
        btn_live_text_settings_->setEnabled(has_title);
    if (btn_external_refresh_) {
        btn_external_refresh_->setEnabled(has_title && has_exposed);
        btn_external_refresh_->setIcon(globe_status_icon(external_enabled));
        QSignalBlocker block(btn_external_refresh_);
        btn_external_refresh_->setChecked(external_enabled);
        btn_external_refresh_->setToolTip(external_enabled
            ? obsgs_tr("OBSTitles.RefreshExternalDataEnabledTooltip")
            : obsgs_tr("OBSTitles.RefreshExternalDataTooltip"));
    }

    if (btn_data_sources_ && btn_data_sources_->menu()) {
        auto actions = btn_data_sources_->menu()->actions();
        for (QAction *action : actions)
            action->setEnabled(has_title && has_exposed);
    }
    if (act_external_data_source_) {
        act_external_data_source_->setEnabled(has_title && has_exposed);
        act_external_data_source_->setText(external_enabled
            ? obsgs_tr("OBSTitles.DisableExternalDataSource")
            : obsgs_tr("OBSTitles.EnableExternalDataSource"));
    }
    if (act_external_data_settings_) {
        act_external_data_settings_->setEnabled(has_title && has_exposed);
    }
}

bool TitleDock::cue_live_text_row(int row, bool allow_uncue)
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    return cue_live_text_row_for_title(title, row, allow_uncue);
}

bool TitleDock::cue_live_text_row_for_title(const std::shared_ptr<Title> &title, int row, bool allow_uncue, bool force_restart)
{
    if (!title) return false;
    const QString title_id = QString::fromStdString(title->id);
    const bool selected_title = title_id == QString::fromStdString(selected_id());

    auto exposed_now = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed_now);

    if (exposed_now.empty()) {
        updating_exposed_text_ = true;
        cache_waiting_cue_row_ = -1;
        cache_waiting_title_id_.clear();
        const bool uncue_active = allow_uncue &&
            (title->current_cue_row == 0 || title->pending_cue_row == 0);
        title->current_cue_row = uncue_active ? -1 : 0;
        title->pending_cue_row = -1;
        title->cue_uncue_requested = uncue_active;
        ++title->cue_revision;
        TitleDataStore::instance().touch_runtime_change();
        seen_store_revision_ = TitleDataStore::instance().revision();
        updating_exposed_text_ = false;
        if (selected_title)
            update_live_text_runtime_status_fast(title, 0);
        schedule_title_list_cache_icon_update(title_id);
        return true;
    }

    if (row < 0 || row >= (int)title->live_text_rows.size())
        return false;

    const bool require_cached_cue = CacheManager::instance().cacheEnabled() &&
        CacheManager::instance().titleCacheability(title) != TitleCacheability::NonCacheable;
    if (require_cached_cue && cache_waiting_title_id_ == title_id && cache_waiting_cue_row_ == row) {
        if (selected_title)
            update_live_text_runtime_status_fast(title, row);
        else
            schedule_title_list_cache_icon_update(title_id);
        return false;
    }
    if (require_cached_cue && !CacheManager::instance().prepareLiveCueForPlayback(title, row)) {
        cache_waiting_cue_row_ = row;
        cache_waiting_title_id_ = QString::fromStdString(title->id);
        OGS_LOG_INFO("LiveCueUI", QStringLiteral("Armed cue row waiting for prerender title=%1 row=%2")
                                    .arg(cache_waiting_title_id_).arg(row));
        /* prepareLiveCueForPlayback() has already queued high-priority disk
         * hydration (or missing renders) for the exact steady/transition state.
         * Nearby rows remain ordinary background work. */
        CacheManager::instance().preloadLiveCues(title, row, 2);
        if (selected_title)
            update_live_text_runtime_status_fast(title, row);
        else
            schedule_title_list_cache_icon_update(title_id);
        QTimer::singleShot(0, this, [this, title, row]() {
            const QString title_id = QString::fromStdString(title->id);
            const bool selected_title = title_id == QString::fromStdString(selected_id());
            if (selected_title)
                update_live_text_runtime_status_fast(title, row);
            else
                schedule_title_list_cache_icon_update(title_id);
            if (cache_waiting_title_id_ == title_id && cache_waiting_cue_row_ == row &&
                CacheManager::instance().prepareLiveCueForPlayback(title, row)) {
                cache_waiting_cue_row_ = -1;
                cache_waiting_title_id_.clear();
                cue_live_text_row_for_title(title, row, false, true);
            }
        });
        return false;
    }

    updating_exposed_text_ = true;
    apply_persistence_settings_to_title(title);
    const bool is_active_cue = !force_restart && title->current_cue_row == row;
    const bool is_pending_cue = !force_restart && title->pending_cue_row == row;
    const int previous_row = title->current_cue_row >= 0 ? title->current_cue_row : title->pending_cue_row;
    const bool can_persist_transition = title->cue_background_persistence &&
        (title->playback_mode == 1 || title->playback_mode == 2) &&
        previous_row >= 0 && previous_row != row;
    const bool needs_outro_before_cue =
        (title->playback_mode == 1 || title->playback_mode == 2) &&
        title->current_cue_row >= 0 && title->current_cue_row != row;

    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.assign(exposed_now.size(), false);
    if (!(allow_uncue && (is_active_cue || is_pending_cue)))
        title->cue_uncue_requested = false;

    if (allow_uncue && (is_active_cue || is_pending_cue)) {
        cache_waiting_cue_row_ = -1;
        cache_waiting_title_id_.clear();
        /* Keep current_cue_row active while the source plays the outro.  This
         * preserves the cued status in the dock and gives cached playback the
         * exact outgoing row until the last frame. */
        title->pending_cue_row = -1;
        title->cue_uncue_requested = true;
        title->cue_persistence_transition = false;
        title->cue_persistent_text_columns.clear();
    } else if (can_persist_transition) {
        for (int col = 0; col < (int)exposed_now.size(); ++col) {
            auto &target = exposed_now[col];
            const int previous_value_row = target && target->exposed_single_value ? 0 : previous_row;
            const int next_value_row = target && target->exposed_single_value ? 0 : row;
            if (title->cue_text_persistence &&
                previous_value_row >= 0 && previous_value_row < (int)title->live_text_rows.size() &&
                next_value_row >= 0 && next_value_row < (int)title->live_text_rows.size() &&
                col < (int)title->live_text_rows[previous_value_row].size() &&
                col < (int)title->live_text_rows[next_value_row].size() &&
                title->live_text_rows[previous_value_row][col] == title->live_text_rows[next_value_row][col])
                title->cue_persistent_text_columns[col] = true;
        }
        title->pending_cue_row = row;
        title->cue_persistence_transition = true;
    } else if (needs_outro_before_cue) {
        title->pending_cue_row = row;
    } else if (force_restart || !is_active_cue || title->pending_cue_row >= 0) {
        for (int col = 0; col < (int)exposed_now.size(); ++col) {
            auto &target = exposed_now[col];
            const int value_row = target && target->exposed_single_value ? 0 : row;
            if (value_row < 0 || value_row >= (int)title->live_text_rows.size() ||
                col >= (int)title->live_text_rows[value_row].size())
                continue;
            apply_live_cue_layer_value(target, title->live_text_rows[value_row][col]);
        }
        title->current_cue_row = row;
        title->pending_cue_row = -1;
    }

    ++title->cue_revision;
    cache_waiting_cue_row_ = -1;
    cache_waiting_title_id_.clear();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    CacheManager::instance().preloadLiveCues(title, row, 2);
    updating_exposed_text_ = false;
    if (selected_title) {
        update_live_text_runtime_status_fast(title, row, previous_row);
        QTimer::singleShot(0, this, [this, title, row, previous_row]() { update_live_text_runtime_status_fast(title, row, previous_row); });
    } else {
        schedule_title_list_cache_icon_update(title_id);
    }
    return true;
}

int TitleDock::live_text_playlist_row_count(const std::shared_ptr<Title> &title) const
{
    if (!title) return 0;
    auto exposed = exposed_text_layers(title);
    if (exposed.empty())
        return 1;
    return (int)title->live_text_rows.size();
}

int TitleDock::next_playlist_row(const std::shared_ptr<Title> &title, int current_row, int row_count) const
{
    if (row_count <= 0) return 0;
    const bool reverse = title ? title->playlist_reverse : playlist_reverse_;
    return (current_row + (reverse ? -1 : 1) + row_count) % row_count;
}

int TitleDock::playlist_step_delay_ms(const std::shared_ptr<Title> &title) const
{
    if (!title) return 1000;

    double seconds = title->playlist_hold_seconds;
    if (title->playback_mode == 1)
        seconds += std::clamp(title->loop_end, title->loop_start, title->duration);
    else if (title->playback_mode == 2)
        seconds += std::clamp(title->pause_time, 0.0, title->duration);
    else
        seconds += title->duration;

    return std::max(1, (int)std::round(seconds * 1000.0));
}

int TitleDock::playlist_hold_delay_ms(const std::shared_ptr<Title> &title) const
{
    const double seconds = title ? title->playlist_hold_seconds : playlist_hold_seconds_;
    return std::max(1, (int)std::round(seconds * 1000.0));
}

bool TitleDock::playlist_row_is_terminal(const std::shared_ptr<Title> &title, int row, int row_count) const
{
    if (row < 0 || row_count <= 0) return false;
    const bool reverse = title ? title->playlist_reverse : playlist_reverse_;
    return reverse ? row == 0 : row == row_count - 1;
}

void TitleDock::play_playlist_outro(const std::shared_ptr<Title> &title)
{
    if (!title) return;

    const int row = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    if (row >= 0)
        cue_live_text_row_for_title(title, row, true);
    else if (live_text_playlist_row_count(title) == 1)
        cue_live_text_row_for_title(title, 0, true);
}

void TitleDock::stop_playlist()
{
    stop_playlist_for_title(TitleDataStore::instance().get_title(selected_id()));
}

void TitleDock::stop_playlist_for_title(const std::shared_ptr<Title> &title)
{
    if (!title)
        return;
    title->playlist_active = false;
    title->playlist_next_due_ms = 0;
    title->playlist_stop_after_due = false;
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();

    bool any_active = false;
    for (const auto &candidate : TitleDataStore::instance().titles()) {
        if (candidate && candidate->playlist_active) {
            any_active = true;
            break;
        }
    }
    if (!any_active && playlist_timer_)
        playlist_timer_->stop();

    if (QString::fromStdString(title->id) == QString::fromStdString(selected_id())) {
        if (btn_playlist_ && btn_playlist_->isChecked()) {
            QSignalBlocker block(btn_playlist_);
            btn_playlist_->setChecked(false);
        }
        update_playlist_countdown_label();
    }
    update_title_list_cache_icon(QString::fromStdString(title->id));
}

void TitleDock::sync_playlist_runtime_state()
{
    bool any_active = false;
    for (const auto &title : TitleDataStore::instance().titles()) {
        if (title && title->playlist_active) {
            any_active = true;
            break;
        }
    }

    if (any_active) {
        if (playlist_timer_ && !playlist_timer_->isActive())
            playlist_timer_->start();
    } else if (playlist_timer_ && playlist_timer_->isActive()) {
        playlist_timer_->stop();
    }

    if (list_) {
        for (int i = 0; i < list_->count(); ++i) {
            auto *item = list_->item(i);
            if (!item)
                continue;
            const QString title_id = item->data(Qt::UserRole).toString();
            auto title = TitleDataStore::instance().get_title(title_id.toStdString());
            const bool active = title && title->playlist_active;
            if (item->data(kTitlePlaylistActiveRole).toBool() != active)
                schedule_title_list_cache_icon_update(title_id);
        }
    }

    update_playlist_controls();
}

void TitleDock::update_playlist_countdown_label()
{
    if (!playlist_countdown_lbl_) return;

    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool active = title && title->playlist_active && title->playlist_next_due_ms > 0;
    if (!active) {
        playlist_countdown_lbl_->setText(QStringLiteral("--"));
        playlist_countdown_lbl_->setVisible(false);
        return;
    }

    const qint64 remaining_ms = std::max<qint64>(0, title->playlist_next_due_ms - QDateTime::currentMSecsSinceEpoch());
    const double remaining_seconds = remaining_ms / 1000.0;
    playlist_countdown_lbl_->setText(QStringLiteral("%1s").arg(remaining_seconds, 0, 'f', remaining_seconds < 10.0 ? 1 : 0));
    playlist_countdown_lbl_->setVisible(true);
}

void TitleDock::start_playlist_step()
{
    start_playlist_step_for_title(TitleDataStore::instance().get_title(selected_id()));
}

void TitleDock::start_playlist_step_for_title(const std::shared_ptr<Title> &title)
{
    const int row_count = live_text_playlist_row_count(title);
    if (!title || row_count <= 0) {
        stop_playlist_for_title(title);
        return;
    }

    if (title->playlist_next_row < 0 || title->playlist_next_row >= row_count)
        title->playlist_next_row = title->playlist_reverse ? row_count - 1 : 0;

    const int row = title->playlist_next_row;
    const bool already_active = title->pending_cue_row < 0 && title->current_cue_row == row;
    if (!already_active)
        cue_live_text_row_for_title(title, row, false);
    title->playlist_next_row = next_playlist_row(title, row, row_count);
    title->playlist_stop_after_due = !title->playlist_loop && playlist_row_is_terminal(title, row, row_count);

    title->playlist_next_due_ms = QDateTime::currentMSecsSinceEpoch() +
        (already_active ? playlist_hold_delay_ms(title) : playlist_step_delay_ms(title));
    if (playlist_timer_ && !playlist_timer_->isActive())
        playlist_timer_->start();
    if (QString::fromStdString(title->id) == QString::fromStdString(selected_id()))
        update_playlist_countdown_label();
    update_title_list_cache_icon(QString::fromStdString(title->id));
}

void TitleDock::on_playlist_tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool any_active = false;
    for (const auto &title : TitleDataStore::instance().titles()) {
        if (!title || !title->playlist_active)
            continue;
        any_active = true;
        if (title->playlist_next_due_ms <= 0 || now < title->playlist_next_due_ms)
            continue;

        if (title->playlist_stop_after_due) {
            play_playlist_outro(title);
            stop_playlist_for_title(title);
        } else {
            start_playlist_step_for_title(title);
        }
    }

    if (!any_active && playlist_timer_)
        playlist_timer_->stop();
    update_playlist_countdown_label();
}

void TitleDock::on_toggle_playlist(bool enabled)
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!enabled) {
        stop_playlist_for_title(title);
        return;
    }

    const int row_count = live_text_playlist_row_count(title);
    if (!title || row_count <= 0) {
        if (btn_playlist_ && btn_playlist_->isChecked()) {
            QSignalBlocker block(btn_playlist_);
            btn_playlist_->setChecked(false);
        }
        update_playlist_countdown_label();
        return;
    }

    title->playlist_active = true;
    title->playlist_loop = playlist_loop_;
    title->playlist_reverse = playlist_reverse_;
    title->playlist_hold_seconds = playlist_hold_seconds_;
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    int base = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    if (title->pending_cue_row < 0 && base >= 0 && base < row_count) {
        title->playlist_next_row = next_playlist_row(title, base, row_count);
        title->playlist_stop_after_due = !title->playlist_loop && playlist_row_is_terminal(title, base, row_count);
        title->playlist_next_due_ms = QDateTime::currentMSecsSinceEpoch() + playlist_hold_delay_ms(title);
        if (playlist_timer_ && !playlist_timer_->isActive())
            playlist_timer_->start();
        update_playlist_countdown_label();
        update_title_list_cache_icon(QString::fromStdString(title->id));
        return;
    }

    if (base >= 0 && base < row_count)
        title->playlist_next_row = base;
    else
        title->playlist_next_row = title->playlist_reverse ? row_count - 1 : 0;

    start_playlist_step_for_title(title);
}

void TitleDock::update_playlist_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool enabled = title && live_text_playlist_row_count(title) > 0;
    if (title) {
        playlist_loop_ = title->playlist_loop;
        playlist_reverse_ = title->playlist_reverse;
        playlist_hold_seconds_ = title->playlist_hold_seconds;
    }
    if (btn_playlist_) {
        QSignalBlocker block(btn_playlist_);
        btn_playlist_->setEnabled(enabled);
        btn_playlist_->setChecked(enabled && title->playlist_active);
    }
    if (act_playlist_loop_) {
        act_playlist_loop_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_loop_);
        act_playlist_loop_->setChecked(title ? title->playlist_loop : playlist_loop_);
    }
    if (act_playlist_reverse_) {
        act_playlist_reverse_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_reverse_);
        act_playlist_reverse_->setChecked(title ? title->playlist_reverse : playlist_reverse_);
    }
    if (act_playlist_restart_on_active_) {
        act_playlist_restart_on_active_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_restart_on_active_);
        act_playlist_restart_on_active_->setChecked(title && title->playlist_restart_on_source_active);
    }
    if (act_playlist_stop_on_inactive_) {
        act_playlist_stop_on_inactive_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_stop_on_inactive_);
        act_playlist_stop_on_inactive_->setChecked(title && title->playlist_stop_on_source_inactive);
    }
    if (act_playlist_hold_)
        act_playlist_hold_->setEnabled(enabled);
    if (auto *hold_action = qobject_cast<QWidgetAction *>(act_playlist_hold_)) {
        if (auto *widget = hold_action->defaultWidget()) {
            if (auto *spin = widget->findChild<QDoubleSpinBox *>()) {
                QSignalBlocker block(spin);
                spin->setValue(title ? title->playlist_hold_seconds : playlist_hold_seconds_);
            }
        }
    }
    update_playlist_countdown_label();
}

void TitleDock::populate_exposed_text()
{
    if (!text_table_) return;
    QSignalBlocker block(text_table_);
    QSignalBlocker header_block(text_table_->horizontalHeader());
    text_table_->clear();
    text_table_->setRowCount(0);
    text_table_->setColumnCount(0);

    auto *header = live_text_cue_header(text_table_);

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) {
        if (header) header->set_select_all_visible(false);
        text_editor_lbl_->setText(obsgs_tr("OBSTitles.LiveTextSelectTitle"));
        text_table_->setEnabled(false);
        if (btn_add_text_row_) btn_add_text_row_->setEnabled(false);
        if (btn_delete_text_row_) btn_delete_text_row_->setEnabled(false);
        if (btn_row_up_) btn_row_up_->setEnabled(false);
        if (btn_row_down_) btn_row_down_->setEnabled(false);
        update_playlist_controls();
        update_persistence_controls();
        update_external_data_controls();
        update_live_text_select_all_state();
        return;
    }

    const QString current_width_title_id = QString::fromStdString(title->id);
    const bool first_width_fit_for_title = live_text_width_initialized_title_id_ != current_width_title_id;
    if (first_width_fit_for_title)
        live_text_width_initialized_title_id_ = current_width_title_id;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_)) {
        const QString border_title_id = QString::fromStdString(title->id);
        cue_table->cue_row_border_color = [this, border_title_id](int row) -> QColor {
            auto current_title = TitleDataStore::instance().get_title(selected_id());
            if (!current_title || QString::fromStdString(current_title->id) != border_title_id)
                return QColor();
            const bool waiting_for_prerender = cache_waiting_title_id_ == border_title_id &&
                cache_waiting_cue_row_ == row;
            if (row == current_title->current_cue_row)
                return live_cue_state_color(true, false);
            if (row == current_title->pending_cue_row || waiting_for_prerender)
                return live_cue_state_color(false, true);
            return QColor();
        };
    }

    const bool has_exposed = !exposed.empty();
    text_table_->setEnabled(true);
    if (btn_add_text_row_) btn_add_text_row_->setEnabled(has_exposed);
    if (btn_delete_text_row_) btn_delete_text_row_->setEnabled(has_exposed);
    if (btn_row_up_) btn_row_up_->setEnabled(has_exposed);
    if (btn_row_down_) btn_row_down_->setEnabled(has_exposed);
    text_editor_lbl_->setText(obsgs_tr("OBSTitles.LiveTextCues"));
    if (header) header->set_select_all_visible(has_exposed);
    if (!has_exposed) {
        text_table_->setRowCount(1);
        text_table_->setColumnCount(3);
        text_table_->setHorizontalHeaderLabels(QStringList()
                                               << QString()
                                               << QString()
                                               << obsgs_tr("OBSTitles.Title"));
        text_table_->horizontalHeader()->setSectionsMovable(false);
        text_table_->horizontalHeader()->setSectionResizeMode(kStaticCueColumn, QHeaderView::ResizeToContents);
        text_table_->horizontalHeader()->setSectionResizeMode(kStaticTitleColumn, QHeaderView::Stretch);
        text_table_->horizontalHeader()->setSectionResizeMode(kStaticCacheColumn, QHeaderView::ResizeToContents);
        text_table_->setVerticalHeaderItem(0, new QTableWidgetItem(QStringLiteral("1")));

        auto *title_item = new QTableWidgetItem(QString::fromStdString(title->name));
        title_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        title_item->setToolTip(title_item->text());
        text_table_->setItem(0, kStaticTitleColumn, title_item);

        auto *cue = live_cue_button(text_table_, 0, kStaticCueColumn);
        if (cue) {
            const bool current = title->current_cue_row == 0;
            const bool queued = title->pending_cue_row == 0 ||
                (cache_waiting_title_id_ == QString::fromStdString(title->id) && cache_waiting_cue_row_ == 0);
            cue->setIcon(obs_icon("cue.svg", live_cue_state_color(current, queued)));
            cue->setToolTip(obsgs_tr("OBSTitles.PlayCueTooltip"));
            cue->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;padding:0;}"
                "QToolButton:hover{background:transparent;border:none;}"));
            connect(cue, &QToolButton::clicked, this, [this]() { cue_live_text_row(0, true); });
        }
        update_live_text_cache_cell(title, 0);
        update_live_text_select_cell_status(0);
        apply_live_text_row_heights();
        adjust_live_text_table_columns(first_width_fit_for_title);
        update_playlist_controls();
        update_persistence_controls();
        update_external_data_controls();
        update_live_text_select_all_state();
        return;
    }

    /* Reconcile stable row IDs/cache states before creating row widgets. This
     * prevents a freshly rebuilt table from briefly binding old numeric-row
     * states to the new row order after delete/re-add/reorder operations. */
    CacheManager::instance().refreshLiveCueStructureAsync(title);

    text_table_->setRowCount((int)title->live_text_rows.size());
    text_table_->setColumnCount((int)exposed.size() + 3);

    QStringList headers;
    headers << QString() << QString() << "";
    for (const auto &layer : exposed)
        headers << live_text_layer_header(layer);
    text_table_->setHorizontalHeaderLabels(headers);
    for (int col = 0; col < (int)exposed.size(); ++col) {
        if (auto *item = text_table_->horizontalHeaderItem(col + kLiveFirstValueColumn))
            item->setToolTip(live_text_layer_header(exposed[col]));
    }
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->horizontalHeader()->setSectionsMovable(true);
    restore_live_text_header_state();
    auto *table_header = text_table_->horizontalHeader();
    if (table_header) {
        const int cue_visual = table_header->visualIndex(kLiveCueColumn);
        if (cue_visual > 0)
            table_header->moveSection(cue_visual, 0);
        const int cache_visual = table_header->visualIndex(kLiveCacheColumn);
        if (cache_visual != 1)
            table_header->moveSection(cache_visual, 1);
        const int select_visual = table_header->visualIndex(kLiveSelectColumn);
        if (select_visual != 2)
            table_header->moveSection(select_visual, 2);
    }
    adjust_live_text_table_columns(first_width_fit_for_title);

    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        text_table_->setVerticalHeaderItem(row, new QTableWidgetItem(QString::number(row + 1)));
        auto *select_item = new QTableWidgetItem();
        select_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        select_item->setCheckState(Qt::Unchecked);
        select_item->setTextAlignment(Qt::AlignCenter);
        text_table_->setItem(row, kLiveSelectColumn, select_item);
        update_live_text_cache_cell(title, row);
        for (int col = 0; col < (int)exposed.size(); ++col) {
            if (exposed[col] && exposed[col]->exposed_single_value && row > 0) {
                auto *single_item = new QTableWidgetItem(QString());
                single_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                single_item->setToolTip(obsgs_tr("OBSTitles.ExposedSingleValue"));
                text_table_->setItem(row, col + kLiveFirstValueColumn, single_item);
                continue;
            }
            if (layer_uses_image_cue_value(exposed[col])) {
                auto *image = new LiveImageCueField(QString::fromStdString(title->live_text_rows[row][col]), text_table_);
                image->path_changed = [this, title, row, col](const QString &path) {
                    commit_live_text_cell_edit(title, row, col, path);
                };
                text_table_->setCellWidget(row, col + kLiveFirstValueColumn, image);
            } else {
                auto *edit = new LiveTextCueField(QString::fromStdString(title->live_text_rows[row][col]), text_table_);
                edit->setPlaceholderText(live_text_layer_header(exposed[col]));
                edit->setStyleSheet("QPlainTextEdit{padding:3px;}");
                edit->editing_finished = [this, title, row, col](const QString &text) {
                    commit_live_text_cell_edit(title, row, col, text);
                };
                QPointer<TitleDock> dock_guard(this);
                edit->focus_changed = [dock_guard, title, row](bool focused) {
                    if (dock_guard)
                        dock_guard->set_live_text_row_render_paused(title, row, focused);
                };
                text_table_->setCellWidget(row, col + kLiveFirstValueColumn, edit);
            }
        }

        const bool waiting_for_prerender = cache_waiting_title_id_ == QString::fromStdString(title->id) &&
            cache_waiting_cue_row_ == row;
        auto *cue = live_cue_button(text_table_, row, kLiveCueColumn);
        if (cue) {
            cue->setToolTip(obsgs_tr("OBSTitles.PlayCueTooltip"));
            cue->setIcon(obs_icon("cue.svg", live_cue_state_color(row == title->current_cue_row,
                                                                    row == title->pending_cue_row || waiting_for_prerender)));
            cue->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;padding:0;}"
                "QToolButton:hover{background:transparent;border:none;}"));
            connect(cue, &QToolButton::clicked, this, [this, row]() { cue_live_text_row(row, true); });
        }
        update_live_text_select_cell_status(row);
    }
    apply_live_text_row_heights();
    adjust_live_text_table_columns(first_width_fit_for_title);
    update_playlist_controls();
    update_persistence_controls();
    update_external_data_controls();
    update_live_text_select_all_state();
}

void TitleDock::on_export_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    QString path = QFileDialog::getSaveFileName(
        this, obsgs_tr("OBSTitles.ExportData"), QString(),
        obsgs_tr("OBSTitles.JsonFileFilter"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");

    QJsonArray columns;
    for (const auto &layer : exposed) {
        QJsonObject column;
        column.insert(QStringLiteral("id"), QString::fromStdString(layer->id));
        column.insert(QStringLiteral("name"), live_text_layer_header(layer));
        columns.append(column);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("OBS Graphics Studio Pro live text cues"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("title"), QString::fromStdString(title->name));
    root.insert(QStringLiteral("columns"), columns);
    root.insert(QStringLiteral("rows"), live_text_rows_to_json(title->live_text_rows));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportData"),
                             obsgs_tr("OBSTitles.ExportDataFailed"));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    status_lbl_->setText(obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ImportData"), QString(),
        obsgs_tr("OBSTitles.JsonFileFilter"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportData"),
                             obsgs_tr("OBSTitles.ImportDataFailed"));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportData"), parse_error.errorString());
        return;
    }

    std::vector<std::vector<std::string>> imported_rows;
    QString error;
    if (!live_text_rows_from_json(doc, imported_rows, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportData"), error);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    const int existing_rows = (int)title->live_text_rows.size();
    const int imported_row_count = (int)imported_rows.size();
    if (imported_row_count > existing_rows) {
        const auto answer = QMessageBox::question(
            this, obsgs_tr("OBSTitles.ImportData"),
            obsgs_tr("OBSTitles.ImportDataCropWarning")
                .arg(imported_row_count)
                .arg(existing_rows),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        imported_rows.resize(existing_rows);
    }

    for (auto &row : imported_rows)
        row.resize(exposed.size());
    title->live_text_rows = std::move(imported_rows);
    title->live_text_row_ids.clear();
    normalize_live_text_rows(title, exposed);
    title->current_cue_row = -1;
    title->pending_cue_row = -1;
    ++title->cue_revision;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    populate_exposed_text();
    status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import_append_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ImportAppendData"), QString(),
        obsgs_tr("OBSTitles.JsonFileFilter"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportAppendData"),
                             obsgs_tr("OBSTitles.ImportDataFailed"));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportAppendData"), parse_error.errorString());
        return;
    }

    std::vector<std::vector<std::string>> imported_rows;
    QString error;
    if (!live_text_rows_from_json(doc, imported_rows, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportAppendData"), error);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    for (auto &row : imported_rows) {
        row.resize(exposed.size());
        title->live_text_rows.push_back(std::move(row));
    }
    normalize_live_text_rows(title, exposed);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    populate_exposed_text();
    status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_toggle_external_data_source()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    title->external_data_enabled = !title->external_data_enabled;
    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    update_external_data_controls();
}

void TitleDock::on_show_external_data_settings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.ExternalDataSource"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *header = new QLabel(obsgs_tr("OBSTitles.ExternalDataSource"), &dialog);
    set_bold_label(header);
    layout->addWidget(header);

    auto *tabs = new QTabWidget(&dialog);
    const QStringList names = {
        obsgs_tr("OBSTitles.CSV"),
        obsgs_tr("OBSTitles.URL"),
        obsgs_tr("OBSTitles.RSSFeed"),
        obsgs_tr("OBSTitles.DataSourcesSettings")
    };
    for (const QString &name : names) {
        auto *tab = new QWidget(tabs);
        auto *tab_layout = new QVBoxLayout(tab);
        auto *placeholder = new QLabel(obsgs_tr("OBSTitles.ExternalDataSettingsPlaceholder"), tab);
        placeholder->setWordWrap(true);
        tab_layout->addWidget(placeholder);
        tab_layout->addStretch();
        tabs->addTab(tab, name);
    }
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.resize(520, 360);
    dialog.exec();
}

void TitleDock::on_refresh_external_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    status_lbl_->setText(obsgs_tr("OBSTitles.ExternalDataRefreshed"));
}

void TitleDock::on_add_live_text_row()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    if (exposed.empty()) return;

    auto selected_rows = selected_live_text_rows();
    std::vector<std::string> row(exposed.size());
    if (selected_rows.size() == 1) {
        const int source_row = selected_rows.front();
        if (source_row >= 0 && source_row < (int)title->live_text_rows.size())
            row = title->live_text_rows[source_row];
    }
    row.resize(exposed.size());

    title->live_text_rows.push_back(std::move(row));
    title->live_text_row_ids.push_back(TitleDataStore::make_uuid());
    const int added_row = (int)title->live_text_rows.size() - 1;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();

    /* Adding a row creates only the transition variants to/from that row.
     * Refresh all requirement sets so those new states are queued, while stable
     * content-addressed keys reuse every previously rendered frame. */
    CacheManager::instance().refreshLiveCueStructure(title);
    populate_exposed_text();
    apply_live_text_row_selection({added_row}, false);
    update_live_text_cache_cell(title, added_row);
}

void TitleDock::on_delete_live_text_rows()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    updating_exposed_text_ = true;
    int next_row = rows.front();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const int row = *it;
        if (row < 0 || row >= (int)title->live_text_rows.size())
            continue;
        title->live_text_rows.erase(title->live_text_rows.begin() + row);
        if (row < static_cast<int>(title->live_text_row_ids.size()))
            title->live_text_row_ids.erase(title->live_text_row_ids.begin() + row);
        if (title->current_cue_row == row)
            title->current_cue_row = -1;
        else if (title->current_cue_row > row)
            --title->current_cue_row;
        if (title->pending_cue_row == row)
            title->pending_cue_row = -1;
        else if (title->pending_cue_row > row)
            --title->pending_cue_row;
    }

    auto exposed_now = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed_now);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    updating_exposed_text_ = false;
    populate_exposed_text();
    if (!title->live_text_rows.empty())
        text_table_->selectRow(std::min(next_row, (int)title->live_text_rows.size() - 1));
}

void TitleDock::on_move_live_text_row_up()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    const bool restore_checked = has_checked_live_text_rows();
    const int row_count = (int)title->live_text_rows.size();
    std::vector<bool> selected(row_count, false);
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            selected[row] = true;
    }

    std::vector<int> order(row_count);
    std::iota(order.begin(), order.end(), 0);
    bool moved = false;
    for (int visual = 1; visual < row_count; ++visual) {
        if (selected[order[visual]] && !selected[order[visual - 1]]) {
            std::swap(order[visual], order[visual - 1]);
            moved = true;
        }
    }
    if (!moved) return;

    ensure_live_text_row_ids(*title);
    std::vector<std::vector<std::string>> reordered;
    std::vector<std::string> reordered_ids;
    reordered.reserve(title->live_text_rows.size());
    reordered_ids.reserve(title->live_text_row_ids.size());
    std::vector<int> new_index(row_count, -1);
    for (int visual = 0; visual < row_count; ++visual) {
        new_index[order[visual]] = visual;
        reordered.push_back(std::move(title->live_text_rows[order[visual]]));
        reordered_ids.push_back(std::move(title->live_text_row_ids[order[visual]]));
    }
    title->live_text_rows = std::move(reordered);
    title->live_text_row_ids = std::move(reordered_ids);
    if (title->current_cue_row >= 0 && title->current_cue_row < row_count)
        title->current_cue_row = new_index[title->current_cue_row];
    if (title->pending_cue_row >= 0 && title->pending_cue_row < row_count)
        title->pending_cue_row = new_index[title->pending_cue_row];

    std::vector<int> moved_rows;
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            moved_rows.push_back(new_index[row]);
    }
    std::sort(moved_rows.begin(), moved_rows.end());

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    apply_live_text_row_selection(moved_rows, restore_checked);
}

void TitleDock::on_move_live_text_row_down()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    const bool restore_checked = has_checked_live_text_rows();
    const int row_count = (int)title->live_text_rows.size();
    std::vector<bool> selected(row_count, false);
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            selected[row] = true;
    }

    std::vector<int> order(row_count);
    std::iota(order.begin(), order.end(), 0);
    bool moved = false;
    for (int visual = row_count - 2; visual >= 0; --visual) {
        if (selected[order[visual]] && !selected[order[visual + 1]]) {
            std::swap(order[visual], order[visual + 1]);
            moved = true;
        }
    }
    if (!moved) return;

    ensure_live_text_row_ids(*title);
    std::vector<std::vector<std::string>> reordered;
    std::vector<std::string> reordered_ids;
    reordered.reserve(title->live_text_rows.size());
    reordered_ids.reserve(title->live_text_row_ids.size());
    std::vector<int> new_index(row_count, -1);
    for (int visual = 0; visual < row_count; ++visual) {
        new_index[order[visual]] = visual;
        reordered.push_back(std::move(title->live_text_rows[order[visual]]));
        reordered_ids.push_back(std::move(title->live_text_row_ids[order[visual]]));
    }
    title->live_text_rows = std::move(reordered);
    title->live_text_row_ids = std::move(reordered_ids);
    if (title->current_cue_row >= 0 && title->current_cue_row < row_count)
        title->current_cue_row = new_index[title->current_cue_row];
    if (title->pending_cue_row >= 0 && title->pending_cue_row < row_count)
        title->pending_cue_row = new_index[title->pending_cue_row];

    std::vector<int> moved_rows;
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            moved_rows.push_back(new_index[row]);
    }
    std::sort(moved_rows.begin(), moved_rows.end());

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    apply_live_text_row_selection(moved_rows, restore_checked);
}


void TitleDock::select_title(const std::string &id)
{
    populate_list();
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString().toStdString() == id) {
            list_->setCurrentRow(i);
            break;
        }
    }
}

std::shared_ptr<Title> TitleDock::create_template_title(const std::string &name,
                                                         int template_id)
{
    auto title = TitleDataStore::instance().create_title(name);
    title->layers.clear();
    title->bg_color = 0x00000000;
    title->duration = 7.0;

    auto add_rect = [&](const std::string &layer_name,
                        double x, double y, float w, float h,
                        uint32_t color, float radius = 0.0f) {
        auto layer = std::make_shared<Layer>();
        layer->id = TitleDataStore::make_uuid();
        layer->name = layer_name;
        layer->type = LayerType::SolidRect;
        layer->position.static_value.x = x;
        layer->position.static_value.y = y;
        layer->rect_width = w;
        layer->rect_height = h;
        layer->size.static_value.x = w;
        layer->size.static_value.y = h;
        layer->corner_radius = radius;
        layer->corner_radius_tl = radius;
        layer->corner_radius_tr = radius;
        layer->corner_radius_br = radius;
        layer->corner_radius_bl = radius;
        layer->corner_radius_locked = true;
        layer->fill_color = color;
        layer->fill_color_a.static_value = (color >> 24) & 0xFF;
        layer->fill_color_r.static_value = (color >> 16) & 0xFF;
        layer->fill_color_g.static_value = (color >> 8) & 0xFF;
        layer->fill_color_b.static_value = color & 0xFF;
        layer->out_time = title->duration;
        title->layers.push_back(layer);
        return layer;
    };

    auto add_text = [&](const std::string &layer_name,
                        const std::string &text,
                        double x, double y, int size,
                        uint32_t color, bool bold = false,
                        int align_h = 1, int align_v = 1) {
        auto layer = std::make_shared<Layer>();
        layer->id = TitleDataStore::make_uuid();
        layer->name = layer_name;
        layer->type = LayerType::Text;
        layer->text_content = text;
        layer->rich_text_html.clear();
        layer->expose_text = true;
        layer->font_family = "Arial";
        layer->font_size = size;
        layer->font_bold = bold;
        layer->text_color = color;
        layer->text_color_a.static_value = (color >> 24) & 0xFF;
        layer->text_color_r.static_value = (color >> 16) & 0xFF;
        layer->text_color_g.static_value = (color >> 8) & 0xFF;
        layer->text_color_b.static_value = color & 0xFF;
        layer->rect_width = 960.0f;
        layer->rect_height = 160.0f;
        layer->size.static_value.x = layer->rect_width;
        layer->size.static_value.y = layer->rect_height;
        layer->position.static_value.x = x;
        layer->position.static_value.y = y;
        layer->align_h = align_h;
        layer->align_v = align_v;
        layer->out_time = title->duration;
        title->layers.push_back(layer);
        return layer;
    };

    switch (template_id) {
    case 1: { /* Lower third */
        title->duration = 8.0;
        add_rect(obs_text_std("OBSTitles.LayerLowerThirdBackplate"), 640, 835, 1120, 155, 0xD0161B24, 18.0f);
        add_rect(obs_text_std("OBSTitles.LayerAccentBar"), 120, 835, 18, 155, 0xFF00A3FF, 9.0f);
        add_text(obs_text_std("OBSTitles.LayerName"), name, 670, 800, 58, 0xFFFFFFFF, true, 0, 1);
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateSubtitleRole"), 670, 872, 34, 0xFFE8E8E8, false, 0, 1);
        break;
    }
    case 2: { /* Center title */
        title->duration = 6.0;
        add_rect(obs_text_std("OBSTitles.LayerSoftPanel"), 960, 540, 1280, 270, 0xB0101018, 28.0f);
        add_rect(obs_text_std("OBSTitles.LayerTopAccent"), 960, 395, 520, 10, 0xFF00A3FF, 5.0f);
        add_text(obs_text_std("OBSTitles.LayerMainTitle"), name, 960, 505, 86, 0xFFFFFFFF, true, 1, 1);
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateEditableSubtitle"), 960, 610, 42, 0xFFE0E0E0, false, 1, 1);
        break;
    }
    case 3: { /* Ticker / strap */
        title->duration = 12.0;
        add_rect(obs_text_std("OBSTitles.LayerTickerBackground"), 960, 1010, 1920, 110, 0xE0101010, 0.0f);
        add_rect(obs_text_std("OBSTitles.LayerTickerAccent"), 125, 1010, 250, 110, 0xFF0078D4, 0.0f);
        add_text(obs_text_std("OBSTitles.LayerTickerLabel"), obs_text_std("OBSTitles.TemplateLive"), 125, 1010, 44, 0xFFFFFFFF, true, 1, 1);
        auto ticker = add_text(obs_text_std("OBSTitles.LayerTickerText"), name, 1030, 1010, 44, 0xFFFFFFFF, false, 0, 1);
        ticker->type = LayerType::Ticker;
        ticker->rect_width = 1640.0f;
        ticker->size.static_value.x = ticker->rect_width;
        ticker->ticker_style = 0;
        ticker->ticker_direction = 1;
        ticker->ticker_speed = 140.0;
        break;
    }
    case 4: { /* Clock */
        title->duration = 10.0;
        add_rect(obs_text_std("OBSTitles.ClockBox"), 960, 540, 620, 210, 0xC0101018, 24.0f);
        auto clock = add_text(obs_text_std("OBSTitles.Clock"), name, 960, 530, 92, 0xFFFFFFFF, true, 1, 1);
        clock->type = LayerType::Clock;
        clock->clock_format = "H:i:s";
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateClockSubtitle"), 960, 640, 34, 0xFFE0E0E0, false, 1, 1);
        break;
    }
    case 5: { /* Full screen */
        title->duration = 7.0;
        add_rect(obs_text_std("OBSTitles.LayerSoftPanel"), 960, 540, 1920, 1080, 0xD0101018, 0.0f);
        add_rect(obs_text_std("OBSTitles.LayerTopAccent"), 960, 250, 700, 12, 0xFF00A3FF, 6.0f);
        add_text(obs_text_std("OBSTitles.LayerMainTitle"), name, 960, 480, 96, 0xFFFFFFFF, true, 1, 1);
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateEditableSubtitle"), 960, 610, 48, 0xFFE0E0E0, false, 1, 1);
        break;
    }
    default: {
        add_text(obs_text_std("OBSTitles.TemplateTitleText"), name, 960, 540, 72, 0xFFFFFFFF, true, 1, 1);
        break;
    }
    }

    for (auto &layer : title->layers)
        layer->out_time = title->duration;

    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    return title;
}

void TitleDock::create_title_from_template(const std::string &default_name,
                                           int template_id)
{
    bool ok = false;
    QString name = QInputDialog::getText(
        this, obsgs_tr("OBSTitles.NewTemplateTitle"), obsgs_tr("OBSTitles.TitleTextPrompt"), QLineEdit::Normal,
        QString::fromStdString(default_name), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    auto title = create_template_title(name.trimmed().toStdString(), template_id);
    select_title(title->id);
    on_edit();
}

/* ══════════════════════════════════════════════════════════════════
 *  Actions
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::on_add()
{
    bool ok;
    QString name = QInputDialog::getText(
        this, obsgs_tr("OBSTitles.NewTitle"), obsgs_tr("OBSTitles.TitleNamePrompt"), QLineEdit::Normal, obsgs_tr("OBSTitles.NewTitle"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    auto title = TitleDataStore::instance().create_title(name.trimmed().toStdString());
    title->preview_screenshot_png_base64 = title_screenshot_png_base64(title_screenshot_image(*title)).toStdString();
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    select_title(title->id);
    on_edit();
}


void TitleDock::on_add_from_templates_library()
{
    const QString root_path = template_library_root_path();
    for (const char *folder : template_library_category_folders)
        QDir(root_path).mkpath(QString::fromUtf8(folder));

    for (const auto &entry : template_library_entries) {
        const QString category_path = QDir(root_path).filePath(QString::fromUtf8(entry.category_folder));
        const QString template_path = QDir(category_path).filePath(QString::fromUtf8(entry.file_name));
        if (QFileInfo::exists(template_path))
            continue;

        auto canned = create_template_title(obs_text_std(entry.default_name_key), entry.id);
        TitleTemplateExportMetadata metadata;
        metadata.title = obs_text_std(entry.name_key);
        metadata.description = obs_text_std(entry.description_key);
        metadata.creator = "OBS Graphics Studio Pro";
        metadata.creation_date = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();

        std::string error;
        TitleDataStore::instance().export_title(canned->id, template_path.toStdString(), metadata, &error);
        TitleDataStore::instance().delete_title(canned->id);
        TitleDataStore::instance().save();
    }
    refresh();

    auto *window = new QDialog(this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(obsgs_tr("OBSTitles.TemplatesLibrary"));
    window->setModal(false);
    window->resize(900, 520);

    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(window));

    auto *intro = new QLabel(obsgs_tr("OBSTitles.TemplatesLibraryPrompt"), window);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *splitter = new QSplitter(Qt::Horizontal, window);
    auto *categories = new TemplateCategoryTree(splitter);
    categories->setMinimumWidth(180);
    categories->setAlternatingRowColors(true);
    populate_template_categories(categories, root_path);

    auto *templates = new TemplateListWidget(splitter);
    templates->setViewMode(QListView::IconMode);
    templates->setIconSize(QSize(120, 72));
    templates->setResizeMode(QListView::Adjust);
    templates->setMovement(QListView::Static);
    templates->setSelectionMode(QAbstractItemView::SingleSelection);
    templates->setSpacing(8);

    auto *metadata_panel = new QWidget(splitter);
    auto *metadata_layout = new QVBoxLayout(metadata_panel);
    metadata_layout->setContentsMargins(0, 0, 0, 0);
    auto *metadata_title = new QLabel(obsgs_tr("OBSTitles.TemplateMetadata"), metadata_panel);
    set_bold_label(metadata_title);
    metadata_layout->addWidget(metadata_title);
    auto *preview = new QLabel(metadata_panel);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(140);
    metadata_layout->addWidget(preview);
    auto *details = new QLabel(metadata_panel);
    details->setWordWrap(true);
    details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    metadata_layout->addWidget(details, 1);

    splitter->addWidget(categories);
    splitter->addWidget(templates);
    splitter->addWidget(metadata_panel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    layout->addWidget(splitter, 1);

    auto load_templates_for_category = [templates](const QString &dir_path) {
        templates->clear();
        QDir dir(dir_path);
        const QStringList filters = template_file_filter().split(' ', Qt::SkipEmptyParts);
        for (const QFileInfo &file : dir.entryInfoList(filters, QDir::Files, QDir::Name)) {
            TemplateFileMetadata metadata = read_template_file_metadata(file.absoluteFilePath());
            auto *item = new QListWidgetItem(metadata.screenshot_icon, metadata.title);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
            item->setData(Qt::UserRole, file.absoluteFilePath());
            item->setToolTip(metadata.description);
            templates->addItem(item);
        }
    };
    auto reload_current_category = [categories, load_templates_for_category, templates]() {
        if (categories->currentItem())
            load_templates_for_category(categories->currentItem()->data(0, Qt::UserRole).toString());
        if (templates->count() > 0)
            templates->setCurrentRow(0);
    };
    categories->templates_moved = reload_current_category;

    auto update_metadata = [templates, preview, details]() {
        auto *item = templates->currentItem();
        if (!item) {
            preview->clear();
            details->setText(obsgs_tr("OBSTitles.TemplateNoSelection"));
            return;
        }

        TemplateFileMetadata metadata = read_template_file_metadata(item->data(Qt::UserRole).toString());
        if (!metadata.screenshot_pixmap.isNull()) {
            preview->setPixmap(metadata.screenshot_pixmap.scaled(
                QSize(220, 140), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
        }
        details->setText(
            QStringLiteral("<b>%1</b><br><br>%2<br><br><b>%3</b> %4<br><b>%5</b> %6")
                .arg(metadata.title.toHtmlEscaped(),
                     metadata.description.toHtmlEscaped().replace('\n', QStringLiteral("<br>")),
                     obsgs_tr("OBSTitles.TemplateCreatorLabel"), metadata.creator.toHtmlEscaped(),
                     obsgs_tr("OBSTitles.TemplateCreationDateLabel"), metadata.creation_date.toHtmlEscaped()));
    };

    QObject::connect(categories, &QTreeWidget::currentItemChanged, window,
                     [load_templates_for_category, update_metadata](QTreeWidgetItem *current, QTreeWidgetItem *) {
        if (!current) return;
        load_templates_for_category(current->data(0, Qt::UserRole).toString());
        update_metadata();
    });
    QObject::connect(categories, &QTreeWidget::itemDoubleClicked, window,
                     [window, categories, root_path, reload_current_category](QTreeWidgetItem *item, int column) {
        if (!item || column != 0) return;
        if (rename_template_category(window, item)) {
            populate_template_categories(categories, root_path);
            reload_current_category();
        }
    });
    categories->category_delete_requested = [window, categories, root_path, reload_current_category](QTreeWidgetItem *item) {
        if (delete_template_category(window, item, root_path)) {
            populate_template_categories(categories, root_path);
            reload_current_category();
        }
    };
    QObject::connect(templates, &QListWidget::currentItemChanged, window,
                     [update_metadata](QListWidgetItem *, QListWidgetItem *) { update_metadata(); });

    auto import_selected_template = [this, window, templates]() {
        auto *selected = templates->currentItem();
        if (!selected)
            return;

        std::string error;
        auto imported = TitleDataStore::instance().import_title(selected->data(Qt::UserRole).toString().toStdString(), &error);
        if (!imported) {
            QMessageBox::warning(window, obsgs_tr("OBSTitles.ImportTitleTemplate"), QString::fromStdString(error));
            return;
        }
        select_title(imported->id);
        status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QString::fromStdString(imported->name)));
        window->close();
    };
    QObject::connect(templates, &QListWidget::itemDoubleClicked, window,
                     [import_selected_template](QListWidgetItem *) { import_selected_template(); });

    auto *template_actions = new QHBoxLayout();
    template_actions->setContentsMargins(0, 0, 0, 0);
    auto *edit_template = new QPushButton(obsgs_tr("OBSTitles.Edit"), window);
    auto *delete_template = new QPushButton(obsgs_tr("OBSTitles.Delete"), window);
    template_actions->addWidget(edit_template);
    template_actions->addWidget(delete_template);
    template_actions->addStretch();
    layout->addLayout(template_actions);

    QObject::connect(edit_template, &QPushButton::clicked, window, [window, templates, reload_current_category, update_metadata]() {
        auto *selected = templates->currentItem();
        if (!selected) return;
        if (prompt_edit_template_file_metadata(window, selected->data(Qt::UserRole).toString())) {
            reload_current_category();
            update_metadata();
        }
    });
    QObject::connect(delete_template, &QPushButton::clicked, window, [window, templates, reload_current_category, update_metadata]() {
        auto *selected = templates->currentItem();
        if (!selected) return;
        const QString path = selected->data(Qt::UserRole).toString();
        const auto reply = QMessageBox::question(
            window, obsgs_tr("OBSTitles.DeleteTemplate"),
            obsgs_tr("OBSTitles.DeleteTemplateQuestionFormat").arg(selected->text()),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        if (!QFile::remove(path)) {
            QMessageBox::warning(window, obsgs_tr("OBSTitles.DeleteTemplate"),
                                 obsgs_tr("OBSTitles.DeleteTemplateFailed"));
            return;
        }
        reload_current_category();
        update_metadata();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, window);
    QObject::connect(buttons, &QDialogButtonBox::accepted, window, import_selected_template);
    QObject::connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::close);
    layout->addWidget(buttons);

    reload_current_category();
    update_metadata();

    window->show();
    window->raise();
    window->activateWindow();
}

void TitleDock::on_duplicate()
{
    auto src = TitleDataStore::instance().get_title(selected_id());
    if (!src) return;

    /* Deep copy by round-tripping through data store */
    auto dup = TitleDataStore::instance().create_title(src->name + obs_text_std("OBSTitles.CopySuffix"));
    dup->description = src->description;
    dup->creator = src->creator;
    dup->creation_date = src->creation_date;
    dup->duration  = src->duration;
    dup->bg_color  = src->bg_color;
    dup->width     = src->width;
    dup->height    = src->height;

    dup->layers.clear();
    for (auto &l : src->layers) {
        auto nl = std::make_shared<Layer>(*l);
        nl->id = TitleDataStore::make_uuid();
        dup->layers.push_back(nl);
    }
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    select_title(dup->id);
}

void TitleDock::on_rename()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    QImage screenshot = title_screenshot_image(*title);
    const QString screenshot_base64 = title_screenshot_png_base64(screenshot);
    if (!screenshot_base64.isEmpty())
        title->preview_screenshot_png_base64 = screenshot_base64.toStdString();

    TitleTemplateExportMetadata metadata;
    if (!prompt_template_metadata(this, *title, screenshot, metadata, nullptr,
                                  obsgs_tr("OBSTitles.EditTemplateMetadata")))
        return;

    title->name = metadata.title;
    title->description = metadata.description;
    title->creator = metadata.creator;
    title->creation_date = metadata.creation_date;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    select_title(title->id);
}

void TitleDock::on_export()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    QImage screenshot = title_screenshot_image(*title);
    QString screenshot_base64 = title_screenshot_png_base64(screenshot);
    if (screenshot_base64.isEmpty()) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                             obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
        return;
    }

    title->preview_screenshot_png_base64 = screenshot_base64.toStdString();
    TitleDataStore::instance().save();

    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = title->preview_screenshot_png_base64;
    bool save_in_template_library = false;
    if (!prompt_template_metadata(this, *title, screenshot, metadata, &save_in_template_library))
        return;

    QString safe_name = QString::fromStdString(metadata.title).trimmed();
    if (safe_name.isEmpty()) safe_name = obsgs_tr("OBSTitles.TemplateFileDialogTitle");
    safe_name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));

    QString path;
    if (save_in_template_library) {
        QString category;
        if (!prompt_template_library_category(this, category))
            return;
        const QString category_path = QDir(template_library_root_path()).filePath(category);
        QDir().mkpath(category_path);
        path = QDir(category_path).filePath(safe_name + QStringLiteral(".ogspt"));
    } else {
        path = QFileDialog::getSaveFileName(
            this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
            QDir(template_library_root_path()).filePath(safe_name + QStringLiteral(".ogspt")),
            obsgs_tr("OBSTitles.TemplateFileFilter"));
        if (path.isEmpty()) return;

        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".ogspt");
    }

    std::string error;
    if (!TitleDataStore::instance().export_title(title->id, path.toStdString(), metadata, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                             QString::fromStdString(error));
        return;
    }

    status_lbl_->setText(obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::import_title_paths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    int imported_count = 0;
    QStringList failures;
    std::shared_ptr<Title> last_imported;

    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile() || !info.isReadable())
            continue;
        const QString suffix = info.suffix().toLower();
        if (suffix != QStringLiteral("ogspt") && suffix != QStringLiteral("otpt") && suffix != QStringLiteral("json"))
            continue;

        std::string error;
        auto imported = TitleDataStore::instance().import_title(info.absoluteFilePath().toStdString(), &error);
        if (!imported) {
            const QString message = error.empty() ? obsgs_tr("OBSTitles.ImportFailed") : QString::fromStdString(error);
            failures.push_back(QStringLiteral("%1 — %2").arg(info.fileName(), message));
            continue;
        }

        last_imported = imported;
        ++imported_count;
    }

    if (imported_count > 0)
        refresh();

    if (last_imported)
        select_title(last_imported->id);

    if (imported_count == 1 && last_imported) {
        status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QString::fromStdString(last_imported->name)));
    } else if (imported_count > 1) {
        status_lbl_->setText(obsgs_tr("OBSTitles.ImportedTitleTemplates").arg(imported_count));
    }

    if (!failures.isEmpty()) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportTitleTemplate"),
                             obsgs_tr("OBSTitles.SomeTemplatesImportFailed")
                                 .arg(failures.join(QStringLiteral("\n"))));
    } else if (imported_count == 0) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportTitleTemplate"),
                             obsgs_tr("OBSTitles.NoSupportedTemplatesImported"));
    }
}

void TitleDock::on_import()
{
    QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ImportTitleTemplate"), QString(),
        obsgs_tr("OBSTitles.TemplateFileFilter"));
    if (path.isEmpty()) return;

    import_title_paths(QStringList{path});
}

void TitleDock::on_delete()
{
    const auto ids = selected_title_ids();
    if (ids.empty()) return;

    int target_row = -1;
    if (list_) {
        int first_selected_row = list_->count();
        for (int i = 0; i < list_->count(); ++i) {
            auto *item = list_->item(i);
            if (item && item->isSelected())
                first_selected_row = std::min(first_selected_row, i);
        }
        if (first_selected_row < list_->count())
            target_row = first_selected_row;
    }

    DeleteTitleConfirmationResult confirmation;
    if (ids.size() == 1) {
        auto title = TitleDataStore::instance().get_title(ids.front());
        if (!title) return;
        confirmation = confirm_delete_single_title(this, *title);
    } else {
        confirmation = confirm_delete_multiple_titles(this, (int)ids.size());
    }

    if (confirmation.button == QMessageBox::Yes) {
        if (confirmation.delete_sources)
            delete_title_sources_for_ids(ids);

        for (const auto &id : ids) {
            CacheManager::instance().removeTitleCache(QString::fromStdString(id), false);
            TitleDataStore::instance().delete_title(id);
        }
        TitleDataStore::instance().save();
        populate_list();
        if (list_ && list_->count() > 0)
            list_->setCurrentRow(std::clamp(target_row, 0, list_->count() - 1));
        else if (list_)
            list_->clearSelection();
    }
}

void TitleDock::on_edit()
{
    std::string id = selected_id();
    if (id.empty()) return;

    if (!editor_) {
        editor_ = new TitleEditor(
            static_cast<QWidget *>(obs_frontend_get_main_window()));
        editor_->setAttribute(Qt::WA_DeleteOnClose);
        connect(editor_, &QObject::destroyed,
                this, [this]() { editor_ = nullptr; });
        connect(editor_, &TitleEditor::title_saved,
                this, [this](const std::string &) { refresh(); });
    }

    editor_->open_title(id);
    editor_->show();
    editor_->raise();
    editor_->activateWindow();
}

void TitleDock::on_add_to_scene()
{
    std::string id = selected_id();
    if (id.empty()) return;

    auto t = TitleDataStore::instance().get_title(id);
    if (!t) return;

    obs_source_t *scene_source = obs_frontend_get_current_scene();
    if (!scene_source) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.NoScene"),
                             obsgs_tr("OBSTitles.NoActiveScene"));
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(scene_source);
    if (!scene) {
        obs_source_release(scene_source);
        return;
    }

    /* Create the source */
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, PROP_TITLE_ID, id.c_str());

    obs_source_t *source = obs_source_create(
        "obs_graphics_studio_pro_source",
        t->name.c_str(),
        settings,
        nullptr);

    if (source) {
        obs_sceneitem_t *item = obs_scene_add(scene, source);
        if (item) {
            struct vec2 pos = {0.0f, 0.0f};
            obs_sceneitem_set_pos(item, &pos);
            obs_sceneitem_set_visible(item, true);
        }
        obs_source_release(source);
        status_lbl_->setText(obsgs_tr("OBSTitles.AddedToScene"));
    } else {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.AddTitleSource"),
                             obsgs_tr("OBSTitles.CreateSourceFailed"));
    }

    obs_data_release(settings);
    obs_source_release(scene_source);
}
