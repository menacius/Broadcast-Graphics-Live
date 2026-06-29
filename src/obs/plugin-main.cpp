/*
 * Broadcast Graphics Live Plugin - plugin-main.cpp
 * Entry point: registers the source, dock, and module lifecycle.
 */

#include "plugin-main.h"
#include "title-source.h"
#include "title-dock.h"
#include "title-hotkeys.h"
#include "title-editor.h"
#include "title-assets.h"
#include "title-data.h"
#include "title-localization.h"
#include "title-logger.h"
#include "title-preferences.h"
#include "title-text-layout.h"
#include "title-text-layout-qt-font-registry.h"
#include "cache-manager.h"
#include "build-info.h"
#include "extensions/effect-extension-catalog.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QByteArray>
#include <QMainWindow>
#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QString>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* ── forward declarations ───────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void *priv);

/* ── module globals ─────────────────────────────────────────────── */
static TitleDock *g_dock = nullptr;
static QAction *g_dock_menu_action = nullptr;
static bool g_frontend_ready = false;
static bool g_frontend_exiting = false;
constexpr int kObsDockLayoutStateVersion = 1;
constexpr const char *kObsDockLayoutSettingsGroup = "ObsDockLayout";
constexpr const char *kObsMainWindowStateKey = "mainWindowState";

static void open_preferences_from_tools_menu(void *)
{
    QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    TitleEditor::show_global_preferences(main);
}

static void save_obs_dock_layout(QMainWindow *main)
{
    if (!main || !g_dock)
        return;

    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kObsDockLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kObsMainWindowStateKey),
                      main->saveState(kObsDockLayoutStateVersion));
    settings.endGroup();
    settings.sync();
}

static void restore_obs_dock_layout(QMainWindow *main)
{
    if (!main || !g_dock)
        return;

    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kObsDockLayoutSettingsGroup));
    const QByteArray state = settings.value(QString::fromUtf8(kObsMainWindowStateKey)).toByteArray();
    settings.endGroup();
    if (!state.isEmpty())
        main->restoreState(state, kObsDockLayoutStateVersion);
}


static QMenu *find_docks_menu(QMainWindow *main)
{
    if (!main || !main->menuBar()) return nullptr;
    for (auto *menu : main->menuBar()->findChildren<QMenu *>()) {
        QString title = menu->title();
        title.remove('&');
        if (title.compare(bgl_tr("OBSTitles.DocksMenu"), Qt::CaseInsensitive) == 0)
            return menu;
    }
    return nullptr;
}

static void destroy_dock_ui(bool frontend_api_available = true)
{
    if (frontend_api_available && g_dock) {
        if (auto *main = qobject_cast<QMainWindow *>(g_dock->parentWidget()))
            save_obs_dock_layout(main);
    }

    if (g_dock_menu_action) {
        QObject::disconnect(g_dock_menu_action, nullptr, nullptr, nullptr);
        if (QWidget *owner = qobject_cast<QWidget *>(g_dock_menu_action->parent()))
            owner->removeAction(g_dock_menu_action);
        delete g_dock_menu_action;
        g_dock_menu_action = nullptr;
    }

    if (g_dock) {
        QObject::disconnect(g_dock, nullptr, nullptr, nullptr);
        if (frontend_api_available)
            obs_frontend_remove_dock("broadcast-graphics-live-dock");
        delete g_dock;
        g_dock = nullptr;
    }
}

static void add_docks_menu_entry(QMainWindow *main)
{
    QMenu *docks_menu = find_docks_menu(main);
    if (!docks_menu || !g_dock || g_dock_menu_action) return;

    g_dock_menu_action = docks_menu->addAction(bgl_brand_icon(), bgl_tr("OBSTitles.DockName"));
    g_dock_menu_action->setObjectName("broadcast-graphics-live-docks-menu-action");
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
    g_frontend_exiting = false;
    TitleLogger::startSession();
    blog(LOG_INFO, "[Broadcast Graphics Live] Loading plugin %s", BGL_BUILD_DISPLAY);
    BGL_LOG_INFO("Plugin", QStringLiteral("Loading plugin %1").arg(QStringLiteral(BGL_BUILD_DISPLAY)));
    BglEffectExtensionCatalog::instance().reload();

    /* 1. Initialise persistent title store */
    TitleDataStore::instance().load();

    /* 2. Register the renderable source type and title cue hotkeys */
    title_source_register();
    title_hotkeys_register();

    /* 3. Add global preferences entry and defer dock/hotkey creation until the OBS UI is ready */
    obs_frontend_add_tools_menu_item("Broadcast Graphics Live Preferences", open_preferences_from_tools_menu, nullptr);
    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    blog(LOG_INFO, "[Broadcast Graphics Live] Plugin loaded.");
    BGL_LOG_INFO("Plugin", QStringLiteral("Plugin loaded"));
    return true;
}

/* ── module unload ──────────────────────────────────────────────── */
void obs_module_unload(void)
{
    title_hotkeys_unregister();
    TitleDataStore::instance().shutdownSaveWorker();
    TitleDataStore::instance().save();
    /* OBS_FRONTEND_EVENT_EXIT is the final point at which frontend API calls
     * are permitted.  A normal OBS shutdown has already removed the dock in
     * that callback, so module unload must not call remove_event_callback() or
     * remove_dock() against a frontend that is being dismantled.  Manual plugin
     * unload while OBS is still running keeps the normal frontend cleanup. */
    if (!g_frontend_exiting)
        obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    destroy_dock_ui(!g_frontend_exiting);
    /* Stop publication before rotating cache generations. This prevents an
     * in-flight prerender job from holding cache locks while shutdown clears the
     * index, and avoids deleting files on the OBS frontend thread. */
    CacheManager::instance().shutdownWorker();
    if (TitlePreferences::clear_cache_on_exit()) {
        BGL_LOG_INFO("Plugin", QStringLiteral("Detaching frame cache on module unload"));
        CacheManager::instance().clearAll();
    }
    release_title_gpu_render_resources();
    shared_text_layout_cache().clear();
    text_layout_clear_raw_font_registry();
    BglEffectExtensionCatalog::instance().shutdown();
    blog(LOG_INFO, "[Broadcast Graphics Live] Plugin unloaded.");
    BGL_LOG_INFO("Plugin", QStringLiteral("Plugin unloaded"));
    TitleLogger::endSession();
}

/* ── frontend event handler ─────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void * /*priv*/)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        BGL_LOG_INFO("Plugin", QStringLiteral("Frontend finished loading"));
        TitleDataStore::instance().load();

        QMainWindow *main =
            static_cast<QMainWindow *>(obs_frontend_get_main_window());

        if (g_dock)
            destroy_dock_ui();

        g_dock = new TitleDock(main);
        QObject::connect(g_dock, &QObject::destroyed, []() {
            g_dock = nullptr;
        });
        g_dock->setObjectName("BroadcastGraphicsLiveDock");
        g_dock->setWindowTitle(bgl_tr("OBSTitles.DockName") + QStringLiteral(" — ") + QStringLiteral(BGL_BUILD_DISPLAY));

        obs_frontend_add_custom_qdock("broadcast-graphics-live-dock", g_dock);
        QTimer::singleShot(0, g_dock, [main]() { restore_obs_dock_layout(main); });
        QObject::connect(g_dock, &QDockWidget::topLevelChanged, g_dock,
                         [main]() { save_obs_dock_layout(main); });
        QObject::connect(g_dock, &QDockWidget::dockLocationChanged, g_dock,
                         [main]() { save_obs_dock_layout(main); });
        QObject::connect(g_dock, &QDockWidget::visibilityChanged, g_dock,
                         [main]() { save_obs_dock_layout(main); });
        add_docks_menu_entry(main);
        g_frontend_ready = true;
        title_hotkeys_register();
        blog(LOG_INFO, "[Broadcast Graphics Live] Dock and title cue hotkeys registered.");
        BGL_LOG_INFO("Plugin", QStringLiteral("Dock and title cue hotkeys registered"));
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
        BGL_LOG_INFO("Plugin", QStringLiteral("Scene collection cleanup"));
        /* OBS emits a cleanup event during initial startup before the scene
         * collection has finished loading. The title store has not changed at
         * that point, so writing it is both unnecessary and vulnerable to
         * transient filesystem/antivirus locks. Real collection switches occur
         * after the frontend is ready and still save the outgoing collection. */
        if (g_frontend_ready) {
            /* Keep source output blocked for the whole cleanup→changed gap;
             * otherwise a video tick in that interval can rebuild and publish
             * the outgoing collection after it was just invalidated. */
            title_source_begin_scene_collection_transition();
            TitleDataStore::instance().save();
        } else {
            title_source_invalidate_all_presentations();
        }
        title_hotkeys_unregister();
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED && g_frontend_ready) {
        BGL_LOG_INFO("Plugin", QStringLiteral("Scene collection changed"));
        TitleDataStore::instance().load();
        title_source_end_scene_collection_transition();
        title_hotkeys_register();
        if (g_dock)
            g_dock->update_scene_collection_title();
    }

    if (event == OBS_FRONTEND_EVENT_EXIT) {
        BGL_LOG_INFO("Plugin", QStringLiteral("Frontend exit"));
        g_frontend_exiting = true;
        title_source_begin_shutdown();
        /* Cache shutdown/rotation is performed once from obs_module_unload(),
         * after the prerender worker has stopped. Doing it here as well caused
         * duplicate clears while sources and the worker were still active. */
        g_frontend_ready = false;
        title_hotkeys_unregister();
        TitleDataStore::instance().shutdownSaveWorker();
        TitleDataStore::instance().save();
        destroy_dock_ui(true);
    }
}
