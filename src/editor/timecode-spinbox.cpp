#include "timecode-spinbox.h"

#include <obs-module.h>

#include <QLineEdit>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <cmath>

TimecodeSpinBox::TimecodeSpinBox(QWidget *parent)
    : QDoubleSpinBox(parent)
{
    setDecimals(3);
    setSingleStep(frame_duration());
    setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    setKeyboardTracking(false);
    setToolTip(QStringLiteral("Enter timecode as HH:MM:SS:FF."));
    lineEdit()->setPlaceholderText(QStringLiteral("00:00:00:00"));
}

double TimecodeSpinBox::frame_rate()
{
    struct obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.fps_den > 0 && ovi.fps_num > 0)
        return (double)ovi.fps_num / (double)ovi.fps_den;
    return 30.0;
}

double TimecodeSpinBox::frame_duration()
{
    return 1.0 / std::max(1.0, frame_rate());
}

int TimecodeSpinBox::rounded_fps()
{
    return std::max(1, (int)std::round(frame_rate()));
}

QString TimecodeSpinBox::format_seconds(double seconds)
{
    const double fps_d = frame_rate();
    const int fps = rounded_fps();
    const int total_frames = std::max(0, (int)std::round(seconds * fps_d));
    const int frames = total_frames % fps;
    const int total_seconds = total_frames / fps;
    const int display_seconds = total_seconds % 60;
    const int minutes = (total_seconds / 60) % 60;
    const int hours = total_seconds / 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(display_seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

bool TimecodeSpinBox::parse_timecode(const QString &text, double *seconds_out)
{
    QString clean = text.trimmed();
    if (clean.endsWith(QLatin1Char('s'), Qt::CaseInsensitive))
        clean.chop(1);
    clean = clean.trimmed();
    if (clean.isEmpty())
        return false;

    bool ok = false;
    const double decimal_seconds = QLocale::c().toDouble(clean, &ok);
    if (ok) {
        if (seconds_out) *seconds_out = decimal_seconds;
        return std::isfinite(decimal_seconds);
    }

    const QStringList parts = clean.split(QLatin1Char(':'));
    if (parts.size() < 2 || parts.size() > 4)
        return false;

    int values[4] = {0, 0, 0, 0};
    const int offset = 4 - parts.size();
    for (int i = 0; i < parts.size(); ++i) {
        bool part_ok = false;
        values[offset + i] = parts[i].toInt(&part_ok);
        if (!part_ok || values[offset + i] < 0)
            return false;
    }

    const int fps = rounded_fps();
    if (values[1] >= 60 || values[2] >= 60 || values[3] >= fps)
        return false;

    const double parsed_seconds = values[0] * 3600.0 + values[1] * 60.0 +
                                  values[2] + values[3] / frame_rate();
    if (seconds_out) *seconds_out = parsed_seconds;
    return std::isfinite(parsed_seconds);
}

QString TimecodeSpinBox::textFromValue(double value) const
{
    return format_seconds(value);
}

double TimecodeSpinBox::valueFromText(const QString &text) const
{
    double seconds = value();
    if (parse_timecode(text, &seconds))
        return seconds;
    return value();
}

QValidator::State TimecodeSpinBox::validate(QString &text, int &pos) const
{
    Q_UNUSED(pos);
    double seconds = 0.0;
    if (parse_timecode(text, &seconds) && seconds >= minimum() && seconds <= maximum())
        return QValidator::Acceptable;
    if (text.trimmed().isEmpty())
        return QValidator::Intermediate;
    return QValidator::Intermediate;
}

void TimecodeSpinBox::stepBy(int steps)
{
    const double stepped = value() + steps * singleStep();
    setValue(std::clamp(stepped, minimum(), maximum()));
}
