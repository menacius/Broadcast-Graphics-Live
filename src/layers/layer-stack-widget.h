#pragma once

#include "title-data.h"
#include "title-rich-text.h"

#include <QWidget>
#include <QGroupBox>
#include <QScrollArea>
#include <QListWidget>
#include <QToolButton>
#include <QActionGroup>
#include <QButtonGroup>
#include <QMenu>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTextEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QPointF>
#include <QPoint>
#include <QRectF>
#include <QColor>
#include <QPixmap>
#include <QElapsedTimer>
#include <memory>
#include <string>
#include <vector>
#include <set>

class QEvent;
class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QContextMenuEvent;
class QResizeEvent;
class QPaintEvent;
class QPainter;
class QScrollBar;
/* ══════════════════════════════════════════════════════════════════
 *  LayerStack  – AE-style layer list on the left of the timeline
 * ══════════════════════════════════════════════════════════════════ */
class LayerStack : public QWidget {
    Q_OBJECT

public:
    explicit LayerStack(QWidget *parent = nullptr);

    void set_title(std::shared_ptr<Title> t);
    void refresh();
    void set_selected_layer(const std::string &layer_id);
    void set_selected_layers(const std::vector<std::string> &layer_ids);
    void set_layer_clipboard_available(bool available);
    QScrollBar *vertical_scroll_bar() const;
    std::vector<std::string> selected_ids() const;

signals:
    void layer_selected(const std::string &layer_id);
    void layers_selected(const std::vector<std::string> &layer_ids);
    void layer_visibility_changed(const std::string &layer_id, bool v);
    void layer_lock_changed(const std::string &layer_id, bool locked);
    void layer_expand_changed(const std::string &layer_id, bool expanded);
    void layer_parent_changed(const std::string &layer_id, const std::string &parent_id);
    void layer_mask_changed(const std::string &layer_id, const std::string &mask_source_id, MaskMode mask_mode);
    void layer_name_changed(const std::string &layer_id, const std::string &name);
    void layer_order_changed();
    void add_layer_requested(LayerType type);
    void clone_layer_requested(const std::string &layer_id);
    void copy_layer_requested(const std::string &layer_id);
    void paste_layer_requested(const std::string &layer_id);
    void delete_layer_requested(const std::string &layer_id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_add_text();
    void on_add_clock();
    void on_add_ticker();
    void on_add_rect();
    void on_add_image();
    void on_move_up();
    void on_move_down();
    void on_delete();
    void on_item_changed(QListWidgetItem *item);
    void on_selection_changed();
    void show_layer_context_menu(const QPoint &pos);

private:
    void populate();
    void sync_order_from_list();
    std::string selected_id() const;

    std::shared_ptr<Title> title_;
    QListWidget  *list_     = nullptr;
    QToolButton  *btn_add_  = nullptr;
    QToolButton  *btn_move_up_ = nullptr;
    QToolButton  *btn_move_down_ = nullptr;
    QToolButton  *btn_del_       = nullptr;
    bool          layer_clipboard_available_ = false;
};


