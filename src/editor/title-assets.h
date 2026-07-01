#pragma once

#include <obs-module.h>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QIcon>
#include <QIODevice>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>
#include <QWidget>

static inline QColor bgl_icon_color()
{
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    const QColor button_text = palette.color(QPalette::Active, QPalette::ButtonText);
    return button_text.isValid() ? button_text : QColor(0x20, 0x20, 0x20);
}

static inline QIcon bgl_icon(const char *file_name, const QColor &color)
{
    QString rel = QStringLiteral("icons/") + QString::fromUtf8(file_name);
    char *path = obs_module_file(rel.toUtf8().constData());
    if (!path)
        return QIcon();

    const QString icon_path = QString::fromUtf8(path);
    bfree(path);

    QFile file(icon_path);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon(icon_path);

    QByteArray svg = file.readAll();
    svg.replace("currentColor", color.name(QColor::HexRgb).toUtf8());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return QIcon(icon_path);

    QIcon icon;
    const int sizes[] = {16, 20, 24, 32};
    for (int size : sizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        icon.addPixmap(pixmap);
    }
    return icon;
}

static inline QIcon bgl_icon(const char *file_name)
{
    return bgl_icon(file_name, bgl_icon_color());
}

static inline QIcon bgl_brand_icon()
{
    char *path = obs_module_file("icons/broadcast-graphics-live-app-icon.png");
    if (path) {
        const QString icon_path = QString::fromUtf8(path);
        bfree(path);

        QPixmap source(icon_path);
        if (!source.isNull()) {
            QIcon icon;
            const int sizes[] = {16, 20, 24, 32, 48, 64, 96, 128, 256};
            for (int size : sizes)
                icon.addPixmap(source.scaled(size, size, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
            return icon;
        }

        return QIcon(icon_path);
    }

    path = obs_module_file("icons/broadcast-graphics-live-app-icon.svg");
    if (!path)
        return QIcon();

    const QString icon_path = QString::fromUtf8(path);
    bfree(path);

    QFile file(icon_path);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon(icon_path);

    const QByteArray svg = file.readAll();
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return QIcon(icon_path);

    QIcon icon;
    const int sizes[] = {16, 20, 24, 32, 48, 64, 96, 128, 256};
    for (int size : sizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter, pixmap.rect());
        icon.addPixmap(pixmap);
    }
    return icon;
}

static inline void bgl_apply_brand_icon(QWidget *window)
{
    if (window)
        window->setWindowIcon(bgl_brand_icon());
}

