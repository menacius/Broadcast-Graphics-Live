#include "title-editor-internal.h"

ToolsSidebar::ToolsSidebar(QWidget *parent) : QWidget(parent)
{
    constexpr int kSidebarIconSize = 22;
    constexpr int kSidebarButtonSize = 36;
    const QPalette pal = qApp->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);

    setObjectName(QStringLiteral("OBSGraphicsStudioProToolsSidebarPanel"));
    setMinimumWidth(kSidebarButtonSize + 8);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setStyleSheet(QStringLiteral(
        "QWidget#OBSGraphicsStudioProToolsSidebarPanel{background:%1;color:%2;}"
        "QToolButton{color:%3;background:%4;border:1px solid transparent;border-radius:3px;padding:0;"
        "min-width:%5px;min-height:%5px;max-width:%5px;max-height:%5px;}"
        "QToolButton:hover{background:%6;border-color:%7;}"
        "QToolButton:checked{background:%8;color:%9;border-color:%8;}"
        "QToolButton::menu-indicator{image:none;width:0px;}"
        "QMenu{color:%10;background:%11;border:1px solid %7;}"
        "QMenu::item{padding:5px 22px;}"
        "QMenu::item:selected{background:%8;color:%9;}")
        .arg(window.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             button_text.name(QColor::HexRgb),
             button.name(QColor::HexRgb),
             QString::number(kSidebarButtonSize),
             hover.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb),
             pal.color(QPalette::Text).name(QColor::HexRgb),
             base.name(QColor::HexRgb)));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 6, 4, 6);
    layout->setSpacing(4);

    tool_group_ = new QActionGroup(this);
    tool_group_->setExclusive(true);

    auto make_tool_button = [this, layout](const QString &text, const QIcon &icon, const QString &tip) {
        auto *button = new HoldMenuToolButton(this);
        button->setText(text);
        button->setAccessibleName(text);
        button->setToolTip(tip);
        button->setIcon(icon);
        button->setIconSize(QSize(kSidebarIconSize, kSidebarIconSize));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setCheckable(true);
        button->setAutoRaise(false);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setFixedSize(kSidebarButtonSize, kSidebarButtonSize);
        layout->addWidget(button, 0, Qt::AlignHCenter);
        return button;
    };

    selection_button_ = make_tool_button(QStringLiteral("Selection Tool"), cursor_tool_icon(),
                                         QStringLiteral("Selection/Cursor Tool: normal object selection mode"));
    shape_button_ = make_tool_button(QStringLiteral("Shape Tool"), shape_tool_icon(selected_shape_),
                                     QStringLiteral("Shape Tool: choose a shape, then click and drag on the canvas"));
    text_button_ = make_tool_button(QStringLiteral("Text Tool"), text_tool_icon(selected_text_layer_type_),
                                    QStringLiteral("Text Tool: choose text, clock, or ticker, then click and drag on the canvas"));
    color_picker_button_ = make_tool_button(QStringLiteral("Color Picker Tool"), obs_icon("eyedropper.svg"),
                                            QStringLiteral("Color Picker Tool: sample a color from the canvas into the selected layer"));

    auto *selection_action = new QAction(cursor_tool_icon(), QStringLiteral("Selection Tool"), this);
    selection_action->setCheckable(true);
    selection_action->setChecked(true);
    auto *shape_action = new QAction(shape_tool_icon(selected_shape_), QStringLiteral("Shape Tool"), this);
    shape_action->setCheckable(true);
    auto *text_action = new QAction(text_tool_icon(selected_text_layer_type_), QStringLiteral("Text Tool"), this);
    text_action->setCheckable(true);
    auto *color_picker_action = new QAction(obs_icon("eyedropper.svg"), QStringLiteral("Color Picker Tool"), this);
    color_picker_action->setCheckable(true);
    tool_group_->addAction(selection_action);
    tool_group_->addAction(shape_action);
    tool_group_->addAction(text_action);
    tool_group_->addAction(color_picker_action);
    selection_button_->setDefaultAction(selection_action);
    shape_button_->setDefaultAction(shape_action);
    text_button_->setDefaultAction(text_action);
    color_picker_button_->setDefaultAction(color_picker_action);

    shape_menu_ = new QMenu(shape_button_);
    shape_button_->setMenu(shape_menu_);
    rebuild_shape_menu();
    text_menu_ = new QMenu(text_button_);
    text_button_->setMenu(text_menu_);
    rebuild_text_menu();

    connect(selection_action, &QAction::triggered, this, [this]() {
        emit selection_tool_requested();
    });
    connect(shape_action, &QAction::triggered, this, [this]() {
        emit shape_tool_requested(selected_shape_);
    });
    connect(text_action, &QAction::triggered, this, [this]() {
        emit text_tool_requested(selected_text_layer_type_);
    });
    connect(color_picker_action, &QAction::triggered, this, [this]() {
        emit color_picker_tool_requested();
    });

    layout->addStretch(1);
}

void ToolsSidebar::set_selected_shape(ShapeType shape_type)
{
    selected_shape_ = shape_type;
    const QIcon icon = shape_tool_icon(shape_type);
    if (shape_button_) {
        shape_button_->setIcon(icon);
        if (auto *action = shape_button_->defaultAction()) {
            action->setIcon(icon);
            action->setText(shape_display_name(shape_type));
            action->setChecked(true);
        }
        shape_button_->setToolTip(QStringLiteral("Shape Tool: %1. Click and drag on the canvas to draw.").arg(shape_display_name(shape_type)));
        shape_button_->setChecked(true);
    }
}

void ToolsSidebar::rebuild_shape_menu()
{
    if (!shape_menu_) return;
    shape_menu_->clear();
    const std::vector<ShapeType> shapes = {
        ShapeType::Rectangle,
        ShapeType::RoundedRectangle,
        ShapeType::Ellipse,
        ShapeType::Triangle,
        ShapeType::Star,
        ShapeType::Polygon,
        ShapeType::Diamond,
        ShapeType::Line,
    };
    for (ShapeType shape : shapes) {
        QAction *action = shape_menu_->addAction(shape_tool_icon(shape), shape_display_name(shape));
        connect(action, &QAction::triggered, this, [this, shape]() {
            set_selected_shape(shape);
            emit shape_tool_requested(shape);
        });
    }
}


void ToolsSidebar::set_selected_text_layer_type(LayerType type)
{
    if (type != LayerType::Text && type != LayerType::Clock && type != LayerType::Ticker)
        type = LayerType::Text;
    selected_text_layer_type_ = type;
    const QIcon icon = text_tool_icon(type);
    const QString name = text_tool_display_name(type);
    if (text_button_) {
        text_button_->setIcon(icon);
        if (auto *action = text_button_->defaultAction()) {
            action->setIcon(icon);
            action->setText(name);
            action->setChecked(true);
        }
        text_button_->setToolTip(QStringLiteral("Text Tool: %1. Click and drag on the canvas to draw a text box.").arg(name));
        text_button_->setChecked(true);
    }
}

void ToolsSidebar::rebuild_text_menu()
{
    if (!text_menu_) return;
    text_menu_->clear();
    const std::vector<LayerType> types = {LayerType::Text, LayerType::Clock, LayerType::Ticker};
    for (LayerType type : types) {
        QAction *action = text_menu_->addAction(text_tool_icon(type), text_tool_display_name(type));
        connect(action, &QAction::triggered, this, [this, type]() {
            set_selected_text_layer_type(type);
            emit text_tool_requested(type);
        });
    }
}


