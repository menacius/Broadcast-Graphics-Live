#pragma once

#include <QDoubleSpinBox>
#include <QString>
#include <QValidator>

class TimecodeSpinBox : public QDoubleSpinBox {
public:
    explicit TimecodeSpinBox(QWidget *parent = nullptr);

    static QString format_seconds(double seconds);
    static bool parse_timecode(const QString &text, double *seconds_out);

protected:
    QString textFromValue(double value) const override;
    double valueFromText(const QString &text) const override;
    QValidator::State validate(QString &text, int &pos) const override;
    void stepBy(int steps) override;

private:
    static double frame_rate();
    static double frame_duration();
    static int rounded_fps();
};
