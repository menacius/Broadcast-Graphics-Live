#include "title-editor-internal.h"
#include "text-animator-presets.h"
#include "text-animator-preset-io.h"
#include "external-data-binding-dialog.h"
#include "external-data.h"
#include "bgl-modern-controls.h"
#include "title-logger.h"

#include <memory>
#include <algorithm>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QDataStream>
#include <QBuffer>
#include <QToolButton>
#include <QDir>
#include <QEventLoop>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QTimer>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSplitter>


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
/* Full member-function definitions must be included only after the constructor
 * and the legacy ordered implementation chain have returned to file scope. */
#include "properties-panel/text-animator-controls.inc"
