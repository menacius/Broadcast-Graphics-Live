#pragma once

#include "layer-transition.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QWidget;

class TransitionEditorDialog final : public QDialog {
public:
    explicit TransitionEditorDialog(const LayerTransition &transition,
                                    double maximum_duration,
                                    QWidget *parent = nullptr);

    LayerTransition transition() const;

private:
    void update_model_from_controls();
    void update_control_visibility();

    LayerTransition transition_;
    QWidget *preview_ = nullptr;
    QCheckBox *enabled_ = nullptr;
    QDoubleSpinBox *duration_ = nullptr;
    QComboBox *easing_ = nullptr;
    QComboBox *unit_ = nullptr;
    QComboBox *direction_ = nullptr;
    QDoubleSpinBox *stagger_ = nullptr;
    QDoubleSpinBox *blur_ = nullptr;
    QDoubleSpinBox *scale_ = nullptr;
    QDoubleSpinBox *offset_ = nullptr;
    QDoubleSpinBox *softness_ = nullptr;
    QCheckBox *reverse_order_ = nullptr;
    QLabel *unit_label_ = nullptr;
    QLabel *direction_label_ = nullptr;
    QLabel *stagger_label_ = nullptr;
    QLabel *blur_label_ = nullptr;
    QLabel *scale_label_ = nullptr;
    QLabel *offset_label_ = nullptr;
    QLabel *softness_label_ = nullptr;
};
