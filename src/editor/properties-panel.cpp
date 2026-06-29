#include "title-editor-internal.h"
#include "bgl-modern-controls.h"
#include "title-logger.h"

#include <memory>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QEventLoop>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QTimer>


#include "open-color-palette.h"

/* Ordered implementation modules. Keep this list in source order. */
#include "properties-panel/popup-state.inc"
#include "properties-panel/construction-gradient-image-signals.inc"
#include "properties-panel/construction-transform-character.inc"
#include "properties-panel/construction-type-live-shape.inc"
#include "properties-panel/color-gradient-editing.inc"
#include "properties-panel/auto-style-and-property-actions.inc"
#include "properties-panel/property-synchronization.inc"
#include "properties-panel/selection-refresh.inc"
