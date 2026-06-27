#include "effect-extension-catalog.h"
#include "bgl-plugin-api.h"
#include "rendering/title-effect-registry.h"

#include <obs-module.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>

#include <algorithm>

namespace {

void appendBuiltIns(std::vector<BglEffectExtensionDefinition> &effects)
{
    for (const auto &meta : TitleEffectRegistry::definitions()) {
        BglEffectExtensionDefinition def;
        def.id = QString::fromUtf8(meta.stable_id);
        def.displayName = QString::fromUtf8(meta.display_name);
        def.category = QString::fromUtf8(meta.category);
        def.providerId = QStringLiteral("bgl.core");
        def.providerVersion = QStringLiteral(PLUGIN_VERSION);
        def.builtIn = true;
        def.builtInType = meta.type;
        effects.push_back(std::move(def));
    }
}

QList<QLibrary *> &loadedLibraries()
{
    static QList<QLibrary *> libs;
    return libs;
}

void unloadNativeLibraries()
{
    auto &libraries = loadedLibraries();
    for (QLibrary *library : libraries) {
        if (!library)
            continue;
        library->unload();
        delete library;
    }
    libraries.clear();
}

QString moduleDataRoot()
{
    char *path = obs_module_file("extensions");
    QString result = path ? QString::fromUtf8(path) : QString();
    if (path) bfree(path);
    return result;
}

QString moduleConfigRoot()
{
    char *path = obs_module_config_path("extensions");
    QString result = path ? QString::fromUtf8(path) : QString();
    if (path) bfree(path);
    return result;
}

constexpr qint64 kMaxExtensionJsonBytes = 4ll * 1024ll * 1024ll;
constexpr int kMaxExtensionScanDepth = 8;
constexpr std::size_t kMaxCatalogEffects = 1024;
constexpr uint32_t kMaxNativeEffectsPerPlugin = 256;

QString containedExtensionPath(const QString &basePath, const QString &candidate)
{
    if (basePath.isEmpty() || candidate.trimmed().isEmpty())
        return {};
    const QString base = QDir(basePath).canonicalPath().isEmpty()
        ? QDir(basePath).absolutePath()
        : QDir(basePath).canonicalPath();
    QFileInfo info(QFileInfo(candidate).isAbsolute()
                       ? candidate
                       : QDir(basePath).filePath(candidate));
    const QString resolved = info.exists() && !info.canonicalFilePath().isEmpty()
        ? info.canonicalFilePath()
        : info.absoluteFilePath();
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString prefix = QDir::cleanPath(base) + QLatin1Char('/');
    const QString cleanResolved = QDir::cleanPath(resolved);
    if (cleanResolved.compare(QDir::cleanPath(base), sensitivity) != 0 &&
        !cleanResolved.startsWith(prefix, sensitivity))
        return {};
    return cleanResolved;
}

QJsonObject loadJsonObjectFile(const QString &path, QStringList *diagnostics = nullptr)
{
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (diagnostics) diagnostics->push_back(QStringLiteral("Cannot read extension index %1").arg(path));
        return {};
    }
    if (file.size() < 0 || file.size() > kMaxExtensionJsonBytes) {
        if (diagnostics) diagnostics->push_back(QStringLiteral("Extension index is too large: %1").arg(path));
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        if (diagnostics) diagnostics->push_back(QStringLiteral("Invalid extension index %1: %2").arg(path, error.errorString()));
        return {};
    }
    return document.object();
}

QJsonObject resolveManifestIndex(const QJsonValue &value, const QString &basePath,
                                 QStringList *diagnostics)
{
    if (value.isString()) {
        const QString path = containedExtensionPath(basePath, value.toString());
        if (path.isEmpty()) {
            if (diagnostics) diagnostics->push_back(QStringLiteral("Extension index escapes its package: %1").arg(value.toString()));
            return {};
        }
        return loadJsonObjectFile(path, diagnostics);
    }
    if (!value.isObject()) return {};
    const QJsonObject object = value.toObject();
    const QString index = object.value(QStringLiteral("index")).toString();
    if (index.isEmpty()) return object;
    const QString indexPath = containedExtensionPath(basePath, index);
    if (indexPath.isEmpty()) {
        if (diagnostics) diagnostics->push_back(QStringLiteral("Extension index escapes its package: %1").arg(index));
        return {};
    }
    QJsonObject resolved = loadJsonObjectFile(indexPath, diagnostics);
    const QString indexDir = QFileInfo(index).path() == QStringLiteral(".")
        ? QString() : QFileInfo(index).path();
    if (!resolved.contains(QStringLiteral("items"))) {
        if (resolved.value(QStringLiteral("presets")).isArray())
            resolved.insert(QStringLiteral("items"), resolved.value(QStringLiteral("presets")));
        else if (resolved.value(QStringLiteral("assets")).isArray())
            resolved.insert(QStringLiteral("items"), resolved.value(QStringLiteral("assets")));
    }
    QJsonArray items = resolved.value(QStringLiteral("items")).toArray();
    QJsonArray safeItems;
    for (int i = 0; i < items.size(); ++i) {
        QJsonObject item = items.at(i).toObject();
        const QString file = item.value(QStringLiteral("file")).toString();
        if (!file.isEmpty()) {
            const QString relative = !indexDir.isEmpty() && QFileInfo(file).isRelative()
                ? QDir(indexDir).filePath(file) : file;
            const QString resolvedFile = containedExtensionPath(basePath, relative);
            if (resolvedFile.isEmpty()) {
                if (diagnostics) diagnostics->push_back(
                    QStringLiteral("Extension asset escapes its package: %1").arg(file));
                continue;
            }
            item.insert(QStringLiteral("file"), resolvedFile);
        }
        safeItems.append(item);
    }
    items = safeItems;
    if (!items.isEmpty()) resolved.insert(QStringLiteral("items"), items);
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it.key() != QStringLiteral("index")) resolved.insert(it.key(), it.value());
    resolved.insert(QStringLiteral("_indexPath"), index);
    return resolved;
}

void hostLog(int level, const char *component, const char *message)
{
    blog(level, "[Broadcast Graphics Live][Extension:%s] %s",
         component ? component : "plugin", message ? message : "");
}
}

BglEffectExtensionCatalog &BglEffectExtensionCatalog::instance()
{
    static BglEffectExtensionCatalog catalog;
    return catalog;
}

void BglEffectExtensionCatalog::reload()
{
    unloadNativeLibraries();
    effects_.clear();
    diagnostics_.clear();
    appendBuiltIns(effects_);
    const QString dataRoot = moduleDataRoot();
    const QString configRoot = moduleConfigRoot();
    scanManifestRoot(dataRoot);
    if (QDir(dataRoot).canonicalPath() != QDir(configRoot).canonicalPath())
        scanManifestRoot(configRoot);
    scanNativeRoot(dataRoot);
    if (QDir(dataRoot).canonicalPath() != QDir(configRoot).canonicalPath())
        scanNativeRoot(configRoot);
}

void BglEffectExtensionCatalog::shutdown()
{
    unloadNativeLibraries();
    effects_.clear();
    diagnostics_.clear();
}

const BglEffectExtensionDefinition *BglEffectExtensionCatalog::find(const QString &id) const
{
    for (const auto &effect : effects_)
        if (effect.id == id) return &effect;
    return nullptr;
}

const BglEffectExtensionDefinition *BglEffectExtensionCatalog::find(LayerEffectType type) const
{
    for (const auto &effect : effects_)
        if (effect.builtIn && effect.builtInType == type) return &effect;
    return nullptr;
}

QString BglEffectExtensionCatalog::builtInId(LayerEffectType type)
{
    if (const auto *definition = TitleEffectRegistry::definition(type))
        return QString::fromUtf8(definition->stable_id);
    return {};
}

bool BglEffectExtensionCatalog::builtInTypeForId(const QString &id, LayerEffectType *type)
{
    for (const auto &definition : TitleEffectRegistry::definitions()) {
        if (id == QString::fromUtf8(definition.stable_id) ||
            id == QString::fromUtf8(definition.legacy_id)) {
            if (type)
                *type = definition.type;
            return true;
        }
    }
    return false;
}

void BglEffectExtensionCatalog::scanManifestRoot(const QString &root, int depth)
{
    if (root.isEmpty() || depth > kMaxExtensionScanDepth ||
        effects_.size() >= kMaxCatalogEffects)
        return;
    QDir dir(root);
    const QStringList manifests = dir.entryList({QStringLiteral("*.bgl-effect.json"), QStringLiteral("manifest.json")}, QDir::Files);
    for (const QString &name : manifests) loadManifest(dir.filePath(name));
    const QFileInfoList children = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &child : children) {
        if (effects_.size() >= kMaxCatalogEffects)
            break;
        scanManifestRoot(child.absoluteFilePath(), depth + 1);
    }
}

void BglEffectExtensionCatalog::loadManifest(const QString &manifestPath)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostics_ << QStringLiteral("Cannot read %1").arg(manifestPath);
        return;
    }
    if (file.size() < 0 || file.size() > kMaxExtensionJsonBytes) {
        diagnostics_ << QStringLiteral("Extension manifest is too large: %1").arg(manifestPath);
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        diagnostics_ << QStringLiteral("Invalid extension manifest %1: %2").arg(manifestPath, error.errorString());
        return;
    }
    const QJsonObject root = document.object();
    const int manifestApi = root.value(QStringLiteral("apiVersion")).toInt();
    if (manifestApi < 1 || manifestApi > static_cast<int>(BGL_PLUGIN_API_VERSION)) {
        diagnostics_ << QStringLiteral("Unsupported API version in %1").arg(manifestPath);
        return;
    }
    BglEffectExtensionDefinition def;
    def.id = root.value(QStringLiteral("id")).toString().trimmed();
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{2,127}$"));
    if (!validId.match(def.id).hasMatch()) {
        diagnostics_ << QStringLiteral("Invalid extension id in %1").arg(manifestPath);
        return;
    }
    def.displayName = root.value(QStringLiteral("name")).toString(def.id).trimmed();
    def.category = root.value(QStringLiteral("category")).toString(QStringLiteral("Extensions"));
    def.providerId = root.value(QStringLiteral("provider")).toString(QStringLiteral("manifest"));
    def.providerVersion = root.value(QStringLiteral("version")).toString(QStringLiteral("1.0.0"));
    def.manifestPath = manifestPath;
    def.basePath = QFileInfo(manifestPath).absolutePath();
    def.shaderPath = containedExtensionPath(def.basePath, root.value(QStringLiteral("shader")).toString());
    def.parameterSchema = root.value(QStringLiteral("parameters")).toObject();
    def.defaults = root.value(QStringLiteral("defaults")).toObject();
    def.editorSchema = root.value(QStringLiteral("editor")).toObject();
    def.presetIndex = resolveManifestIndex(root.value(QStringLiteral("presets")), def.basePath, &diagnostics_);
    def.assetIndex = resolveManifestIndex(root.value(QStringLiteral("assets")), def.basePath, &diagnostics_);
    def.capabilities = root.value(QStringLiteral("capabilities")).toObject();
    def.animationSchema = root.value(QStringLiteral("animation")).toObject();
    def.canvasHandles = root.value(QStringLiteral("canvasHandles")).toArray();
    if (def.canvasHandles.isEmpty())
        def.canvasHandles = def.editorSchema.value(QStringLiteral("canvasHandles")).toArray();
    def.schemaVersion = static_cast<uint32_t>(std::clamp(
        root.value(QStringLiteral("schemaVersion")).toInt(1), 1, 65535));
    for (const QJsonValue &value : root.value(QStringLiteral("techniques")).toArray())
        def.techniques << value.toString();
    if (def.id.isEmpty() || def.shaderPath.isEmpty()) {
        diagnostics_ << QStringLiteral("Extension manifest is missing a valid in-package shader: %1").arg(manifestPath);
        return;
    }
    const QFileInfo shaderInfo(def.shaderPath);
    if (!shaderInfo.isFile() || !shaderInfo.isReadable()) {
        diagnostics_ << QStringLiteral("Extension shader is missing or unreadable: %1").arg(def.shaderPath);
        return;
    }
    if (find(def.id)) {
        diagnostics_ << QStringLiteral("Duplicate effect id ignored: %1").arg(def.id);
        return;
    }
    effects_.push_back(std::move(def));
}

void BglEffectExtensionCatalog::scanNativeRoot(const QString &root, int depth)
{
    if (root.isEmpty() || depth > kMaxExtensionScanDepth ||
        effects_.size() >= kMaxCatalogEffects)
        return;
    QDir dir(root);
#if defined(Q_OS_WIN)
    const QStringList filters{QStringLiteral("*.dll")};
#elif defined(Q_OS_MACOS)
    const QStringList filters{QStringLiteral("*.dylib")};
#else
    const QStringList filters{QStringLiteral("*.so")};
#endif
    for (const QFileInfo &file : dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks)) {
        auto *library = new QLibrary(file.absoluteFilePath());
        if (!library->load()) {
            diagnostics_ << QStringLiteral("Cannot load native extension %1: %2")
                                .arg(file.fileName(), library->errorString());
            delete library;
            continue;
        }

        const bgl_host_api_v1 host{BGL_PLUGIN_API_VERSION, hostLog};
        const auto query3 = reinterpret_cast<bgl_plugin_query_v3_fn>(library->resolve("bgl_plugin_query_v3"));
        const auto query2 = reinterpret_cast<bgl_plugin_query_v2_fn>(library->resolve("bgl_plugin_query_v2"));
        const auto query1 = reinterpret_cast<bgl_plugin_query_v1_fn>(library->resolve("bgl_plugin_query_v1"));
        const bgl_plugin_descriptor_v3 *plugin3 = query3 ? query3(&host) : nullptr;
        const bgl_plugin_descriptor_v2 *queriedPlugin2 = plugin3
            ? &plugin3->v2 : (query2 ? query2(&host) : nullptr);
        const bool validV2Descriptor = queriedPlugin2 &&
            queriedPlugin2->descriptor_size >= sizeof(bgl_plugin_descriptor_v2);
        const bgl_plugin_descriptor_v2 *plugin2 = validV2Descriptor
            ? queriedPlugin2 : nullptr;
        const bgl_plugin_descriptor_v1 *plugin = queriedPlugin2
            ? &queriedPlugin2->v1 : (query1 ? query1(&host) : nullptr);
        if (!plugin || plugin->api_version < BGL_PLUGIN_API_VERSION_1 ||
            plugin->api_version > BGL_PLUGIN_API_VERSION || !plugin->id) {
            diagnostics_ << QStringLiteral("Rejected incompatible native extension: %1").arg(file.fileName());
            library->unload();
            delete library;
            continue;
        }

        const bool hasV3Effects = plugin3 && validV2Descriptor && plugin3->effects_v3 && plugin3->effect_v3_count > 0;
        const bool hasV2Effects = plugin2 && plugin2->effects_v2 && plugin2->effect_v2_count > 0;
        const bool hasV1Effects = plugin->effects && plugin->effect_count > 0;
        const uint32_t declaredCount = hasV3Effects ? plugin3->effect_v3_count
                                      : hasV2Effects ? plugin2->effect_v2_count
                                                     : plugin->effect_count;
        const uint32_t count = std::min(declaredCount, kMaxNativeEffectsPerPlugin);
        if (!hasV3Effects && !hasV2Effects && !hasV1Effects) {
            diagnostics_ << QStringLiteral("Native extension has no effect descriptors: %1").arg(file.fileName());
            library->unload();
            delete library;
            continue;
        }

        auto parseObject = [](const char *json) {
            if (!json)
                return QJsonObject{};
            QJsonParseError error{};
            const auto doc = QJsonDocument::fromJson(QByteArray(json), &error);
            return error.error == QJsonParseError::NoError && doc.isObject()
                ? doc.object() : QJsonObject{};
        };
        auto parseArray = [](const char *json) {
            if (!json)
                return QJsonArray{};
            QJsonParseError error{};
            const auto doc = QJsonDocument::fromJson(QByteArray(json), &error);
            return error.error == QJsonParseError::NoError && doc.isArray()
                ? doc.array() : QJsonArray{};
        };

        uint32_t accepted = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const bgl_effect_descriptor_v3 *v3 = hasV3Effects ? &plugin3->effects_v3[i] : nullptr;
            const bgl_effect_descriptor_v2 *v2 = v3 ? &v3->v2
                                                   : (hasV2Effects ? &plugin2->effects_v2[i] : nullptr);
            const bgl_effect_descriptor_v1 &src = v2 ? v2->v1 : plugin->effects[i];
            if (!src.id || !src.shader_path) {
                diagnostics_ << QStringLiteral("Native extension %1 has an incomplete effect descriptor")
                                    .arg(file.fileName());
                continue;
            }

            const QString effectId = QString::fromUtf8(src.id).trimmed();
            static const QRegularExpression validNativeId(
                QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{2,127}$"));
            if (!validNativeId.match(effectId).hasMatch() || find(effectId)) {
                diagnostics_ << QStringLiteral("Duplicate or invalid native effect id ignored: %1").arg(effectId);
                continue;
            }
            const QString shaderPath = containedExtensionPath(
                file.absolutePath(), QString::fromUtf8(src.shader_path));
            if (shaderPath.isEmpty() || !QFileInfo(shaderPath).isFile() ||
                !QFileInfo(shaderPath).isReadable()) {
                diagnostics_ << QStringLiteral("Native extension shader is missing or outside its package: %1")
                                    .arg(QString::fromUtf8(src.shader_path));
                continue;
            }

            BglEffectExtensionDefinition def;
            def.id = effectId;
            def.displayName = QString::fromUtf8(src.display_name ? src.display_name : src.id);
            def.category = QString::fromUtf8(src.category ? src.category : "Extensions");
            def.shaderPath = shaderPath;
            def.providerId = QString::fromUtf8(plugin->id);
            def.providerVersion = QString::fromUtf8(plugin->version ? plugin->version : "");
            def.nativeProvider = true;
            def.basePath = file.absolutePath();
            if (v2) {
                def.schemaVersion = std::clamp<uint32_t>(v2->schema_version, 1u, 65535u);
                def.editorSchema = parseObject(v2->editor_schema_json);
                def.presetIndex = parseObject(v2->preset_index_json);
                def.assetIndex = parseObject(v2->asset_index_json);
                def.capabilities = parseObject(v2->capabilities_json);
                def.animationSchema = parseObject(v2->animation_schema_json);
                def.validateState = plugin2->validate_state;
                def.migrateState = plugin2->migrate_state;
                def.releaseString = plugin2->release_string;
            }
            if (v3)
                def.canvasHandles = parseArray(v3->canvas_handles_schema_json);
            if (src.manifest_json) {
                const QJsonObject metadata = parseObject(src.manifest_json);
                def.parameterSchema = metadata.value(QStringLiteral("parameters")).toObject();
                def.defaults = metadata.value(QStringLiteral("defaults")).toObject();
                if (def.canvasHandles.isEmpty())
                    def.canvasHandles = metadata.value(QStringLiteral("canvasHandles")).toArray();
            }
            effects_.push_back(std::move(def));
            ++accepted;
        }

        if (accepted == 0) {
            library->unload();
            delete library;
        } else {
            loadedLibraries().push_back(library);
        }
    }
    for (const QFileInfo &child : dir.entryInfoList(
             QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (effects_.size() >= kMaxCatalogEffects)
            break;
        scanNativeRoot(child.absoluteFilePath(), depth + 1);
    }
}
