#include "system-memory.h"
#include "external-data.h"
#include "bgl-modern-controls.h"
#include "title-editor-internal.h"
#include "title-logger.h"
#include "build-info.h"
#include "style-presets.h"
#include "transition-editor-dialog.h"
#include "transition-preset-catalog.h"
#include "text-animator-presets.h"

#include <QPointer>
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
#include <QList>
#include <QDir>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QSet>
#include <QDirIterator>
#include <QDateTime>
#include <QStatusBar>

#include <cmath>


#include "long-press-tool-button.h"

#include "open-color-palette.h"

/* Ordered implementation modules. Keep this list in source order. */
#include "title-editor/window-session.inc"
#include "title-editor/ui-construction.inc"
#include "title-editor/panels-colors.inc"
#include "title-editor/commands-docks.inc"
#include "title-editor/document-shape-editing.inc"
#include "title-editor/playback-cache-preferences.inc"
#include "title-editor/layout-template-tools.inc"
#include "title-editor/signal-handlers.inc"
#include "title-editor/editor-events.inc"
