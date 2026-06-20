/*
 * OBS Graphics Studio Pro Plugin - plugin-main.cpp
 * Entry point: registers the source, dock, and module lifecycle.
 */

#include "plugin-main.h"
#include "title-source.h"
#include "title-dock.h"
#include "title-hotkeys.h"
#include "title-editor.h"
#include "title-data.h"
#include "title-localization.h"
#include "title-logger.h"
#include "title-preferences.h"
#include "cache-manager.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QMainWindow>
#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSignalBlocker>
#include <QString>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* ── forward declarations ───────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void *priv);

/* ── module globals ─────────────────────────────────────────────── */
static TitleDock *g_dock = nullptr;
static QAction *g_dock_menu_action = nullptr;
static bool g_frontend_ready = false;

static void open_preferences_from_tools_menu(void *)
{
    QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    TitleEditor::show_global_preferences(main);
}


static QMenu *find_docks_menu(QMainWindow *main)
{
    if (!main || !main->menuBar()) return nullptr;
    for (auto *menu : main->menuBar()->findChildren<QMenu *>()) {
        QString title = menu->title();
        title.remove('&');
        if (title.compare(obsgs_tr("OBSTitles.DocksMenu"), Qt::CaseInsensitive) == 0)
            return menu;
    }
    return nullptr;
}

static void destroy_dock_ui()
{
    if (g_dock_menu_action) {
        QObject::disconnect(g_dock_menu_action, nullptr, nullptr, nullptr);
        if (QWidget *owner = qobject_cast<QWidget *>(g_dock_menu_action->parent()))
            owner->removeAction(g_dock_menu_action);
        delete g_dock_menu_action;
        g_dock_menu_action = nullptr;
    }

    if (g_dock) {
        QObject::disconnect(g_dock, nullptr, nullptr, nullptr);
        obs_frontend_remove_dock("obs-graphics-studio-pro-dock");
        delete g_dock;
        g_dock = nullptr;
    }
}

static void add_docks_menu_entry(QMainWindow *main)
{
    QMenu *docks_menu = find_docks_menu(main);
    if (!docks_menu || !g_dock || g_dock_menu_action) return;

    g_dock_menu_action = docks_menu->addAction(obsgs_tr("OBSTitles.DockName"));
    g_dock_menu_action->setObjectName("obs-graphics-studio-pro-docks-menu-action");
    g_dock_menu_action->setCheckable(true);
    g_dock_menu_action->setChecked(g_dock->isVisible());
    QObject::connect(g_dock_menu_action, &QAction::triggered, g_dock,
                     [](bool visible) { if (g_dock) g_dock->setVisible(visible); });
    QObject::connect(g_dock, &QDockWidget::visibilityChanged, g_dock_menu_action,
                     [](bool visible) {
                         if (!g_dock_menu_action) return;
                         QSignalBlocker blocker(g_dock_menu_action);
                         g_dock_menu_action->setChecked(visible);
                     });
}

/* ── module load ────────────────────────────────────────────────── */
bool obs_module_load(void)
{
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Loading plugin v%s", PLUGIN_VERSION);
    OGS_LOG_INFO("Plugin", QStringLiteral("Loading plugin version %1").arg(QStringLiteral(PLUGIN_VERSION)));

    /* 1. Initialise persistent title store */
    TitleDataStore::instance().load();

    /* 2. Register the renderable source type and title cue hotkeys */
    title_source_register();
    title_hotkeys_register();

    /* 3. Add global preferences entry and defer dock/hotkey creation until the OBS UI is ready */
    obs_frontend_add_tools_menu_item("OBS Graphics Studio Pro Preferences", open_preferences_from_tools_menu, nullptr);
    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    blog(LOG_INFO, "[OBS Graphics Studio Pro] Plugin loaded.");
    OGS_LOG_INFO("Plugin", QStringLiteral("Plugin loaded"));
    return true;
}

/* ── module unload ──────────────────────────────────────────────── */
void obs_module_unload(void)
{
    title_hotkeys_unregister();
    if (TitlePreferences::clear_cache_on_exit()) {
        OGS_LOG_INFO("Plugin", QStringLiteral("Clearing frame cache on module unload"));
        CacheManager::instance().clearAll();
    }
    TitleDataStore::instance().save();
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    destroy_dock_ui();
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Plugin unloaded.");
    OGS_LOG_INFO("Plugin", QStringLiteral("Plugin unloaded"));
}

/* ── frontend event handler ─────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void * /*priv*/)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        OGS_LOG_INFO("Plugin", QStringLiteral("Frontend finished loading"));
        TitleDataStore::instance().load();

        QMainWindow *main =
            static_cast<QMainWindow *>(obs_frontend_get_main_window());

        if (g_dock)
            destroy_dock_ui();

        g_dock = new TitleDock(main);
        QObject::connect(g_dock, &QObject::destroyed, []() {
            g_dock = nullptr;
        });
        g_dock->setObjectName("OBSGraphicsStudioProDock");
        g_dock->setWindowTitle(obsgs_tr("OBSTitles.DockName"));

        obs_frontend_add_custom_qdock("obs-graphics-studio-pro-dock", g_dock);
        add_docks_menu_entry(main);
        g_frontend_ready = true;
        title_hotkeys_register();
        blog(LOG_INFO, "[OBS Graphics Studio Pro] Dock and title cue hotkeys registered.");
        OGS_LOG_INFO("Plugin", QStringLiteral("Dock and title cue hotkeys registered"));
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
        OGS_LOG_INFO("Plugin", QStringLiteral("Scene collection cleanup"));
        /* OBS emits a cleanup event during initial startup before the scene
         * collection has finished loading. The title store has not changed at
         * that point, so writing it is both unnecessary and vulnerable to
         * transient filesystem/antivirus locks. Real collection switches occur
         * after the frontend is ready and still save the outgoing collection. */
        if (g_frontend_ready)
            TitleDataStore::instance().save();
        title_hotkeys_unregister();
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED && g_frontend_ready) {
        OGS_LOG_INFO("Plugin", QStringLiteral("Scene collection changed"));
        TitleDataStore::instance().load();
        title_hotkeys_register();
        if (g_dock)
            g_dock->update_scene_collection_title();
    }

    if (event == OBS_FRONTEND_EVENT_EXIT) {
        OGS_LOG_INFO("Plugin", QStringLiteral("Frontend exit"));
        if (TitlePreferences::clear_cache_on_exit()) {
            OGS_LOG_INFO("Plugin", QStringLiteral("Clearing frame cache on OBS exit"));
            CacheManager::instance().clearAll();
        }
        g_frontend_ready = false;
        title_hotkeys_unregister();
        TitleDataStore::instance().save();
        destroy_dock_ui();
    }
}
