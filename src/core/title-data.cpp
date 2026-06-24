/*
 * title-data.cpp
 */

#include "title-data.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QSaveFile>
#include <QString>

#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <iterator>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <stdexcept>
#include <cstdio>
#include <limits>
#include <utility>
#include <cctype>
#include <ctime>
#include <thread>

using json = nlohmann::json;

namespace {
constexpr std::streamoff kMaxJsonFileBytes = 512 * 1024 * 1024;
constexpr std::streamoff kMaxEmbeddedAssetBytes = 100 * 1024 * 1024;
constexpr size_t kMaxTitles = 256;
constexpr size_t kMaxLayersPerTitle = 256;
constexpr size_t kMaxKeyframesPerProperty = 2048;
constexpr size_t kMaxLiveTextRows = 256;
constexpr size_t kMaxLiveTextColumns = 32;
constexpr size_t kMaxNameLength = 256;
constexpr size_t kMaxTextLength = 8192;
constexpr size_t kMaxScreenshotBase64Length = 32 * 1024 * 1024;
constexpr double kMaxDuration = 3600.0;
constexpr double kMaxPropertyValue = 100000.0;
constexpr int kMaxCanvasDimension = 16384;

static double finite_or(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

static int normalize_gradient_type(int type)
{
    switch (std::clamp(type, 0, 4)) {
    case 1:
        return 1; /* radial */
    case 2:
        return 2; /* conical; legacy Angle used the same conical renderer */
    case 4:
        return 1; /* legacy Diamond falls back to radial */
    case 0:
    case 3:
    default:
        return 0; /* linear; legacy Reflected is represented by spread */
    }
}

static int normalize_gradient_spread(int spread)
{
    return spread == 1 || spread == 2 ? spread : 0;
}

static std::string current_iso_utc_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    std::ostringstream out;
    out << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

static std::string title_data_dir()
{
    char *cfg_dir = obs_module_config_path("");
    std::string dir(cfg_dir ? cfg_dir : "");
    bfree(cfg_dir);
    os_mkdirs(dir.c_str());
    return dir;
}

static uint64_t fnv1a64(const std::string &value)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

static std::string hex64(uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

static std::string current_scene_collection_name()
{
    char *collection_name = obs_frontend_get_current_scene_collection();
    std::string name(collection_name ? collection_name : "");
    bfree(collection_name);
    if (name.empty())
        name = "unknown-scene-collection";
    return name;
}

static std::string safe_scene_collection_file_stem(const std::string &name)
{
    std::string safe;
    safe.reserve(std::min<size_t>(name.size(), 80));
    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            safe.push_back((char)ch);
        else
            safe.push_back('_');
        if (safe.size() >= 80)
            break;
    }
    if (safe.empty())
        safe = "scene-collection";
    return safe + "-" + hex64(fnv1a64(name));
}

static std::string bounded_string(const json &j, const char *key,
                                  const std::string &fallback,
                                  size_t max_len)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        return fallback;
    std::string value = j[key].get<std::string>();
    if (value.size() > max_len)
        value.resize(max_len);
    return value;
}

static const json *object_member(const json &j, const char *key)
{
    if (!j.is_object())
        return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

static bool json_bool(const json &j, const char *key, bool fallback)
{
    const json *value = object_member(j, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

static int json_int(const json &j, const char *key, int fallback)
{
    const json *value = object_member(j, key);
    if (!value || !value->is_number_integer())
        return fallback;
    const int64_t parsed = value->get<int64_t>();
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
        return fallback;
    return (int)parsed;
}

static int gradient_spread_from_json(const json &j, const char *spread_key,
                                     const char *type_key, int fallback_spread = 0)
{
    const int legacy_type = std::clamp(json_int(j, type_key, 0), 0, 4);
    if (!j.contains(spread_key) && legacy_type == 3)
        return 1; /* legacy Reflected */
    return normalize_gradient_spread(json_int(j, spread_key, fallback_spread));
}

static double json_double(const json &j, const char *key, double fallback)
{
    const json *value = object_member(j, key);
    return value && value->is_number() ? finite_or(value->get<double>(), fallback) : fallback;
}

static uint32_t json_color(const json &j, const char *key, uint32_t fallback)
{
    const json *value = object_member(j, key);
    if (!value)
        return fallback;
    if (value->is_number_unsigned()) {
        const uint64_t parsed = value->get<uint64_t>();
        return parsed <= UINT32_MAX ? (uint32_t)parsed : fallback;
    }
    if (value->is_number_integer()) {
        const int64_t parsed = value->get<int64_t>();
        return parsed >= 0 && parsed <= UINT32_MAX ? (uint32_t)parsed : fallback;
    }
    return fallback;
}


static bool file_exists(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return f.is_open();
}

static std::string file_name_from_path(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string sanitize_asset_file_name(const std::string &file_name)
{
    std::string sanitized;
    sanitized.reserve(file_name.size());
    for (unsigned char ch : file_name) {
        if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_')
            sanitized.push_back((char)ch);
        else
            sanitized.push_back('_');
    }
    while (!sanitized.empty() && sanitized.front() == '.')
        sanitized.erase(sanitized.begin());
    if (sanitized.empty())
        sanitized = "image.bin";
    if (sanitized.size() > 160)
        sanitized.resize(160);
    return sanitized;
}

static std::string lower_extension(const std::string &file_name)
{
    const size_t dot = file_name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = file_name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return ext;
}

static std::string mime_type_for_file_name(const std::string &file_name)
{
    const std::string ext = lower_extension(file_name);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "svg" || ext == "svgz") return "image/svg+xml";
    return "application/octet-stream";
}

static bool read_binary_file(const std::string &path, std::string &out, std::streamoff max_bytes, std::string *error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "Could not open asset file: " + path;
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || size > max_bytes) {
        if (error) *error = "Asset file is too large to embed: " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);

    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!f.good() && !f.eof()) {
        if (error) *error = "Failed while reading asset file: " + path;
        return false;
    }
    return true;
}

static std::string base64_encode(const std::string &data)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    int value = 0;
    int bits = -6;
    for (unsigned char ch : data) {
        value = (value << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            encoded.push_back(table[(value >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6)
        encoded.push_back(table[((value << 8) >> (bits + 8)) & 0x3F]);
    while (encoded.size() % 4)
        encoded.push_back('=');
    return encoded;
}

static bool base64_decode(const std::string &encoded, std::string &out)
{
    static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> reverse(256, -1);
    for (int i = 0; i < (int)table.size(); ++i)
        reverse[(unsigned char)table[i]] = i;

    out.clear();
    int value = 0;
    int bits = -8;
    for (unsigned char ch : encoded) {
        if (std::isspace(ch))
            continue;
        if (ch == '=')
            break;
        if (reverse[ch] == -1)
            return false;
        value = (value << 6) + reverse[ch];
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

static uint64_t fnv1a_64(const std::string &data)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : data) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string hex_u64(uint64_t value)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << value;
    return ss.str();
}

static std::string embedded_assets_dir()
{
    char *cfg_dir = obs_module_config_path("");
    std::string dir(cfg_dir);
    bfree(cfg_dir);
    const std::string assets = dir + "/assets";
    os_mkdirs(assets.c_str());
    return assets;
}

static bool attach_embedded_image_asset(const Layer &layer, json &j, bool required, std::string *error)
{
    if (layer.type != LayerType::Image || layer.image_path.empty())
        return true;

    std::string data;
    if (!read_binary_file(layer.image_path, data, kMaxEmbeddedAssetBytes, error))
        return !required;

    const std::string file_name = sanitize_asset_file_name(file_name_from_path(layer.image_path));
    json asset;
    asset["file_name"] = file_name;
    asset["mime_type"] = mime_type_for_file_name(file_name);
    asset["size"] = data.size();
    asset["hash"] = hex_u64(fnv1a_64(data));
    asset["data_base64"] = base64_encode(data);
    j["embedded_image"] = std::move(asset);
    return true;
}

static bool restore_embedded_image_asset(const json &j, std::string &image_path)
{
    const json *asset = object_member(j, "embedded_image");
    if (!asset || !asset->is_object())
        return false;

    const std::string data64 = bounded_string(*asset, "data_base64", "", (size_t)kMaxEmbeddedAssetBytes * 2);
    if (data64.empty())
        return false;

    std::string data;
    if (!base64_decode(data64, data) || data.empty() || (std::streamoff)data.size() > kMaxEmbeddedAssetBytes)
        return false;

    std::string file_name = sanitize_asset_file_name(bounded_string(*asset, "file_name", "image.bin", kMaxNameLength));
    const std::string hash = hex_u64(fnv1a_64(data));
    file_name = hash + "-" + file_name;

    const std::string path = embedded_assets_dir() + "/" + file_name;
    if (!file_exists(path)) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f.good()) {
            std::remove(path.c_str());
            return false;
        }
    }

    image_path = path;
    return true;
}

static bool read_json_file(const std::string &path, json &out, std::string *error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "Could not open the file.";
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || size > kMaxJsonFileBytes) {
        if (error) *error = "File is too large for a title template.";
        return false;
    }
    f.seekg(0, std::ios::beg);

    try {
        f >> out;
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return false;
    }
    return true;
}

static void ensure_unique_title_id(const std::shared_ptr<Title> &title,
                                   std::unordered_set<std::string> &seen)
{
    if (!title)
        return;
    if (title->id.empty() || seen.find(title->id) != seen.end())
        title->id = TitleDataStore::make_uuid();
    seen.insert(title->id);

    std::unordered_set<std::string> layer_ids;
    for (auto &layer : title->layers) {
        if (!layer)
            continue;
        if (layer->id.empty() || layer_ids.find(layer->id) != layer_ids.end())
            layer->id = TitleDataStore::make_uuid();
        layer_ids.insert(layer->id);
    }
    for (auto &layer : title->layers) {
        if (!layer)
            continue;
        if (!layer->parent_id.empty() && layer_ids.find(layer->parent_id) == layer_ids.end())
            layer->parent_id.clear();
        if (!layer->mask_source_id.empty() && layer_ids.find(layer->mask_source_id) == layer_ids.end()) {
            layer->mask_source_id.clear();
            layer->mask_mode = MaskMode::None;
        }
    }
}
} // namespace

static void set_argb_channels(AnimatedProperty &a, AnimatedProperty &r, AnimatedProperty &g, AnimatedProperty &b, uint32_t argb)
{
    a.static_value = (argb >> 24) & 0xFF;
    r.static_value = (argb >> 16) & 0xFF;
    g.static_value = (argb >> 8) & 0xFF;
    b.static_value = argb & 0xFF;
}

static void set_color_channels(Layer &l, bool text, uint32_t argb)
{
    AnimatedProperty &a = text ? l.text_color_a : l.fill_color_a;
    AnimatedProperty &r = text ? l.text_color_r : l.fill_color_r;
    AnimatedProperty &g = text ? l.text_color_g : l.fill_color_g;
    AnimatedProperty &b = text ? l.text_color_b : l.fill_color_b;
    set_argb_channels(a, r, g, b, argb);
}

static void set_background_color_channels(Layer &l, uint32_t argb)
{
    l.background_color_a.static_value = (argb >> 24) & 0xFF;
    l.background_color_r.static_value = (argb >> 16) & 0xFF;
    l.background_color_g.static_value = (argb >> 8) & 0xFF;
    l.background_color_b.static_value = argb & 0xFF;
}

/* ══════════════════════════════════════════════════════════════════
 *  UUID helper
 * ══════════════════════════════════════════════════════════════════ */
std::string TitleDataStore::make_uuid()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);
    // Set UUID version 4 bits
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << (hi >> 32);
    ss << '-' << std::setw(4) << ((hi >> 16) & 0xFFFF);
    ss << '-' << std::setw(4) << (hi & 0xFFFF);
    ss << '-' << std::setw(4) << (lo >> 48);
    ss << '-' << std::setw(12) << (lo & 0x0000FFFFFFFFFFFFULL);
    return ss.str();
}

/* ══════════════════════════════════════════════════════════════════
 *  Title helpers
 * ══════════════════════════════════════════════════════════════════ */
std::shared_ptr<Layer> Title::find_layer(const std::string &lid) const
{
    for (auto &l : layers)
        if (l && l->id == lid) return l;
    return nullptr;
}

void Title::add_layer(std::shared_ptr<Layer> l)
{
    if (l)
        layers.push_back(l);
}

void Title::remove_layer(const std::string &lid)
{
    layers.erase(
        std::remove_if(layers.begin(), layers.end(),
                       [&](auto &l){ return !l || l->id == lid; }),
        layers.end());
    for (auto &layer : layers) {
        if (!layer) continue;
        if (layer->parent_id == lid) layer->parent_id.clear();
        if (layer->mask_source_id == lid) {
            layer->mask_source_id.clear();
            layer->mask_mode = MaskMode::None;
        }
    }
}

void Title::move_layer(const std::string &lid, int delta)
{
    auto it = std::find_if(layers.begin(), layers.end(),
                           [&](auto &l){ return l && l->id == lid; });
    if (it == layers.end()) return;
    int idx = (int)(it - layers.begin());
    int dst = std::clamp(idx + delta, 0, (int)layers.size() - 1);
    if (idx == dst) return;
    auto layer = *it;
    layers.erase(it);
    layers.insert(layers.begin() + dst, layer);
}

/* ══════════════════════════════════════════════════════════════════
 *  TitleDataStore
 * ══════════════════════════════════════════════════════════════════ */
TitleDataStore &TitleDataStore::instance()
{
    static TitleDataStore inst;
    return inst;
}

std::vector<std::shared_ptr<Title>> TitleDataStore::titles() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return titles_;
}

uint64_t TitleDataStore::on_change(ChangeCallback cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const uint64_t id = next_change_cb_id_++;
    change_cbs_.push_back(ChangeObserver {id, std::move(cb)});
    return id;
}

void TitleDataStore::remove_change_callback(uint64_t callback_id)
{
    if (callback_id == 0) return;

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = std::remove_if(change_cbs_.begin(), change_cbs_.end(),
                             [callback_id](const ChangeObserver &observer) {
                                 return observer.id == callback_id;
                             });
    change_cbs_.erase(it, change_cbs_.end());
}

void TitleDataStore::notify_change()
{
    touch_runtime_change();

    std::vector<ChangeCallback> callbacks;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        callbacks.reserve(change_cbs_.size());
        for (const auto &observer : change_cbs_)
            callbacks.push_back(observer.callback);
    }

    for (auto &cb : callbacks) cb();
}

void TitleDataStore::touch_runtime_change()
{
    revision_.fetch_add(1, std::memory_order_relaxed);
}

void ensure_live_text_row_ids(Title &title)
{
    if (title.live_text_row_ids.size() > title.live_text_rows.size())
        title.live_text_row_ids.resize(title.live_text_rows.size());

    std::set<std::string> used;
    for (size_t i = 0; i < title.live_text_rows.size(); ++i) {
        if (i >= title.live_text_row_ids.size())
            title.live_text_row_ids.push_back({});
        std::string &id = title.live_text_row_ids[i];
        if (id.empty() || used.count(id))
            id = TitleDataStore::make_uuid();
        used.insert(id);
    }
}

std::string live_text_row_id(const Title &title, int row)
{
    if (row < 0 || row >= static_cast<int>(title.live_text_rows.size()))
        return {};
    if (row < static_cast<int>(title.live_text_row_ids.size()) &&
        !title.live_text_row_ids[static_cast<size_t>(row)].empty())
        return title.live_text_row_ids[static_cast<size_t>(row)];
    return std::string("legacy-row-") + std::to_string(row);
}

std::shared_ptr<Title> TitleDataStore::create_title(const std::string &name)
{
    auto t = std::make_shared<Title>();
    t->id   = make_uuid();
    t->name = name;
    t->creation_date = current_iso_utc_string();

    /* Default: one text layer */
    auto layer = std::make_shared<Layer>();
    layer->id   = make_uuid();
    layer->name = "Title Text";
    layer->type = LayerType::Text;
    layer->position.static_value.x = 960.0;
    layer->position.static_value.y = 540.0;
    layer->rect_width = 960.0f;
    layer->rect_height = 160.0f;
    layer->size.static_value.x = layer->rect_width;
    layer->size.static_value.y = layer->rect_height;
    set_color_channels(*layer, true, layer->text_color);
    set_color_channels(*layer, false, layer->fill_color);
    layer->text_content = name;
    layer->rich_text = rich_text_document_from_layer_defaults(*layer);
    layer->expose_text = true;
    t->layers.push_back(layer);

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        titles_.push_back(t);
    }
    notify_change();
    return t;
}

std::shared_ptr<Title> TitleDataStore::get_title(const std::string &id) const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto &t : titles_)
        if (t->id == id) return t;
    return nullptr;
}

void TitleDataStore::delete_title(const std::string &id)
{
    bool deleted = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto old_size = titles_.size();
        titles_.erase(
            std::remove_if(titles_.begin(), titles_.end(),
                           [&](auto &t){ return t && t->id == id; }),
            titles_.end());
        deleted = titles_.size() != old_size;
    }

    if (deleted)
        notify_change();
}

void TitleDataStore::rename_title(const std::string &id, const std::string &n)
{
    bool renamed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto &t : titles_) {
            if (t && t->id == id) {
                t->name = n;
                renamed = true;
                break;
            }
        }
    }

    if (renamed)
        notify_change();
}

/* ── persistence ──────────────────────────────────────────────────── */
std::string TitleDataStore::data_path()
{
    const std::string dir = title_data_dir() + "/scene-collection-titles";
    os_mkdirs(dir.c_str());
    const std::string collection_name = current_scene_collection_name();
    return dir + "/" + safe_scene_collection_file_stem(collection_name) + ".json";
}

/* ---- JSON serialisation helpers (flat, no macros) ---- */
static json keyframe_to_json(const Keyframe &k)
{
    return {
        {"time",   k.time},
        {"value",  k.value},
        {"easing", (int)k.easing},
        {"cx1",    k.cx1}, {"cy1", k.cy1},
        {"cx2",    k.cx2}, {"cy2", k.cy2},
    };
}

static Keyframe keyframe_from_json(const json &j)
{
    Keyframe k;
    if (!j.is_object())
        return k;
    k.time = std::clamp(finite_or(json_double(j, "time", 0.0), 0.0), 0.0, kMaxDuration);
    k.value = std::clamp(finite_or(json_double(j, "value", 0.0), 0.0), -kMaxPropertyValue, kMaxPropertyValue);
    k.easing = (EasingType)std::clamp(json_int(j, "easing", 0), 0, (int)EasingType::Hold);
    k.cx1 = std::clamp(finite_or(json_double(j, "cx1", 0.333), 0.333), 0.0, 1.0);
    k.cy1 = std::clamp(finite_or(json_double(j, "cy1", 0.0), 0.0), 0.0, 1.0);
    k.cx2 = std::clamp(finite_or(json_double(j, "cx2", 0.667), 0.667), 0.0, 1.0);
    k.cy2 = std::clamp(finite_or(json_double(j, "cy2", 1.0), 1.0), 0.0, 1.0);
    return k;
}

static json aprop_to_json(const AnimatedProperty &p)
{
    json j = { {"static_value", p.static_value} };
    json kf = json::array();
    for (auto &k : p.keyframes) kf.push_back(keyframe_to_json(k));
    j["keyframes"] = kf;
    return j;
}

static AnimatedProperty aprop_from_json(const json &j, const std::string &name)
{
    AnimatedProperty p;
    p.name = name;
    if (!j.is_object())
        return p;

    p.static_value = std::clamp(finite_or(json_double(j, "static_value", 0.0), 0.0),
                                -kMaxPropertyValue, kMaxPropertyValue);
    if (j.contains("keyframes") && j["keyframes"].is_array()) {
        const size_t count = std::min(j["keyframes"].size(), kMaxKeyframesPerProperty);
        p.keyframes.reserve(count);
        for (size_t i = 0; i < count; ++i)
            p.keyframes.push_back(keyframe_from_json(j["keyframes"][i]));
        std::sort(p.keyframes.begin(), p.keyframes.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
    }
    return p;
}

static json vec2_aprop_to_json(const AnimatedVec2Property &p)
{
    json j;
    j["static_value"] = {{"x", p.static_value.x}, {"y", p.static_value.y}};
    json kf = json::array();
    for (const auto &k : p.keyframes) {
        kf.push_back({{"time", k.time},
                      {"value", {{"x", k.value.x}, {"y", k.value.y}}},
                      {"easing", (int)k.easing},
                      {"cx1", k.cx1}, {"cy1", k.cy1},
                      {"cx2", k.cx2}, {"cy2", k.cy2}});
    }
    j["keyframes"] = kf;
    return j;
}

static void vec2_aprop_from_json(const json &j, AnimatedVec2Property &p)
{
    if (!j.is_object())
        return;
    if (j.contains("static_value") && j["static_value"].is_object()) {
        p.static_value.x = std::clamp(finite_or(json_double(j["static_value"], "x", p.static_value.x), p.static_value.x),
                                      -kMaxPropertyValue, kMaxPropertyValue);
        p.static_value.y = std::clamp(finite_or(json_double(j["static_value"], "y", p.static_value.y), p.static_value.y),
                                      -kMaxPropertyValue, kMaxPropertyValue);
    }
    if (j.contains("keyframes") && j["keyframes"].is_array()) {
        const size_t count = std::min(j["keyframes"].size(), kMaxKeyframesPerProperty);
        p.keyframes.clear();
        p.keyframes.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto &item = j["keyframes"][i];
            if (!item.is_object()) continue;
            VectorKeyframe k;
            k.time = std::clamp(finite_or(json_double(item, "time", 0.0), 0.0), 0.0, kMaxDuration);
            if (item.contains("value") && item["value"].is_object()) {
                k.value.x = std::clamp(finite_or(json_double(item["value"], "x", 0.0), 0.0),
                                       -kMaxPropertyValue, kMaxPropertyValue);
                k.value.y = std::clamp(finite_or(json_double(item["value"], "y", 0.0), 0.0),
                                       -kMaxPropertyValue, kMaxPropertyValue);
            }
            k.easing = (EasingType)std::clamp(json_int(item, "easing", 0), 0, (int)EasingType::Hold);
            k.cx1 = std::clamp(finite_or(json_double(item, "cx1", 0.333), 0.333), 0.0, 1.0);
            k.cy1 = std::clamp(finite_or(json_double(item, "cy1", 0.0), 0.0), 0.0, 1.0);
            k.cx2 = std::clamp(finite_or(json_double(item, "cx2", 0.667), 0.667), 0.0, 1.0);
            k.cy2 = std::clamp(finite_or(json_double(item, "cy2", 1.0), 1.0), 0.0, 1.0);
            p.keyframes.push_back(k);
        }
        std::sort(p.keyframes.begin(), p.keyframes.end(),
                  [](const VectorKeyframe &a, const VectorKeyframe &b) { return a.time < b.time; });
    }
}



static json gradient_stops_to_json(const std::vector<GradientStop> &stops)
{
    json arr = json::array();
    for (const auto &stop : stops) {
        arr.push_back({{"color", stop.color},
                       {"position", std::clamp((double)stop.position, 0.0, 1.0)},
                       {"opacity", std::clamp((double)stop.opacity, 0.0, 1.0)}});
    }
    return arr;
}

static std::vector<GradientStop> gradient_stops_from_json(const json &j)
{
    std::vector<GradientStop> stops;
    if (!j.is_array()) return stops;
    for (const auto &item : j) {
        if (!item.is_object()) continue;
        GradientStop stop;
        stop.color = json_color(item, "color", (uint32_t)0xFFFFFFFF);
        stop.position = (float)std::clamp(finite_or(json_double(item, "position", 0.5), 0.5), 0.0, 1.0);
        stop.opacity = (float)std::clamp(finite_or(json_double(item, "opacity", 1.0), 1.0), 0.0, 1.0);
        stops.push_back(stop);
    }
    std::sort(stops.begin(), stops.end(), [](const GradientStop &a, const GradientStop &b) {
        return a.position < b.position;
    });
    return stops;
}

static json bezier_path_points_to_json(const std::vector<BezierPathPoint> &points)
{
    json arr = json::array();
    for (const auto &point : points) {
        arr.push_back({{"x", point.x}, {"y", point.y},
                       {"in_x", point.in_x}, {"in_y", point.in_y},
                       {"out_x", point.out_x}, {"out_y", point.out_y},
                       {"has_in", point.has_in}, {"has_out", point.has_out},
                       {"smooth", point.smooth}, {"starts_subpath", point.starts_subpath},
                       {"corner_radius", point.corner_radius}});
    }
    return arr;
}

static std::vector<BezierPathPoint> bezier_path_points_from_json(const json &j)
{
    constexpr size_t kMaxPathPoints = 4096;
    constexpr double kMaxNormalizedCoordinate = 1024.0;
    std::vector<BezierPathPoint> points;
    if (!j.is_array()) return points;
    points.reserve(std::min(j.size(), kMaxPathPoints));
    for (size_t i = 0; i < j.size() && points.size() < kMaxPathPoints; ++i) {
        const auto &item = j[i];
        if (!item.is_object()) continue;
        BezierPathPoint point;
        point.x = std::clamp(finite_or(json_double(item, "x", 0.0), 0.0),
                             -kMaxNormalizedCoordinate, kMaxNormalizedCoordinate);
        point.y = std::clamp(finite_or(json_double(item, "y", 0.0), 0.0),
                             -kMaxNormalizedCoordinate, kMaxNormalizedCoordinate);
        point.in_x = std::clamp(finite_or(json_double(item, "in_x", point.x), point.x),
                                -kMaxNormalizedCoordinate, kMaxNormalizedCoordinate);
        point.in_y = std::clamp(finite_or(json_double(item, "in_y", point.y), point.y),
                                -kMaxNormalizedCoordinate, kMaxNormalizedCoordinate);
        point.out_x = std::clamp(finite_or(json_double(item, "out_x", point.x), point.x),
                                 -kMaxNormalizedCoordinate, kMaxNormalizedCoordinate);
        point.out_y = std::clamp(finite_or(json_double(item, "out_y", point.y), point.y),
                                 -kMaxNormalizedCoordinate, kMaxNormalizedCoordinate);
        point.has_in = json_bool(item, "has_in", false);
        point.has_out = json_bool(item, "has_out", false);
        point.smooth = json_bool(item, "smooth", false);
        point.starts_subpath = !points.empty() && json_bool(item, "starts_subpath", false);
        point.corner_radius = std::clamp(finite_or(json_double(item, "corner_radius", 0.0), 0.0),
                                         0.0, (double)kMaxCanvasDimension);
        if (!point.has_in) { point.in_x = point.x; point.in_y = point.y; }
        if (!point.has_out) { point.out_x = point.x; point.out_y = point.y; }
        points.push_back(point);
    }
    return points;
}

static json rich_fill_to_json(const RichTextFill &f)
{
    return {{"type", f.type}, {"color", f.color}, {"gradient_type", f.gradient_type},
            {"gradient_spread", f.gradient_spread},
            {"gradient_start_color", f.gradient_start_color}, {"gradient_end_color", f.gradient_end_color},
            {"gradient_start_pos", f.gradient_start_pos}, {"gradient_end_pos", f.gradient_end_pos},
            {"gradient_start_opacity", f.gradient_start_opacity}, {"gradient_end_opacity", f.gradient_end_opacity},
            {"gradient_opacity", f.gradient_opacity}, {"gradient_angle", f.gradient_angle},
            {"gradient_center_x", f.gradient_center_x}, {"gradient_center_y", f.gradient_center_y},
            {"gradient_scale", f.gradient_scale}, {"gradient_focal_x", f.gradient_focal_x},
            {"gradient_focal_y", f.gradient_focal_y}};
}

static RichTextFill rich_fill_from_json(const json &j, const RichTextFill &fallback = {})
{
    RichTextFill f = fallback;
    if (!j.is_object()) return f;
    f.type = std::clamp(json_int(j, "type", f.type), 0, 1);
    f.color = json_color(j, "color", f.color);
    f.gradient_spread = gradient_spread_from_json(j, "gradient_spread", "gradient_type", f.gradient_spread);
    f.gradient_type = normalize_gradient_type(json_int(j, "gradient_type", f.gradient_type));
    f.gradient_start_color = json_color(j, "gradient_start_color", f.gradient_start_color);
    f.gradient_end_color = json_color(j, "gradient_end_color", f.gradient_end_color);
    f.gradient_start_pos = (float)std::clamp(finite_or(json_double(j, "gradient_start_pos", f.gradient_start_pos), f.gradient_start_pos), 0.0, 1.0);
    f.gradient_end_pos = (float)std::clamp(finite_or(json_double(j, "gradient_end_pos", f.gradient_end_pos), f.gradient_end_pos), 0.0, 1.0);
    f.gradient_start_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_start_opacity", f.gradient_start_opacity), f.gradient_start_opacity), 0.0, 1.0);
    f.gradient_end_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_end_opacity", f.gradient_end_opacity), f.gradient_end_opacity), 0.0, 1.0);
    f.gradient_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_opacity", f.gradient_opacity), f.gradient_opacity), 0.0, 1.0);
    f.gradient_angle = (float)finite_or(json_double(j, "gradient_angle", f.gradient_angle), f.gradient_angle);
    f.gradient_center_x = (float)std::clamp(finite_or(json_double(j, "gradient_center_x", f.gradient_center_x), f.gradient_center_x), -100.0, 100.0);
    f.gradient_center_y = (float)std::clamp(finite_or(json_double(j, "gradient_center_y", f.gradient_center_y), f.gradient_center_y), -100.0, 100.0);
    f.gradient_scale = (float)std::clamp(finite_or(json_double(j, "gradient_scale", f.gradient_scale), f.gradient_scale), 0.01, 100.0);
    f.gradient_focal_x = (float)std::clamp(finite_or(json_double(j, "gradient_focal_x", f.gradient_focal_x), f.gradient_focal_x), -100.0, 100.0);
    f.gradient_focal_y = (float)std::clamp(finite_or(json_double(j, "gradient_focal_y", f.gradient_focal_y), f.gradient_focal_y), -100.0, 100.0);
    return f;
}

static json rich_stroke_to_json(const RichTextStroke &stroke)
{
    return {{"enabled", stroke.enabled}, {"width", stroke.width},
            {"opacity", stroke.opacity}, {"on_front", stroke.on_front},
            {"alignment", stroke.alignment}, {"antialias", stroke.antialias},
            {"join_style", stroke.join_style}, {"fill", rich_fill_to_json(stroke.fill)}};
}

static RichTextStroke rich_stroke_from_json(const json &j, const RichTextStroke &fallback = {})
{
    RichTextStroke stroke = fallback;
    if (!j.is_object()) return stroke;
    stroke.enabled = json_bool(j, "enabled", stroke.enabled);
    stroke.width = (float)std::clamp(finite_or(json_double(j, "width", stroke.width), stroke.width), 0.0, 200.0);
    stroke.opacity = (float)std::clamp(finite_or(json_double(j, "opacity", stroke.opacity), stroke.opacity), 0.0, 1.0);
    stroke.on_front = json_bool(j, "on_front", stroke.on_front);
    stroke.alignment = std::clamp(json_int(j, "alignment", stroke.alignment), 0, 2);
    stroke.antialias = json_bool(j, "antialias", stroke.antialias);
    stroke.join_style = std::clamp(json_int(j, "join_style", stroke.join_style), 0, 2);
    if (j.contains("fill")) stroke.fill = rich_fill_from_json(j["fill"], stroke.fill);
    return stroke;
}

static json rich_char_format_to_json(const RichTextCharFormat &f)
{
    return {{"font_family", f.font_family}, {"font_style", f.font_style},
            {"font_size", f.font_size}, {"bold", f.bold}, {"italic", f.italic},
            {"underline", f.underline}, {"strikethrough", f.strikethrough},
            {"kerning", f.kerning}, {"kerning_mode", f.kerning_mode},
            {"manual_kerning", f.manual_kerning}, {"tracking", f.tracking},
            {"scale_x", f.scale_x}, {"scale_y", f.scale_y},
            {"baseline_shift", f.baseline_shift}, {"text_style", f.text_style},
            {"ligatures", f.ligatures}, {"stylistic_alternates", f.stylistic_alternates},
            {"fractions", f.fractions}, {"opentype_features", f.opentype_features},
            {"language", f.language}, {"fill", rich_fill_to_json(f.fill)},
            {"stroke", rich_stroke_to_json(f.stroke)}};
}

static RichTextCharFormat rich_char_format_from_json(const json &j, const RichTextCharFormat &fallback = {})
{
    RichTextCharFormat f = fallback;
    if (!j.is_object()) return f;
    f.font_family = bounded_string(j, "font_family", f.font_family, kMaxNameLength);
    f.font_style = bounded_string(j, "font_style", f.font_style, kMaxNameLength);
    f.font_size = std::clamp(json_int(j, "font_size", f.font_size), 1, 512);
    f.bold = json_bool(j, "bold", f.bold);
    f.italic = json_bool(j, "italic", f.italic);
    f.underline = json_bool(j, "underline", f.underline);
    f.strikethrough = json_bool(j, "strikethrough", f.strikethrough);
    f.kerning = json_bool(j, "kerning", f.kerning);
    f.kerning_mode = std::clamp(json_int(j, "kerning_mode", f.kerning_mode), 0, 2);
    f.manual_kerning = (float)std::clamp(finite_or(json_double(j, "manual_kerning", f.manual_kerning), f.manual_kerning), -1000.0, 1000.0);
    f.tracking = (float)std::clamp(finite_or(json_double(j, "tracking", f.tracking), f.tracking), -1000.0, 1000.0);
    f.scale_x = (float)std::clamp(finite_or(json_double(j, "scale_x", f.scale_x), f.scale_x), 0.01, 100.0);
    f.scale_y = (float)std::clamp(finite_or(json_double(j, "scale_y", f.scale_y), f.scale_y), 0.01, 100.0);
    f.baseline_shift = (float)std::clamp(finite_or(json_double(j, "baseline_shift", f.baseline_shift), f.baseline_shift), -1000.0, 1000.0);
    f.text_style = std::clamp(json_int(j, "text_style", f.text_style), 0, 4);
    f.ligatures = json_bool(j, "ligatures", f.ligatures);
    f.stylistic_alternates = json_bool(j, "stylistic_alternates", f.stylistic_alternates);
    f.fractions = json_bool(j, "fractions", f.fractions);
    f.opentype_features = json_bool(j, "opentype_features", f.opentype_features);
    f.language = bounded_string(j, "language", f.language, kMaxNameLength);
    if (j.contains("fill")) f.fill = rich_fill_from_json(j["fill"], f.fill);
    if (j.contains("stroke")) f.stroke = rich_stroke_from_json(j["stroke"], f.stroke);
    return f;
}

static json rich_paragraph_format_to_json(const RichTextParagraphFormat &f)
{
    return {{"align_h", f.align_h}, {"align_v", f.align_v}, {"indent_left", f.indent_left},
            {"indent_right", f.indent_right}, {"indent_first_line", f.indent_first_line},
            {"line_spacing", f.line_spacing},
            {"space_before", f.space_before}, {"space_after", f.space_after}, {"hyphenate", f.hyphenate}};
}

static RichTextParagraphFormat rich_paragraph_format_from_json(const json &j, const RichTextParagraphFormat &fallback = {})
{
    RichTextParagraphFormat f = fallback;
    if (!j.is_object()) return f;
    f.align_h = std::clamp(json_int(j, "align_h", f.align_h), 0, 6);
    f.align_v = std::clamp(json_int(j, "align_v", f.align_v), 0, 3);
    f.indent_left = (float)std::clamp(finite_or(json_double(j, "indent_left", f.indent_left), f.indent_left), 0.0, 10000.0);
    f.indent_right = (float)std::clamp(finite_or(json_double(j, "indent_right", f.indent_right), f.indent_right), 0.0, 10000.0);
    f.indent_first_line = (float)std::clamp(finite_or(json_double(j, "indent_first_line", f.indent_first_line), f.indent_first_line), -10000.0, 10000.0);
    f.line_spacing = (float)std::clamp(finite_or(json_double(j, "line_spacing", f.line_spacing), f.line_spacing), -1000.0, 1000.0);
    f.space_before = (float)std::clamp(finite_or(json_double(j, "space_before", f.space_before), f.space_before), 0.0, 10000.0);
    f.space_after = (float)std::clamp(finite_or(json_double(j, "space_after", f.space_after), f.space_after), 0.0, 10000.0);
    f.hyphenate = json_bool(j, "hyphenate", f.hyphenate);
    return f;
}

static json rich_doc_to_json(const RichTextDocument &doc)
{
    json blocks = json::array();
    for (const auto &b : doc.blocks)
        blocks.push_back({{"start", b.start}, {"length", b.length}, {"mask", b.mask},
                          {"format", rich_paragraph_format_to_json(b.format)}});
    json ranges = json::array();
    for (const auto &r : doc.ranges)
        ranges.push_back({{"start", r.start}, {"length", r.length}, {"mask", r.mask},
                          {"format", rich_char_format_to_json(r.format)}});

    /* Runtime edit history is intentionally not persisted. Transactions use
     * Unicode-safe boundaries, but the title file contains canonical state only. */

    json auto_rules = json::array();
    for (const auto &rule : doc.auto_style_rules) {
        auto_rules.push_back({{"enabled", rule.enabled},
                              {"style_preset_id", rule.style_preset_id},
                              {"rule_id", rule.rule_id},
                              {"display_name", rule.display_name},
                              {"conflict_mode", rule.conflict_mode},
                              {"match_mode", rule.match_mode},
                              {"stop_processing", rule.stop_processing},
                              {"excludes_rule_ids", rule.excludes_rule_ids},
                              {"condition_type", rule.condition_type},
                              {"start_condition", rule.start_condition},
                              {"end_condition", rule.end_condition},
                              {"start_offset", rule.start_offset},
                              {"end_offset", rule.end_offset},
                              {"start_custom_chars", rule.start_custom_chars},
                              {"end_custom_chars", rule.end_custom_chars},
                              {"require_stop_match", rule.require_stop_match},
                              {"include_start_marker", rule.include_start_marker},
                              {"include_end_marker", rule.include_end_marker},
                              {"start", rule.start},
                              {"length", rule.length},
                              {"cached_mask", rule.cached_mask},
                              {"cached_format", rich_char_format_to_json(rule.cached_format)}});
    }
    return {{"version", doc.version}, {"plain_text", doc.plain_text},
            {"default_format", rich_char_format_to_json(doc.default_format)},
            {"default_paragraph_format", rich_paragraph_format_to_json(doc.default_paragraph_format)},
            {"has_typing_format", doc.has_typing_format},
            {"typing_format", rich_char_format_to_json(doc.has_typing_format ? doc.typing_format : doc.default_format)},
            {"typing_format_mask", doc.has_typing_format ? doc.typing_format_mask : 0u},
            {"blocks", blocks}, {"ranges", ranges},
            {"auto_style_enabled", doc.auto_style_enabled},
            {"auto_default_style_preset_id", doc.auto_default_style_preset_id},
            {"auto_default_style_cached_mask", doc.auto_default_style_cached_mask},
            {"auto_default_style_cached_format", rich_char_format_to_json(doc.auto_default_style_cached_format)},
            {"auto_style_rules", auto_rules},
            {"selection", {{"anchor", doc.selection.anchor}, {"head", doc.selection.head}}}};
}

static RichTextDocument rich_doc_from_json(const json &j, const Layer &layer)
{
    RichTextDocument doc = rich_text_document_from_layer_defaults(layer);
    if (!j.is_object()) return doc;
    doc.version = std::clamp(json_int(j, "version", 1), 1, 2);
    doc.plain_text = bounded_string(j, "plain_text", doc.plain_text, kMaxTextLength);
    if (j.contains("default_format")) doc.default_format = rich_char_format_from_json(j["default_format"], doc.default_format);
    if (j.contains("default_paragraph_format")) doc.default_paragraph_format = rich_paragraph_format_from_json(j["default_paragraph_format"], doc.default_paragraph_format);
    doc.has_typing_format = json_bool(j, "has_typing_format", false);
    doc.typing_format = j.contains("typing_format") ? rich_char_format_from_json(j["typing_format"], doc.default_format) : doc.default_format;
    doc.typing_format_mask = j.contains("typing_format_mask")
        ? (uint32_t)std::clamp(json_int(j, "typing_format_mask", 0), 0, (int)RichTextCharAll)
        : (doc.has_typing_format
               ? rich_text_char_format_difference_mask(doc.typing_format, doc.default_format)
               : 0u);
    doc.blocks.clear();
    if (j.contains("blocks") && j["blocks"].is_array()) {
        for (size_t i = 0; i < std::min(j["blocks"].size(), kMaxTextLength); ++i) {
            const auto &bj = j["blocks"][i];
            if (!bj.is_object()) continue;
            RichTextBlock block;
            block.start = (size_t)std::clamp(json_int(bj, "start", 0), 0, (int)kMaxTextLength);
            block.length = (size_t)std::clamp(json_int(bj, "length", 0), 0, (int)kMaxTextLength);
            block.format = bj.contains("format")
                ? rich_paragraph_format_from_json(bj["format"], doc.default_paragraph_format)
                : doc.default_paragraph_format;
            /* Version 1 paragraph blocks had no override mask. Infer the
             * sparse fields that differ from the document default. */
            block.mask = bj.contains("mask")
                ? (uint32_t)std::clamp(json_int(bj, "mask", (int)RichTextParagraphAll),
                                       0, (int)RichTextParagraphAll)
                : rich_text_paragraph_format_difference_mask(block.format,
                                                              doc.default_paragraph_format);
            doc.blocks.push_back(block);
        }
    }
    doc.ranges.clear();
    if (j.contains("ranges") && j["ranges"].is_array()) {
        for (size_t i = 0; i < std::min(j["ranges"].size(), kMaxTextLength); ++i) {
            const auto &rj = j["ranges"][i];
            if (!rj.is_object()) continue;
            RichTextRange r;
            r.start = (size_t)std::clamp(json_int(rj, "start", 0), 0, (int)kMaxTextLength);
            r.length = (size_t)std::clamp(json_int(rj, "length", 0), 0, (int)kMaxTextLength);
            r.format = rj.contains("format") ? rich_char_format_from_json(rj["format"], doc.default_format) : doc.default_format;
            /* Version 1 stored complete style snapshots. Since it had no
             * explicit override intent, infer the sparse fields that differ
             * from the document default during migration. */
            r.mask = rj.contains("mask")
                ? (uint32_t)std::clamp(json_int(rj, "mask", (int)RichTextCharAll), 0, (int)RichTextCharAll)
                : rich_text_char_format_difference_mask(r.format, doc.default_format);
            doc.ranges.push_back(r);
        }
    }
    doc.auto_style_enabled = json_bool(j, "auto_style_enabled", false);
    doc.auto_default_style_preset_id = bounded_string(j, "auto_default_style_preset_id", "", kMaxNameLength);
    doc.auto_default_style_cached_mask = (uint32_t)std::clamp(json_int(j, "auto_default_style_cached_mask", 0), 0, (int)RichTextCharAll);
    doc.auto_default_style_cached_format = j.contains("auto_default_style_cached_format")
        ? rich_char_format_from_json(j["auto_default_style_cached_format"], doc.default_format)
        : doc.default_format;
    doc.auto_style_rules.clear();
    if (j.contains("auto_style_rules") && j["auto_style_rules"].is_array()) {
        for (size_t i = 0; i < std::min(j["auto_style_rules"].size(), (size_t)128); ++i) {
            const auto &rj = j["auto_style_rules"][i];
            if (!rj.is_object()) continue;
            RichTextAutoStyleRule rule;
            rule.enabled = json_bool(rj, "enabled", true);
            rule.style_preset_id = bounded_string(rj, "style_preset_id", "", kMaxNameLength);
            rule.rule_id = bounded_string(rj, "rule_id", "", kMaxNameLength);
            rule.display_name = bounded_string(rj, "display_name", "", kMaxNameLength);
            rule.conflict_mode = bounded_string(rj, "conflict_mode", "override_previous", kMaxNameLength);
            rule.match_mode = bounded_string(rj, "match_mode", "all_matches", kMaxNameLength);
            rule.stop_processing = json_bool(rj, "stop_processing", false);
            rule.excludes_rule_ids.clear();
            if (rj.contains("excludes_rule_ids") && rj["excludes_rule_ids"].is_array()) {
                for (size_t xi = 0; xi < std::min(rj["excludes_rule_ids"].size(), (size_t)64); ++xi) {
                    if (rj["excludes_rule_ids"][xi].is_string())
                        rule.excludes_rule_ids.push_back(rj["excludes_rule_ids"][xi].get<std::string>());
                }
            }
            if (rule.rule_id.empty()) rule.rule_id = std::to_string(i + 1);
            rule.condition_type = bounded_string(rj, "condition_type", "range_markers", kMaxNameLength);
            rule.start_condition = bounded_string(rj, "start_condition", "text_start", kMaxNameLength);
            rule.end_condition = bounded_string(rj, "end_condition", "character_index", kMaxNameLength);
            rule.start_offset = (size_t)std::clamp(json_int(rj, "start_offset", 0), 0, (int)kMaxTextLength);
            rule.end_offset = (size_t)std::clamp(json_int(rj, "end_offset", json_int(rj, "length", 0)), 0, (int)kMaxTextLength);
            rule.start_custom_chars = bounded_string(rj, "start_custom_chars", "", 16);
            rule.end_custom_chars = bounded_string(rj, "end_custom_chars", "", 16);
            rule.require_stop_match = json_bool(rj, "require_stop_match", true);
            rule.include_start_marker = json_bool(rj, "include_start_marker", true);
            rule.include_end_marker = json_bool(rj, "include_end_marker", false);
            rule.start = (size_t)std::clamp(json_int(rj, "start", 0), 0, (int)kMaxTextLength);
            rule.length = (size_t)std::clamp(json_int(rj, "length", 0), 0, (int)kMaxTextLength);
            if (!rj.contains("start_condition") && !rj.contains("end_condition") && rule.condition_type == "start_to_char") {
                rule.start_condition = "text_start";
                rule.end_condition = "character_index";
                rule.start_offset = 0;
                rule.end_offset = rule.length;
            }
            rule.cached_mask = (uint32_t)std::clamp(json_int(rj, "cached_mask", 0), 0, (int)RichTextCharAll);
            rule.cached_format = rj.contains("cached_format") ? rich_char_format_from_json(rj["cached_format"], doc.default_format) : doc.default_format;
            doc.auto_style_rules.push_back(rule);
        }
    }
    if (j.contains("selection") && j["selection"].is_object()) {
        doc.selection.anchor = (size_t)std::clamp(json_int(j["selection"], "anchor", 0), 0, (int)kMaxTextLength);
        doc.selection.head = (size_t)std::clamp(json_int(j["selection"], "head", 0), 0, (int)kMaxTextLength);
    }
    /* Ignore transactions embedded by older files. They describe a previous
     * editing session and are not part of the canonical document contract. */
    doc.transactions.clear();
    doc.normalize();
    return doc;
}

static json layer_to_json(const Layer &l, bool include_embedded_assets = true,
                          bool require_embedded_assets = false, std::string *error = nullptr,
                          bool *asset_embed_failed = nullptr)
{
    json j;
    j["id"]       = l.id;
    j["name"]     = l.name;
    j["type"]     = (int)l.type;
    j["visible"]  = l.visible;
    j["locked"]   = l.locked;
    j["properties_expanded"] = l.properties_expanded;
    j["parent_id"] = l.parent_id;
    j["mask_source_id"] = l.mask_source_id;
    j["mask_mode"] = (int)l.mask_mode;
    j["blend_mode"] = (int)l.blend_mode;
    j["use_as_scene_mask"] = l.use_as_scene_mask;
    j["effect_stack_respects_masks"] = l.effect_stack_respects_masks;
    json effects = json::array();
    for (const auto &effect : l.effects) {
        effects.push_back({{"type", (int)effect.type},
                           {"enabled", effect.enabled},
                           {"brightness", effect.brightness},
                           {"contrast", effect.contrast},
                           {"saturation", effect.saturation},
                           {"tint_color", effect.tint_color},
                           {"tint_amount", effect.tint_amount},
                           {"effect_color", effect.effect_color},
                           {"effect_opacity", effect.effect_opacity},
                           {"effect_size", effect.effect_size},
                           {"effect_distance", effect.effect_distance},
                           {"effect_angle", effect.effect_angle},
                           {"effect_spread", effect.effect_spread},
                           {"effect_falloff", effect.effect_falloff},
                           {"effect_blur_type", effect.effect_blur_type},
                           {"effect_samples", effect.effect_samples},
                           {"effect_centered", effect.effect_centered},
                           {"blend_mode", (int)effect.blend_mode},
                           {"effect_fill_type", effect.effect_fill_type},
                           {"effect_join_style", effect.effect_join_style},
                           {"effect_on_front", effect.effect_on_front},
                           {"effect_antialias", effect.effect_antialias},
                           {"effect_stroke_color", effect.effect_stroke_color},
                           {"effect_stroke_width", effect.effect_stroke_width},
                           {"effect_stroke_opacity", effect.effect_stroke_opacity},
                           {"effect_padding_left", effect.effect_padding_left},
                           {"effect_padding_right", effect.effect_padding_right},
                           {"effect_padding_top", effect.effect_padding_top},
                           {"effect_padding_bottom", effect.effect_padding_bottom},
                           {"effect_corner_radius_tl", effect.effect_corner_radius_tl},
                           {"effect_corner_radius_tr", effect.effect_corner_radius_tr},
                           {"effect_corner_radius_br", effect.effect_corner_radius_br},
                           {"effect_corner_radius_bl", effect.effect_corner_radius_bl},
                           {"effect_corner_type", effect.effect_corner_type},
                           {"effect_gradient_type", effect.effect_gradient_type},
                           {"effect_gradient_spread", effect.effect_gradient_spread},
                           {"effect_gradient_start_color", effect.effect_gradient_start_color},
                           {"effect_gradient_end_color", effect.effect_gradient_end_color},
                           {"effect_gradient_start_pos", effect.effect_gradient_start_pos},
                           {"effect_gradient_end_pos", effect.effect_gradient_end_pos},
                           {"effect_gradient_start_opacity", effect.effect_gradient_start_opacity},
                           {"effect_gradient_end_opacity", effect.effect_gradient_end_opacity},
                           {"effect_gradient_opacity", effect.effect_gradient_opacity},
                           {"effect_gradient_angle", effect.effect_gradient_angle},
                           {"effect_gradient_center_x", effect.effect_gradient_center_x},
                           {"effect_gradient_center_y", effect.effect_gradient_center_y},
                           {"effect_gradient_scale", effect.effect_gradient_scale},
                           {"effect_gradient_focal_x", effect.effect_gradient_focal_x},
                           {"effect_gradient_focal_y", effect.effect_gradient_focal_y},
                           {"enabled_prop", aprop_to_json(effect.enabled_prop)},
                           {"opacity_prop", aprop_to_json(effect.opacity_prop)},
                           {"size_prop", aprop_to_json(effect.size_prop)},
                           {"distance_prop", aprop_to_json(effect.distance_prop)},
                           {"angle_prop", aprop_to_json(effect.angle_prop)},
                           {"spread_prop", aprop_to_json(effect.spread_prop)},
                           {"falloff_prop", aprop_to_json(effect.falloff_prop)},
                           {"stroke_width_prop", aprop_to_json(effect.stroke_width_prop)},
                           {"stroke_opacity_prop", aprop_to_json(effect.stroke_opacity_prop)},
                           {"padding_left_prop", aprop_to_json(effect.padding_left_prop)},
                           {"padding_right_prop", aprop_to_json(effect.padding_right_prop)},
                           {"padding_top_prop", aprop_to_json(effect.padding_top_prop)},
                           {"padding_bottom_prop", aprop_to_json(effect.padding_bottom_prop)},
                           {"corner_radius_tl_prop", aprop_to_json(effect.corner_radius_tl_prop)},
                           {"corner_radius_tr_prop", aprop_to_json(effect.corner_radius_tr_prop)},
                           {"corner_radius_br_prop", aprop_to_json(effect.corner_radius_br_prop)},
                           {"corner_radius_bl_prop", aprop_to_json(effect.corner_radius_bl_prop)},
                           {"color_a", aprop_to_json(effect.color_a)},
                           {"color_r", aprop_to_json(effect.color_r)},
                           {"color_g", aprop_to_json(effect.color_g)},
                           {"color_b", aprop_to_json(effect.color_b)},
                           {"stroke_color_a", aprop_to_json(effect.stroke_color_a)},
                           {"stroke_color_r", aprop_to_json(effect.stroke_color_r)},
                           {"stroke_color_g", aprop_to_json(effect.stroke_color_g)},
                           {"stroke_color_b", aprop_to_json(effect.stroke_color_b)}});
    }
    j["effects"] = effects;
    json transitions = json::array();
    for (const auto &transition : l.transitions) {
        transitions.push_back({
            {"id", transition.id},
            {"preset_id", transition.preset_id},
            {"display_name", transition.display_name},
            {"enabled", transition.enabled},
            {"kind", (int)transition.kind},
            {"type", (int)transition.type},
            {"edge", (int)transition.edge},
            {"unit", (int)transition.unit},
            {"direction", (int)transition.direction},
            {"easing", (int)transition.easing},
            {"duration", transition.duration},
            {"blur_amount", transition.blur_amount},
            {"scale_from", transition.scale_from},
            {"offset", transition.offset},
            {"stagger", transition.stagger},
            {"softness", transition.softness},
            {"reverse_order", transition.reverse_order},
        });
    }
    j["transitions"] = transitions;
    j["in_time"]  = l.in_time;
    j["out_time"] = l.out_time;

    j["position"] = vec2_aprop_to_json(l.position);
    j["scale"]    = vec2_aprop_to_json(l.scale);
    j["scale_lock"] = l.scale_lock;
    j["rotation"] = aprop_to_json(l.rotation);
    j["opacity"]  = aprop_to_json(l.opacity);

    j["text_content"]  = l.text_content;
    /* rich_text is the only style source of truth; do not serialize legacy HTML. */
    j["rich_text"] = rich_doc_to_json(l.rich_text);
    j["clock_format"]  = l.clock_format;
    j["expose_text"]   = l.expose_text;
    j["exposed_hide_if_empty"] = l.exposed_hide_if_empty;
    j["exposed_single_value"] = l.exposed_single_value;
    j["ignore_persistence"] = l.ignore_persistence;
    j["font_family"]   = l.font_family;
    j["font_style"]    = l.font_style;
    j["font_size"]     = l.font_size;
    j["font_size_prop"] = aprop_to_json(l.font_size_prop);
    j["font_bold"]     = l.font_bold;
    j["font_italic"]   = l.font_italic;
    j["font_kerning"]  = l.font_kerning;
    j["kerning_mode"]  = l.kerning_mode;
    j["manual_kerning"] = l.manual_kerning;
    j["text_leading"]  = l.text_leading;
    j["char_tracking"] = l.char_tracking;
    j["char_tracking_prop"] = aprop_to_json(l.char_tracking_prop);
    j["char_scale_x"]  = l.char_scale_x;
    j["char_scale_x_prop"] = aprop_to_json(l.char_scale_x_prop);
    j["char_scale_y"]  = l.char_scale_y;
    j["char_scale_y_prop"] = aprop_to_json(l.char_scale_y_prop);
    j["baseline_shift"] = l.baseline_shift;
    j["baseline_shift_prop"] = aprop_to_json(l.baseline_shift_prop);
    j["text_style"]    = l.text_style;
    j["text_underline"] = l.text_underline;
    j["text_strikethrough"] = l.text_strikethrough;
    j["text_ligatures"] = l.text_ligatures;
    j["text_stylistic_alternates"] = l.text_stylistic_alternates;
    j["text_fractions"] = l.text_fractions;
    j["text_opentype_features"] = l.text_opentype_features;
    j["text_language"] = l.text_language;
    j["text_overflow_mode"] = l.text_overflow_mode;
    j["text_fit_min_scale"] = l.text_fit_min_scale;
    j["text_box_width_to_text"] = l.text_box_width_to_text;
    j["text_box_height_to_text"] = l.text_box_height_to_text;
    j["max_text_box_width"] = l.max_text_box_width;
    j["max_text_box_height"] = l.max_text_box_height;
    j["ticker_style"] = l.ticker_style;
    j["ticker_speed"] = l.ticker_speed;
    j["ticker_line_hold"] = l.ticker_line_hold;
    j["ticker_direction"] = l.ticker_direction;
    j["text_color"]    = l.text_color;
    j["outline_enabled"] = l.outline_enabled;
    j["stroke_fill_type"] = l.stroke_fill_type;
    j["stroke_color"]  = l.stroke_color;
    j["stroke_width"]  = l.stroke_width;
    j["outline_opacity"] = l.outline_opacity;
    j["outline_join_style"] = l.outline_join_style;
    j["outline_on_front"] = l.outline_on_front;
    j["outline_alignment"] = l.outline_alignment;
    j["outline_antialias"] = l.outline_antialias;
    j["stroke_gradient_type"] = l.stroke_gradient_type;
    j["stroke_gradient_spread"] = l.stroke_gradient_spread;
    j["stroke_gradient_start_color"] = l.stroke_gradient_start_color;
    j["stroke_gradient_end_color"] = l.stroke_gradient_end_color;
    j["stroke_gradient_start_pos"] = l.stroke_gradient_start_pos;
    j["stroke_gradient_end_pos"] = l.stroke_gradient_end_pos;
    j["stroke_gradient_start_opacity"] = l.stroke_gradient_start_opacity;
    j["stroke_gradient_end_opacity"] = l.stroke_gradient_end_opacity;
    j["stroke_gradient_opacity"] = l.stroke_gradient_opacity;
    j["stroke_gradient_angle"] = l.stroke_gradient_angle;
    j["stroke_gradient_center_x"] = l.stroke_gradient_center_x;
    j["stroke_gradient_center_y"] = l.stroke_gradient_center_y;
    j["stroke_gradient_scale"] = l.stroke_gradient_scale;
    j["stroke_gradient_focal_x"] = l.stroke_gradient_focal_x;
    j["stroke_gradient_focal_y"] = l.stroke_gradient_focal_y;
    j["stroke_gradient_stops"] = gradient_stops_to_json(l.stroke_gradient_stops);
    j["align_h"]       = l.align_h;
    j["align_v"]       = l.align_v;
    j["paragraph_indent_left"] = l.paragraph_indent_left;
    j["paragraph_indent_right"] = l.paragraph_indent_right;
    j["paragraph_indent_first_line"] = l.paragraph_indent_first_line;
    j["paragraph_indent_left_prop"] = aprop_to_json(l.paragraph_indent_left_prop);
    j["paragraph_indent_right_prop"] = aprop_to_json(l.paragraph_indent_right_prop);
    j["paragraph_indent_first_line_prop"] = aprop_to_json(l.paragraph_indent_first_line_prop);
    j["paragraph_space_before"] = l.paragraph_space_before;
    j["paragraph_space_before_prop"] = aprop_to_json(l.paragraph_space_before_prop);
    j["paragraph_space_after"] = l.paragraph_space_after;
    j["paragraph_space_after_prop"] = aprop_to_json(l.paragraph_space_after_prop);
    j["paragraph_hyphenate"] = l.paragraph_hyphenate;

    j["fill_color"]    = l.fill_color;
    j["fill_type"]     = l.fill_type;
    j["gradient_type"] = l.gradient_type;
    j["gradient_spread"] = l.gradient_spread;
    j["gradient_start_color"] = l.gradient_start_color;
    j["gradient_end_color"] = l.gradient_end_color;
    j["gradient_start_pos"] = l.gradient_start_pos;
    j["gradient_end_pos"] = l.gradient_end_pos;
    j["gradient_start_opacity"] = l.gradient_start_opacity;
    j["gradient_end_opacity"] = l.gradient_end_opacity;
    j["gradient_opacity"] = l.gradient_opacity;
    j["gradient_angle"] = l.gradient_angle;
    j["gradient_center_x"] = l.gradient_center_x;
    j["gradient_center_y"] = l.gradient_center_y;
    j["gradient_scale"] = l.gradient_scale;
    j["gradient_focal_x"] = l.gradient_focal_x;
    j["gradient_focal_y"] = l.gradient_focal_y;
    j["gradient_stops"] = gradient_stops_to_json(l.gradient_stops);
    j["background_enabled"] = l.background_enabled;
    j["background_color"] = l.background_color;
    j["background_opacity"] = l.background_opacity;
    j["background_padding"] = l.background_padding_x;
    j["background_padding_x"] = l.background_padding_x;
    j["background_padding_y"] = l.background_padding_y;
    j["background_padding_left"] = l.background_padding_left;
    j["background_padding_right"] = l.background_padding_right;
    j["background_padding_top"] = l.background_padding_top;
    j["background_padding_bottom"] = l.background_padding_bottom;
    j["background_corner_radius"] = l.background_corner_radius;
    j["background_corner_radius_tl"] = l.background_corner_radius_tl;
    j["background_corner_radius_tr"] = l.background_corner_radius_tr;
    j["background_corner_radius_br"] = l.background_corner_radius_br;
    j["background_corner_radius_bl"] = l.background_corner_radius_bl;
    j["background_corner_type"] = (int)l.background_corner_type;
    j["background_fill_type"] = l.background_fill_type;
    j["background_stroke_color"] = l.background_stroke_color;
    j["background_stroke_width"] = l.background_stroke_width;
    j["background_stroke_opacity"] = l.background_stroke_opacity;
    j["background_stroke_fill_type"] = l.background_stroke_fill_type;
    j["background_gradient_type"] = l.background_gradient_type;
    j["background_gradient_spread"] = l.background_gradient_spread;
    j["background_gradient_start_color"] = l.background_gradient_start_color;
    j["background_gradient_end_color"] = l.background_gradient_end_color;
    j["background_gradient_start_pos"] = l.background_gradient_start_pos;
    j["background_gradient_end_pos"] = l.background_gradient_end_pos;
    j["background_gradient_start_opacity"] = l.background_gradient_start_opacity;
    j["background_gradient_end_opacity"] = l.background_gradient_end_opacity;
    j["background_gradient_opacity"] = l.background_gradient_opacity;
    j["background_gradient_angle"] = l.background_gradient_angle;
    j["background_gradient_center_x"] = l.background_gradient_center_x;
    j["background_gradient_center_y"] = l.background_gradient_center_y;
    j["background_gradient_scale"] = l.background_gradient_scale;
    j["background_gradient_focal_x"] = l.background_gradient_focal_x;
    j["background_gradient_focal_y"] = l.background_gradient_focal_y;
    j["background_gradient_stops"] = gradient_stops_to_json(l.background_gradient_stops);
    j["background_enabled_prop"] = aprop_to_json(l.background_enabled_prop);
    j["background_opacity_prop"] = aprop_to_json(l.background_opacity_prop);
    j["background_padding_x_prop"] = aprop_to_json(l.background_padding_x_prop);
    j["background_padding_y_prop"] = aprop_to_json(l.background_padding_y_prop);
    j["background_padding_left_prop"] = aprop_to_json(l.background_padding_left_prop);
    j["background_padding_right_prop"] = aprop_to_json(l.background_padding_right_prop);
    j["background_padding_top_prop"] = aprop_to_json(l.background_padding_top_prop);
    j["background_padding_bottom_prop"] = aprop_to_json(l.background_padding_bottom_prop);
    j["background_corner_radius_prop"] = aprop_to_json(l.background_corner_radius_prop);
    j["background_corner_radius_tl_prop"] = aprop_to_json(l.background_corner_radius_tl_prop);
    j["background_corner_radius_tr_prop"] = aprop_to_json(l.background_corner_radius_tr_prop);
    j["background_corner_radius_br_prop"] = aprop_to_json(l.background_corner_radius_br_prop);
    j["background_corner_radius_bl_prop"] = aprop_to_json(l.background_corner_radius_bl_prop);
    j["background_stroke_width_prop"] = aprop_to_json(l.background_stroke_width_prop);
    j["background_stroke_opacity_prop"] = aprop_to_json(l.background_stroke_opacity_prop);
    j["background_color_a"] = aprop_to_json(l.background_color_a);
    j["background_color_r"] = aprop_to_json(l.background_color_r);
    j["background_color_g"] = aprop_to_json(l.background_color_g);
    j["background_color_b"] = aprop_to_json(l.background_color_b);
    j["background_stroke_color_a"] = aprop_to_json(l.background_stroke_color_a);
    j["background_stroke_color_r"] = aprop_to_json(l.background_stroke_color_r);
    j["background_stroke_color_g"] = aprop_to_json(l.background_stroke_color_g);
    j["background_stroke_color_b"] = aprop_to_json(l.background_stroke_color_b);
    j["rect_width"]    = l.rect_width;
    j["rect_height"]   = l.rect_height;
    j["corner_radius"] = l.corner_radius;
    j["corner_radius_tl"] = l.corner_radius_tl;
    j["corner_radius_tr"] = l.corner_radius_tr;
    j["corner_radius_br"] = l.corner_radius_br;
    j["corner_radius_bl"] = l.corner_radius_bl;
    j["corner_radius_locked"] = l.corner_radius_locked;
    j["corner_bevel_roundness"] = l.corner_bevel_roundness;
    j["shape_type"] = (int)l.shape_type;
    j["path_points"] = bezier_path_points_to_json(l.path_points);
    j["path_closed"] = l.path_closed;
    j["shape_points"] = l.shape_points;
    j["shape_sides"] = l.shape_sides;
    j["shape_inner_radius"] = l.shape_inner_radius;
    j["shape_outer_radius"] = l.shape_outer_radius;
    j["shape_roundness"] = l.shape_roundness;
    j["shape_inner_roundness"] = l.shape_inner_roundness;
    j["scale_stroke_with_shape"] = l.scale_stroke_with_shape;
    j["scale_corners_with_shape"] = l.scale_corners_with_shape;
    j["size"]          = vec2_aprop_to_json(l.size);
    j["origin"]        = vec2_aprop_to_json(l.origin_prop);
    j["origin_x"]      = l.origin_x;
    j["origin_y"]      = l.origin_y;
    j["shadow_enabled"] = l.shadow_enabled;
    j["shadow_color"] = l.shadow_color;
    j["shadow_opacity"] = l.shadow_opacity;
    j["shadow_distance"] = l.shadow_distance;
    j["shadow_angle"] = l.shadow_angle;
    j["shadow_blur"] = l.shadow_blur;
    j["shadow_spread"] = l.shadow_spread;
    j["shadow_blur_type"] = (int)l.shadow_blur_type;
    j["long_shadow_enabled"] = l.long_shadow_enabled;
    j["long_shadow_color"] = l.long_shadow_color;
    j["long_shadow_opacity"] = l.long_shadow_opacity;
    j["long_shadow_length"] = l.long_shadow_length;
    j["long_shadow_angle"] = l.long_shadow_angle;
    j["long_shadow_falloff"] = l.long_shadow_falloff;
    j["long_shadow_blur_type"] = (int)l.long_shadow_blur_type;
    j["long_shadow_blur"] = l.long_shadow_blur;
    j["shadow_enabled_prop"] = aprop_to_json(l.shadow_enabled_prop);
    j["shadow_opacity_prop"] = aprop_to_json(l.shadow_opacity_prop);
    j["shadow_distance_prop"] = aprop_to_json(l.shadow_distance_prop);
    j["shadow_angle_prop"] = aprop_to_json(l.shadow_angle_prop);
    j["shadow_blur_prop"] = aprop_to_json(l.shadow_blur_prop);
    j["shadow_spread_prop"] = aprop_to_json(l.shadow_spread_prop);
    j["shadow_color_a"] = aprop_to_json(l.shadow_color_a);
    j["shadow_color_r"] = aprop_to_json(l.shadow_color_r);
    j["shadow_color_g"] = aprop_to_json(l.shadow_color_g);
    j["shadow_color_b"] = aprop_to_json(l.shadow_color_b);
    j["text_color_a"]  = aprop_to_json(l.text_color_a);
    j["text_color_r"]  = aprop_to_json(l.text_color_r);
    j["text_color_g"]  = aprop_to_json(l.text_color_g);
    j["text_color_b"]  = aprop_to_json(l.text_color_b);
    j["fill_color_a"]  = aprop_to_json(l.fill_color_a);
    j["fill_color_r"]  = aprop_to_json(l.fill_color_r);
    j["fill_color_g"]  = aprop_to_json(l.fill_color_g);
    j["fill_color_b"]  = aprop_to_json(l.fill_color_b);
    j["image_path"]    = l.image_path;
    j["scale_filter"]  = (int)l.scale_filter;
    j["image_box_lock_aspect_ratio"] = l.image_box_lock_aspect_ratio;
    j["image_box_mode"] = (int)l.image_box_mode;
    j["image_size_auto_fit"] = l.image_size_auto_fit;
    j["image_crop_when_outside_box"] = l.image_crop_when_outside_box;
    j["image_anchor_x"] = l.image_anchor_x;
    j["image_anchor_y"] = l.image_anchor_y;
    j["image_width"] = l.image_width;
    j["image_height"] = l.image_height;
    j["image_size"] = vec2_aprop_to_json(l.image_size);
    if (include_embedded_assets && !attach_embedded_image_asset(l, j, require_embedded_assets, error)) {
        if (asset_embed_failed)
            *asset_embed_failed = true;
    }
    j["lock_aspect_ratio"] = l.lock_aspect_ratio;
    return j;
}

std::string layer_render_fingerprint(const Layer &layer)
{
    json j = layer_to_json(layer, false, false, nullptr, nullptr);
    static constexpr const char *kCompositorOnlyKeys[] = {
        "id", "name", "visible", "locked", "properties_expanded",
        "parent_id", "mask_source_id", "mask_mode", "blend_mode",
        "use_as_scene_mask", "effect_stack_respects_masks",
        "in_time", "out_time", "position", "scale", "scale_lock",
        "rotation", "opacity", "expose_text", "exposed_hide_if_empty",
        "exposed_single_value", "ignore_persistence"
    };
    for (const char *key : kCompositorOnlyKeys)
        j.erase(key);

    /* General transitions are implemented as GPU matrices/opacity. Text
     * transitions remain in the fingerprint because they alter glyph coverage. */
    if (j.contains("transitions") && j["transitions"].is_array()) {
        json raster_transitions = json::array();
        for (const auto &transition : j["transitions"]) {
            if (!transition.is_object() ||
                json_int(transition, "kind", 0) !=
                    static_cast<int>(LayerTransitionKind::General))
                raster_transitions.push_back(transition);
        }
        j["transitions"] = std::move(raster_transitions);
    }

    const std::string serialized = j.dump();
    return std::to_string(std::hash<std::string>{}(serialized));
}

static std::shared_ptr<Layer> layer_from_json(const json &j, bool require_embedded_assets = false,
                                               std::string *error = nullptr)
{
    auto l = std::make_shared<Layer>();
    if (!j.is_object())
        return l;

    l->id       = bounded_string(j, "id", "", kMaxNameLength);
    l->name     = bounded_string(j, "name", "Layer", kMaxNameLength);
    l->type     = (LayerType)std::clamp(json_int(j, "type", 0), 0, (int)LayerType::Ticker);
    l->visible  = json_bool(j, "visible", true);
    l->locked   = json_bool(j, "locked", false);
    l->properties_expanded = json_bool(j, "properties_expanded", false);
    l->parent_id = bounded_string(j, "parent_id", "", kMaxNameLength);
    l->mask_source_id = bounded_string(j, "mask_source_id", "", kMaxNameLength);
    l->mask_mode = (MaskMode)std::clamp(json_int(j, "mask_mode", 0), 0, (int)MaskMode::InvertedLuma);
    if (l->mask_source_id.empty()) l->mask_mode = MaskMode::None;
    l->blend_mode = (EffectBlendMode)std::clamp(json_int(j, "blend_mode", (int)EffectBlendMode::Normal), 0, (int)EffectBlendMode::Color);
    l->use_as_scene_mask = json_bool(j, "use_as_scene_mask", false);
    l->effect_stack_respects_masks = json_bool(j, "effect_stack_respects_masks", false);
    if (j.contains("effects") && j["effects"].is_array()) {
        const size_t count = std::min(j["effects"].size(), (size_t)64);
        l->effects.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto &effect_json = j["effects"][i];
            if (!effect_json.is_object()) continue;
            LayerEffect effect;
            effect.type = (LayerEffectType)std::clamp(json_int(effect_json, "type", 0), 0, 13);
            effect.enabled = json_bool(effect_json, "enabled", true);
            switch (effect.type) {
            case LayerEffectType::DropShadow:
            case LayerEffectType::LongShadow:
            case LayerEffectType::InnerShadow:
                effect.blend_mode = EffectBlendMode::Multiply;
                break;
            case LayerEffectType::ColorOverlay:
                effect.blend_mode = EffectBlendMode::Color;
                break;
            case LayerEffectType::Glow:
            case LayerEffectType::InnerGlow:
                effect.blend_mode = EffectBlendMode::Additive;
                break;
            case LayerEffectType::Blur:
                effect.effect_opacity = 1.0f;
                break;
            case LayerEffectType::MotionBlur:
                effect.effect_opacity = 1.0f;
                effect.effect_size = 180.0f;
                effect.effect_angle = 0.0f;
                break;
            default:
                effect.blend_mode = EffectBlendMode::Normal;
                break;
            }
            effect.brightness = (float)std::clamp(finite_or(json_double(effect_json, "brightness", 0.0), 0.0), -1.0, 1.0);
            effect.contrast = (float)std::clamp(finite_or(json_double(effect_json, "contrast", 1.0), 1.0), 0.0, 4.0);
            effect.saturation = (float)std::clamp(finite_or(json_double(effect_json, "saturation", 1.0), 1.0), 0.0, 4.0);
            effect.tint_color = json_color(effect_json, "tint_color", (uint32_t)0xFFFFFFFF);
            effect.tint_amount = (float)std::clamp(finite_or(json_double(effect_json, "tint_amount", 1.0), 1.0), 0.0, 1.0);
            effect.effect_color = json_color(effect_json, "effect_color", effect.tint_color);
            effect.effect_opacity = (float)std::clamp(finite_or(json_double(effect_json, "effect_opacity", effect.tint_amount), effect.tint_amount), 0.0, 1.0);
            effect.effect_size = (float)std::clamp(finite_or(json_double(effect_json, "effect_size", 16.0), 16.0), 0.0, 512.0);
            effect.effect_distance = (float)std::clamp(finite_or(json_double(effect_json, "effect_distance", 8.0), 8.0), 0.0, 4096.0);
            const double default_effect_angle = effect.type == LayerEffectType::MotionBlur ? 0.0 : 135.0;
            effect.effect_angle = (float)finite_or(json_double(effect_json, "effect_angle", default_effect_angle), default_effect_angle);
            effect.effect_spread = (float)std::clamp(finite_or(json_double(effect_json, "effect_spread", 0.0), 0.0), 0.0, 512.0);
            effect.effect_falloff = (float)std::clamp(finite_or(json_double(effect_json, "effect_falloff", 1.0), 1.0), 0.0, 8.0);
            effect.effect_blur_type = std::clamp(json_int(effect_json, "effect_blur_type", (int)ShadowBlurType::StackFast), 0, (int)ShadowBlurType::DualKawase);
            effect.effect_samples = std::clamp(json_int(effect_json, "effect_samples", 8), 2, 64);
            effect.effect_centered = json_bool(effect_json, "effect_centered", true);
            if (effect_json.contains("blend_mode"))
                effect.blend_mode = (EffectBlendMode)std::clamp(json_int(effect_json, "blend_mode", (int)effect.blend_mode), 0, (int)EffectBlendMode::Color);
            effect.effect_owned_style_loaded = effect_json.contains("effect_fill_type") ||
                                               effect_json.contains("enabled_prop") ||
                                               effect_json.contains("color_a");
            effect.effect_fill_type = std::clamp(json_int(effect_json, "effect_fill_type", effect.effect_fill_type), 0, 2);
            effect.effect_join_style = std::clamp(json_int(effect_json, "effect_join_style", effect.effect_join_style), 0, 2);
            effect.effect_on_front = json_bool(effect_json, "effect_on_front", effect.effect_on_front);
            effect.effect_antialias = json_bool(effect_json, "effect_antialias", effect.effect_antialias);
            effect.effect_stroke_color = json_color(effect_json, "effect_stroke_color", effect.effect_stroke_color);
            effect.effect_stroke_width = (float)std::clamp(finite_or(json_double(effect_json, "effect_stroke_width", effect.effect_stroke_width), effect.effect_stroke_width), 0.0, (double)kMaxCanvasDimension);
            effect.effect_stroke_opacity = (float)std::clamp(finite_or(json_double(effect_json, "effect_stroke_opacity", effect.effect_stroke_opacity), effect.effect_stroke_opacity), 0.0, 1.0);
            effect.effect_padding_left = (float)std::clamp(finite_or(json_double(effect_json, "effect_padding_left", effect.effect_padding_left), effect.effect_padding_left), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
            effect.effect_padding_right = (float)std::clamp(finite_or(json_double(effect_json, "effect_padding_right", effect.effect_padding_right), effect.effect_padding_right), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
            effect.effect_padding_top = (float)std::clamp(finite_or(json_double(effect_json, "effect_padding_top", effect.effect_padding_top), effect.effect_padding_top), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
            effect.effect_padding_bottom = (float)std::clamp(finite_or(json_double(effect_json, "effect_padding_bottom", effect.effect_padding_bottom), effect.effect_padding_bottom), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
            effect.effect_corner_radius_tl = (float)std::clamp(finite_or(json_double(effect_json, "effect_corner_radius_tl", effect.effect_corner_radius_tl), effect.effect_corner_radius_tl), 0.0, (double)kMaxCanvasDimension);
            effect.effect_corner_radius_tr = (float)std::clamp(finite_or(json_double(effect_json, "effect_corner_radius_tr", effect.effect_corner_radius_tr), effect.effect_corner_radius_tr), 0.0, (double)kMaxCanvasDimension);
            effect.effect_corner_radius_br = (float)std::clamp(finite_or(json_double(effect_json, "effect_corner_radius_br", effect.effect_corner_radius_br), effect.effect_corner_radius_br), 0.0, (double)kMaxCanvasDimension);
            effect.effect_corner_radius_bl = (float)std::clamp(finite_or(json_double(effect_json, "effect_corner_radius_bl", effect.effect_corner_radius_bl), effect.effect_corner_radius_bl), 0.0, (double)kMaxCanvasDimension);
            effect.effect_corner_type = std::clamp(json_int(effect_json, "effect_corner_type", effect.effect_corner_type), 0, 3);
            effect.effect_gradient_spread = gradient_spread_from_json(effect_json, "effect_gradient_spread",
                                                                       "effect_gradient_type",
                                                                       effect.effect_gradient_spread);
            effect.effect_gradient_type = normalize_gradient_type(json_int(effect_json, "effect_gradient_type",
                                                                           effect.effect_gradient_type));
            effect.effect_gradient_start_color = json_color(effect_json, "effect_gradient_start_color", effect.effect_gradient_start_color);
            effect.effect_gradient_end_color = json_color(effect_json, "effect_gradient_end_color", effect.effect_gradient_end_color);
            effect.effect_gradient_start_pos = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_start_pos", effect.effect_gradient_start_pos), effect.effect_gradient_start_pos), 0.0, 1.0);
            effect.effect_gradient_end_pos = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_end_pos", effect.effect_gradient_end_pos), effect.effect_gradient_end_pos), 0.0, 1.0);
            effect.effect_gradient_start_opacity = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_start_opacity", effect.effect_gradient_start_opacity), effect.effect_gradient_start_opacity), 0.0, 1.0);
            effect.effect_gradient_end_opacity = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_end_opacity", effect.effect_gradient_end_opacity), effect.effect_gradient_end_opacity), 0.0, 1.0);
            effect.effect_gradient_opacity = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_opacity", effect.effect_gradient_opacity), effect.effect_gradient_opacity), 0.0, 1.0);
            effect.effect_gradient_angle = (float)finite_or(json_double(effect_json, "effect_gradient_angle", effect.effect_gradient_angle), effect.effect_gradient_angle);
            effect.effect_gradient_center_x = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_center_x", effect.effect_gradient_center_x), effect.effect_gradient_center_x), -100.0, 100.0);
            effect.effect_gradient_center_y = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_center_y", effect.effect_gradient_center_y), effect.effect_gradient_center_y), -100.0, 100.0);
            effect.effect_gradient_scale = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_scale", effect.effect_gradient_scale), effect.effect_gradient_scale), 0.01, 100.0);
            effect.effect_gradient_focal_x = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_focal_x", effect.effect_gradient_focal_x), effect.effect_gradient_focal_x), -100.0, 100.0);
            effect.effect_gradient_focal_y = (float)std::clamp(finite_or(json_double(effect_json, "effect_gradient_focal_y", effect.effect_gradient_focal_y), effect.effect_gradient_focal_y), -100.0, 100.0);
            effect.enabled_prop.static_value = effect.enabled ? 1.0 : 0.0;
            effect.opacity_prop.static_value = effect.effect_opacity;
            effect.size_prop.static_value = effect.effect_size;
            effect.distance_prop.static_value = effect.effect_distance;
            effect.angle_prop.static_value = effect.effect_angle;
            effect.spread_prop.static_value = effect.effect_spread;
            effect.falloff_prop.static_value = effect.effect_falloff;
            effect.stroke_width_prop.static_value = effect.effect_stroke_width;
            effect.stroke_opacity_prop.static_value = effect.effect_stroke_opacity;
            effect.padding_left_prop.static_value = effect.effect_padding_left;
            effect.padding_right_prop.static_value = effect.effect_padding_right;
            effect.padding_top_prop.static_value = effect.effect_padding_top;
            effect.padding_bottom_prop.static_value = effect.effect_padding_bottom;
            effect.corner_radius_tl_prop.static_value = effect.effect_corner_radius_tl;
            effect.corner_radius_tr_prop.static_value = effect.effect_corner_radius_tr;
            effect.corner_radius_br_prop.static_value = effect.effect_corner_radius_br;
            effect.corner_radius_bl_prop.static_value = effect.effect_corner_radius_bl;
            set_argb_channels(effect.color_a, effect.color_r, effect.color_g, effect.color_b, effect.effect_color);
            set_argb_channels(effect.stroke_color_a, effect.stroke_color_r, effect.stroke_color_g, effect.stroke_color_b, effect.effect_stroke_color);
            if (effect_json.contains("enabled_prop")) effect.enabled_prop = aprop_from_json(effect_json["enabled_prop"], "effect_enabled");
            if (effect_json.contains("opacity_prop")) effect.opacity_prop = aprop_from_json(effect_json["opacity_prop"], "effect_opacity");
            if (effect_json.contains("size_prop")) effect.size_prop = aprop_from_json(effect_json["size_prop"], "effect_size");
            if (effect_json.contains("distance_prop")) effect.distance_prop = aprop_from_json(effect_json["distance_prop"], "effect_distance");
            if (effect_json.contains("angle_prop")) effect.angle_prop = aprop_from_json(effect_json["angle_prop"], "effect_angle");
            if (effect_json.contains("spread_prop")) effect.spread_prop = aprop_from_json(effect_json["spread_prop"], "effect_spread");
            if (effect_json.contains("falloff_prop")) effect.falloff_prop = aprop_from_json(effect_json["falloff_prop"], "effect_falloff");
            if (effect_json.contains("stroke_width_prop")) effect.stroke_width_prop = aprop_from_json(effect_json["stroke_width_prop"], "effect_stroke_width");
            if (effect_json.contains("stroke_opacity_prop")) effect.stroke_opacity_prop = aprop_from_json(effect_json["stroke_opacity_prop"], "effect_stroke_opacity");
            if (effect_json.contains("padding_left_prop")) effect.padding_left_prop = aprop_from_json(effect_json["padding_left_prop"], "effect_padding_left");
            if (effect_json.contains("padding_right_prop")) effect.padding_right_prop = aprop_from_json(effect_json["padding_right_prop"], "effect_padding_right");
            if (effect_json.contains("padding_top_prop")) effect.padding_top_prop = aprop_from_json(effect_json["padding_top_prop"], "effect_padding_top");
            if (effect_json.contains("padding_bottom_prop")) effect.padding_bottom_prop = aprop_from_json(effect_json["padding_bottom_prop"], "effect_padding_bottom");
            if (effect_json.contains("corner_radius_tl_prop")) effect.corner_radius_tl_prop = aprop_from_json(effect_json["corner_radius_tl_prop"], "effect_corner_radius_tl");
            if (effect_json.contains("corner_radius_tr_prop")) effect.corner_radius_tr_prop = aprop_from_json(effect_json["corner_radius_tr_prop"], "effect_corner_radius_tr");
            if (effect_json.contains("corner_radius_br_prop")) effect.corner_radius_br_prop = aprop_from_json(effect_json["corner_radius_br_prop"], "effect_corner_radius_br");
            if (effect_json.contains("corner_radius_bl_prop")) effect.corner_radius_bl_prop = aprop_from_json(effect_json["corner_radius_bl_prop"], "effect_corner_radius_bl");
            if (effect_json.contains("color_a")) effect.color_a = aprop_from_json(effect_json["color_a"], "effect_color_a");
            if (effect_json.contains("color_r")) effect.color_r = aprop_from_json(effect_json["color_r"], "effect_color_r");
            if (effect_json.contains("color_g")) effect.color_g = aprop_from_json(effect_json["color_g"], "effect_color_g");
            if (effect_json.contains("color_b")) effect.color_b = aprop_from_json(effect_json["color_b"], "effect_color_b");
            if (effect_json.contains("stroke_color_a")) effect.stroke_color_a = aprop_from_json(effect_json["stroke_color_a"], "effect_stroke_color_a");
            if (effect_json.contains("stroke_color_r")) effect.stroke_color_r = aprop_from_json(effect_json["stroke_color_r"], "effect_stroke_color_r");
            if (effect_json.contains("stroke_color_g")) effect.stroke_color_g = aprop_from_json(effect_json["stroke_color_g"], "effect_stroke_color_g");
            if (effect_json.contains("stroke_color_b")) effect.stroke_color_b = aprop_from_json(effect_json["stroke_color_b"], "effect_stroke_color_b");
            if (effect.type == LayerEffectType::ColorOverlay) {
                effect.tint_color = effect.effect_color;
                effect.tint_amount = effect.effect_opacity;
            }
            l->effects.push_back(effect);
        }
    }
    l->in_time  = std::clamp(finite_or(json_double(j, "in_time", 0.0), 0.0), 0.0, kMaxDuration);
    l->out_time = std::clamp(finite_or(json_double(j, "out_time", 5.0), 5.0), l->in_time, kMaxDuration);
    l->transitions.clear();
    if (j.contains("transitions") && j["transitions"].is_array()) {
        bool edge_seen[2] = {false, false};
        size_t accepted_count = 0;
        // Scan a small bounded prefix rather than only the first two entries.
        // Older or hand-edited files can contain a duplicate edge before a
        // valid transition for the other edge; stopping at index 2 silently
        // discarded that valid transition.
        const size_t count = std::min(j["transitions"].size(), (size_t)32);
        for (size_t i = 0; i < count && accepted_count < 2; ++i) {
            const auto &transition_json = j["transitions"][i];
            if (!transition_json.is_object()) continue;
            LayerTransition transition;
            transition.id = bounded_string(transition_json, "id", "", kMaxNameLength);
            transition.preset_id = bounded_string(transition_json, "preset_id", "", kMaxNameLength);
            transition.display_name = bounded_string(transition_json, "display_name", "Transition", kMaxNameLength);
            transition.enabled = json_bool(transition_json, "enabled", true);
            transition.kind = (LayerTransitionKind)std::clamp(json_int(transition_json, "kind", 0),
                (int)LayerTransitionKind::General, (int)LayerTransitionKind::Text);
            transition.type = (LayerTransitionType)std::clamp(json_int(transition_json, "type", 0),
                (int)LayerTransitionType::Dissolve, (int)LayerTransitionType::TextBlurSlide);
            transition.edge = (LayerTransitionEdge)std::clamp(json_int(transition_json, "edge", 0),
                (int)LayerTransitionEdge::In, (int)LayerTransitionEdge::Out);
            transition.unit = (LayerTransitionUnit)std::clamp(json_int(transition_json, "unit", 0),
                (int)LayerTransitionUnit::Character, (int)LayerTransitionUnit::Sentence);
            transition.direction = (LayerTransitionDirection)std::clamp(json_int(transition_json, "direction", 0),
                (int)LayerTransitionDirection::None, (int)LayerTransitionDirection::Down);
            transition.easing = (EasingType)std::clamp(json_int(transition_json, "easing", (int)EasingType::EaseInOut),
                (int)EasingType::Linear, (int)EasingType::Hold);
            const double layer_duration = std::max(1.0 / 240.0, l->out_time - l->in_time);
            transition.duration = std::clamp(finite_or(json_double(transition_json, "duration", 0.5), 0.5),
                                             1.0 / 240.0, layer_duration);
            transition.blur_amount = std::clamp(finite_or(json_double(transition_json, "blur_amount", 18.0), 18.0), 0.0, 256.0);
            transition.scale_from = std::clamp(finite_or(json_double(transition_json, "scale_from", 0.82), 0.82), -10.0, 10.0);
            transition.offset = std::clamp(finite_or(json_double(transition_json, "offset", 80.0), 80.0), 0.0, 10000.0);
            transition.stagger = std::clamp(finite_or(json_double(transition_json, "stagger", 0.35), 0.35), 0.0, 0.95);
            transition.softness = std::clamp(finite_or(json_double(transition_json, "softness", 0.0), 0.0), 0.0, 1.0);
            transition.reverse_order = json_bool(transition_json, "reverse_order", false);
            const bool text_type = layer_transition_type_is_text(transition.type);
            transition.kind = text_type ? LayerTransitionKind::Text : LayerTransitionKind::General;
            const int edge_index = transition.edge == LayerTransitionEdge::Out ? 1 : 0;
            if (edge_seen[edge_index]) continue;
            edge_seen[edge_index] = true;
            l->transitions.push_back(std::move(transition));
            ++accepted_count;
        }
    }

    if (j.contains("position")) vec2_aprop_from_json(j["position"], l->position);
    if (j.contains("scale"))    vec2_aprop_from_json(j["scale"], l->scale);
    l->scale_lock = json_bool(j, "scale_lock", true);
    if (j.contains("rotation")) l->rotation = aprop_from_json(j["rotation"], "rotation");
    if (j.contains("opacity"))  l->opacity  = aprop_from_json(j["opacity"],  "opacity");
    l->scale.static_value.x = std::clamp(l->scale.static_value.x, -100.0, 100.0);
    l->scale.static_value.y = std::clamp(l->scale.static_value.y, -100.0, 100.0);
    l->opacity.static_value = std::clamp(l->opacity.static_value, 0.0, 1.0);

    l->text_content  = bounded_string(j, "text_content", "Title", kMaxTextLength);
    l->clock_format  = bounded_string(j, "clock_format", "H:i:s", kMaxNameLength);
    l->expose_text   = json_bool(j, "expose_text", false);
    l->exposed_hide_if_empty = json_bool(j, "exposed_hide_if_empty", false);
    l->exposed_single_value = json_bool(j, "exposed_single_value", false);
    l->live_cue_hidden_if_empty = false;
    l->ignore_persistence = !l->expose_text && json_bool(j, "ignore_persistence", false);
    l->font_family   = bounded_string(j, "font_family", "Helvetica Neue", kMaxNameLength);
    l->font_style    = bounded_string(j, "font_style", "Regular", kMaxNameLength);
    l->font_size     = std::clamp(json_int(j, "font_size", 72), 1, 512);
    l->font_size_prop.static_value = l->font_size;
    if (j.contains("font_size_prop")) l->font_size_prop = aprop_from_json(j["font_size_prop"], "font_size");
    l->font_size_prop.static_value = std::clamp(l->font_size_prop.static_value, 1.0, 512.0);
    l->font_bold     = json_bool(j, "font_bold", false);
    l->font_italic   = json_bool(j, "font_italic", false);
    l->font_kerning  = json_bool(j, "font_kerning", true);
    l->kerning_mode  = std::clamp(json_int(j, "kerning_mode", 0), 0, 2);
    l->manual_kerning = (float)std::clamp(finite_or(json_double(j, "manual_kerning", 0.0), 0.0), -1000.0, 1000.0);
    l->text_leading  = (float)std::clamp(finite_or(json_double(j, "text_leading", 0.0), 0.0), -1000.0, 1000.0);
    l->char_tracking = (float)std::clamp(finite_or(json_double(j, "char_tracking", 0.0), 0.0), -1000.0, 1000.0);
    l->char_tracking_prop.static_value = l->char_tracking;
    if (j.contains("char_tracking_prop")) l->char_tracking_prop = aprop_from_json(j["char_tracking_prop"], "char_tracking");
    l->char_tracking_prop.static_value = std::clamp(l->char_tracking_prop.static_value, -1000.0, 1000.0);
    l->char_scale_x  = (float)std::clamp(finite_or(json_double(j, "char_scale_x", 1.0), 1.0), 0.01, 100.0);
    l->char_scale_x_prop.static_value = l->char_scale_x;
    if (j.contains("char_scale_x_prop")) l->char_scale_x_prop = aprop_from_json(j["char_scale_x_prop"], "char_scale_x");
    l->char_scale_x_prop.static_value = std::clamp(l->char_scale_x_prop.static_value, 0.01, 100.0);
    l->char_scale_y  = (float)std::clamp(finite_or(json_double(j, "char_scale_y", 1.0), 1.0), 0.01, 100.0);
    l->char_scale_y_prop.static_value = l->char_scale_y;
    if (j.contains("char_scale_y_prop")) l->char_scale_y_prop = aprop_from_json(j["char_scale_y_prop"], "char_scale_y");
    l->char_scale_y_prop.static_value = std::clamp(l->char_scale_y_prop.static_value, 0.01, 100.0);
    l->baseline_shift = (float)std::clamp(finite_or(json_double(j, "baseline_shift", 0.0), 0.0), -1000.0, 1000.0);
    l->baseline_shift_prop.static_value = l->baseline_shift;
    if (j.contains("baseline_shift_prop")) l->baseline_shift_prop = aprop_from_json(j["baseline_shift_prop"], "baseline_shift");
    l->baseline_shift_prop.static_value = std::clamp(l->baseline_shift_prop.static_value, -1000.0, 1000.0);
    l->text_style    = std::clamp(json_int(j, "text_style", 0), 0, 4);
    l->text_underline = json_bool(j, "text_underline", false);
    l->text_strikethrough = json_bool(j, "text_strikethrough", false);
    l->text_ligatures = json_bool(j, "text_ligatures", true);
    l->text_stylistic_alternates = json_bool(j, "text_stylistic_alternates", false);
    l->text_fractions = json_bool(j, "text_fractions", false);
    l->text_opentype_features = json_bool(j, "text_opentype_features", false);
    l->text_language = bounded_string(j, "text_language", "English", kMaxNameLength);
    l->text_overflow_mode = std::clamp(json_int(j, "text_overflow_mode", 0), 0, 2);
    l->text_fit_min_scale = (float)std::clamp(finite_or(json_double(j, "text_fit_min_scale", 0.5), 0.5), 0.05, 1.0);
    l->text_box_width_to_text = json_bool(j, "text_box_width_to_text", false);
    l->text_box_height_to_text = json_bool(j, "text_box_height_to_text", false);
    l->max_text_box_width = (float)std::clamp(finite_or(json_double(j, "max_text_box_width", 1920.0), 1920.0), 1.0, (double)kMaxCanvasDimension);
    l->max_text_box_height = (float)std::clamp(finite_or(json_double(j, "max_text_box_height", 1080.0), 1080.0), 1.0, (double)kMaxCanvasDimension);
    l->ticker_style = std::clamp(json_int(j, "ticker_style", 0), 0, 2);
    l->ticker_speed = std::clamp(finite_or(json_double(j, "ticker_speed", 120.0), 120.0), 0.0, 10000.0);
    l->ticker_line_hold = std::clamp(finite_or(json_double(j, "ticker_line_hold", 2.0), 2.0), 0.0, kMaxDuration);
    l->ticker_direction = std::clamp(json_int(j, "ticker_direction", 1), 0, 1);
    l->text_color    = json_color(j, "text_color", (uint32_t)0xFFFFFFFF);
    l->stroke_fill_type = std::clamp(json_int(j, "stroke_fill_type", 1), 0, 2);
    l->stroke_color  = json_color(j, "stroke_color", (uint32_t)0xFF000000);
    l->stroke_width  = std::clamp(finite_or(json_double(j, "stroke_width", 0.0), 0.0), 0.0, 512.0);
    l->outline_enabled = json_bool(j, "outline_enabled", l->stroke_width > 0.0f);
    l->outline_opacity = std::clamp(finite_or(json_double(j, "outline_opacity", 1.0), 1.0), 0.0, 1.0);
    l->outline_join_style = std::clamp(json_int(j, "outline_join_style", 1), 0, 2);
    l->outline_on_front = json_bool(j, "outline_on_front", false);
    l->outline_alignment = std::clamp(json_int(j, "outline_alignment", 0), 0, 2);
    l->outline_antialias = json_bool(j, "outline_antialias", true);
    l->stroke_gradient_spread = gradient_spread_from_json(j, "stroke_gradient_spread", "stroke_gradient_type", 0);
    l->stroke_gradient_type = normalize_gradient_type(json_int(j, "stroke_gradient_type", 0));
    l->stroke_gradient_start_color = json_color(j, "stroke_gradient_start_color", (uint32_t)0xFFFFFFFF);
    l->stroke_gradient_end_color = json_color(j, "stroke_gradient_end_color", l->stroke_color);
    l->stroke_gradient_start_pos = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_start_pos", 0.0), 0.0), 0.0, 1.0);
    l->stroke_gradient_end_pos = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_end_pos", 1.0), 1.0), 0.0, 1.0);
    l->stroke_gradient_start_opacity = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_start_opacity", 1.0), 1.0), 0.0, 1.0);
    l->stroke_gradient_end_opacity = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_end_opacity", 1.0), 1.0), 0.0, 1.0);
    l->stroke_gradient_opacity = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_opacity", 1.0), 1.0), 0.0, 1.0);
    l->stroke_gradient_angle = (float)finite_or(json_double(j, "stroke_gradient_angle", 0.0), 0.0);
    l->stroke_gradient_center_x = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_center_x", 0.5), 0.5), -100.0, 100.0);
    l->stroke_gradient_center_y = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_center_y", 0.5), 0.5), -100.0, 100.0);
    l->stroke_gradient_scale = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_scale", 1.0), 1.0), 0.01, 100.0);
    l->stroke_gradient_focal_x = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_focal_x", l->stroke_gradient_center_x), l->stroke_gradient_center_x), -100.0, 100.0);
    l->stroke_gradient_focal_y = (float)std::clamp(finite_or(json_double(j, "stroke_gradient_focal_y", l->stroke_gradient_center_y), l->stroke_gradient_center_y), -100.0, 100.0);
    l->stroke_gradient_stops = gradient_stops_from_json(j.value("stroke_gradient_stops", json::array()));
    l->align_h       = std::clamp(json_int(j, "align_h", 1), 0, 6);
    l->align_v       = std::clamp(json_int(j, "align_v", 1), 0, 3);
    l->paragraph_indent_left = (float)std::clamp(finite_or(json_double(j, "paragraph_indent_left", 0.0), 0.0), -10000.0, 10000.0);
    l->paragraph_indent_right = (float)std::clamp(finite_or(json_double(j, "paragraph_indent_right", 0.0), 0.0), -10000.0, 10000.0);
    l->paragraph_indent_first_line = (float)std::clamp(finite_or(json_double(j, "paragraph_indent_first_line", 0.0), 0.0), -10000.0, 10000.0);
    l->paragraph_indent_left_prop.static_value = l->paragraph_indent_left;
    l->paragraph_indent_right_prop.static_value = l->paragraph_indent_right;
    l->paragraph_indent_first_line_prop.static_value = l->paragraph_indent_first_line;
    if (j.contains("paragraph_indent_left_prop")) l->paragraph_indent_left_prop = aprop_from_json(j["paragraph_indent_left_prop"], "paragraph_indent_left");
    if (j.contains("paragraph_indent_right_prop")) l->paragraph_indent_right_prop = aprop_from_json(j["paragraph_indent_right_prop"], "paragraph_indent_right");
    if (j.contains("paragraph_indent_first_line_prop")) l->paragraph_indent_first_line_prop = aprop_from_json(j["paragraph_indent_first_line_prop"], "paragraph_indent_first_line");
    l->paragraph_indent_left_prop.static_value = std::clamp(l->paragraph_indent_left_prop.static_value, -10000.0, 10000.0);
    l->paragraph_indent_right_prop.static_value = std::clamp(l->paragraph_indent_right_prop.static_value, -10000.0, 10000.0);
    l->paragraph_indent_first_line_prop.static_value = std::clamp(l->paragraph_indent_first_line_prop.static_value, -10000.0, 10000.0);
    l->paragraph_space_before = (float)std::clamp(finite_or(json_double(j, "paragraph_space_before", 0.0), 0.0), -10000.0, 10000.0);
    l->paragraph_space_before_prop.static_value = l->paragraph_space_before;
    if (j.contains("paragraph_space_before_prop")) l->paragraph_space_before_prop = aprop_from_json(j["paragraph_space_before_prop"], "paragraph_space_before");
    l->paragraph_space_before_prop.static_value = std::clamp(l->paragraph_space_before_prop.static_value, -10000.0, 10000.0);
    l->paragraph_space_after = (float)std::clamp(finite_or(json_double(j, "paragraph_space_after", 0.0), 0.0), -10000.0, 10000.0);
    l->paragraph_space_after_prop.static_value = l->paragraph_space_after;
    if (j.contains("paragraph_space_after_prop")) l->paragraph_space_after_prop = aprop_from_json(j["paragraph_space_after_prop"], "paragraph_space_after");
    l->paragraph_space_after_prop.static_value = std::clamp(l->paragraph_space_after_prop.static_value, -10000.0, 10000.0);
    l->paragraph_hyphenate = json_bool(j, "paragraph_hyphenate", false);

    l->fill_color    = json_color(j, "fill_color", (uint32_t)0xFF222222);
    l->fill_type     = std::clamp(json_int(j, "fill_type", 0), 0, 1);
    l->gradient_spread = gradient_spread_from_json(j, "gradient_spread", "gradient_type", 0);
    l->gradient_type = normalize_gradient_type(json_int(j, "gradient_type", 0));
    l->gradient_start_color = json_color(j, "gradient_start_color", (uint32_t)0xFF4B6EA8);
    l->gradient_end_color = json_color(j, "gradient_end_color", (uint32_t)0xFF1B1B1B);
    l->gradient_start_pos = (float)std::clamp(finite_or(json_double(j, "gradient_start_pos", 0.0), 0.0), 0.0, 1.0);
    l->gradient_end_pos = (float)std::clamp(finite_or(json_double(j, "gradient_end_pos", 1.0), 1.0), 0.0, 1.0);
    l->gradient_start_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_start_opacity", 1.0), 1.0), 0.0, 1.0);
    l->gradient_end_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_end_opacity", 1.0), 1.0), 0.0, 1.0);
    l->gradient_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_opacity", 1.0), 1.0), 0.0, 1.0);
    l->gradient_angle = (float)finite_or(json_double(j, "gradient_angle", 0.0), 0.0);
    l->gradient_center_x = (float)std::clamp(finite_or(json_double(j, "gradient_center_x", 0.5), 0.5), -100.0, 100.0);
    l->gradient_center_y = (float)std::clamp(finite_or(json_double(j, "gradient_center_y", 0.5), 0.5), -100.0, 100.0);
    l->gradient_scale = (float)std::clamp(finite_or(json_double(j, "gradient_scale", 1.0), 1.0), 0.01, 100.0);
    l->gradient_focal_x = (float)std::clamp(finite_or(json_double(j, "gradient_focal_x", l->gradient_center_x), l->gradient_center_x), -100.0, 100.0);
    l->gradient_focal_y = (float)std::clamp(finite_or(json_double(j, "gradient_focal_y", l->gradient_center_y), l->gradient_center_y), -100.0, 100.0);
    l->gradient_stops = gradient_stops_from_json(j.value("gradient_stops", json::array()));
    l->rich_text = j.contains("rich_text") ? rich_doc_from_json(j["rich_text"], *l) : rich_text_document_from_layer_defaults(*l);
    rich_text_document_ensure_canonical(*l);
    l->background_enabled = json_bool(j, "background_enabled", false);
    l->background_color = json_color(j, "background_color", (uint32_t)0xFF000000);
    l->background_opacity = (float)std::clamp(finite_or(json_double(j, "background_opacity", 0.35), 0.35), 0.0, 1.0);
    const double legacy_padding = finite_or(json_double(j, "background_padding", 0.0), 0.0);
    l->background_padding_x = (float)std::clamp(finite_or(json_double(j, "background_padding_x", legacy_padding), legacy_padding), 0.0, (double)kMaxCanvasDimension);
    l->background_padding_y = (float)std::clamp(finite_or(json_double(j, "background_padding_y", legacy_padding), legacy_padding), 0.0, (double)kMaxCanvasDimension);
    l->background_padding_left = (float)std::clamp(finite_or(json_double(j, "background_padding_left", l->background_padding_x), l->background_padding_x), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
    l->background_padding_right = (float)std::clamp(finite_or(json_double(j, "background_padding_right", l->background_padding_x), l->background_padding_x), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
    l->background_padding_top = (float)std::clamp(finite_or(json_double(j, "background_padding_top", l->background_padding_y), l->background_padding_y), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
    l->background_padding_bottom = (float)std::clamp(finite_or(json_double(j, "background_padding_bottom", l->background_padding_y), l->background_padding_y), -(double)kMaxCanvasDimension, (double)kMaxCanvasDimension);
    l->background_corner_radius = (float)std::clamp(finite_or(json_double(j, "background_corner_radius", 0.0), 0.0), 0.0, (double)kMaxCanvasDimension);
    l->background_corner_radius_tl = (float)std::clamp(finite_or(json_double(j, "background_corner_radius_tl", l->background_corner_radius), l->background_corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->background_corner_radius_tr = (float)std::clamp(finite_or(json_double(j, "background_corner_radius_tr", l->background_corner_radius), l->background_corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->background_corner_radius_br = (float)std::clamp(finite_or(json_double(j, "background_corner_radius_br", l->background_corner_radius), l->background_corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->background_corner_radius_bl = (float)std::clamp(finite_or(json_double(j, "background_corner_radius_bl", l->background_corner_radius), l->background_corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->background_corner_type = (CornerType)std::clamp(json_int(j, "background_corner_type", (int)CornerType::Round), 0, (int)CornerType::Cutout);
    l->background_fill_type = std::clamp(json_int(j, "background_fill_type", 0), 0, 1);
    l->background_stroke_color = json_color(j, "background_stroke_color", (uint32_t)0x00000000);
    l->background_stroke_width = (float)std::clamp(finite_or(json_double(j, "background_stroke_width", 0.0), 0.0), 0.0, (double)kMaxCanvasDimension);
    l->background_stroke_opacity = (float)std::clamp(finite_or(json_double(j, "background_stroke_opacity", 1.0), 1.0), 0.0, 1.0);
    l->background_stroke_fill_type = std::clamp(json_int(j, "background_stroke_fill_type", 0), 0, 1);
    l->background_gradient_spread = gradient_spread_from_json(j, "background_gradient_spread",
                                                             "background_gradient_type",
                                                             l->gradient_spread);
    l->background_gradient_type = normalize_gradient_type(json_int(j, "background_gradient_type",
                                                                  l->gradient_type));
    l->background_gradient_start_color = json_color(j, "background_gradient_start_color", l->gradient_start_color);
    l->background_gradient_end_color = json_color(j, "background_gradient_end_color", l->gradient_end_color);
    l->background_gradient_start_pos = (float)std::clamp(finite_or(json_double(j, "background_gradient_start_pos", l->gradient_start_pos), l->gradient_start_pos), 0.0, 1.0);
    l->background_gradient_end_pos = (float)std::clamp(finite_or(json_double(j, "background_gradient_end_pos", l->gradient_end_pos), l->gradient_end_pos), 0.0, 1.0);
    l->background_gradient_start_opacity = (float)std::clamp(finite_or(json_double(j, "background_gradient_start_opacity", l->gradient_start_opacity), l->gradient_start_opacity), 0.0, 1.0);
    l->background_gradient_end_opacity = (float)std::clamp(finite_or(json_double(j, "background_gradient_end_opacity", l->gradient_end_opacity), l->gradient_end_opacity), 0.0, 1.0);
    l->background_gradient_opacity = (float)std::clamp(finite_or(json_double(j, "background_gradient_opacity", l->gradient_opacity), l->gradient_opacity), 0.0, 1.0);
    l->background_gradient_angle = (float)finite_or(json_double(j, "background_gradient_angle", l->gradient_angle), l->gradient_angle);
    l->background_gradient_center_x = (float)std::clamp(finite_or(json_double(j, "background_gradient_center_x", l->gradient_center_x), l->gradient_center_x), -100.0, 100.0);
    l->background_gradient_center_y = (float)std::clamp(finite_or(json_double(j, "background_gradient_center_y", l->gradient_center_y), l->gradient_center_y), -100.0, 100.0);
    l->background_gradient_scale = (float)std::clamp(finite_or(json_double(j, "background_gradient_scale", l->gradient_scale), l->gradient_scale), 0.01, 100.0);
    l->background_gradient_focal_x = (float)std::clamp(finite_or(json_double(j, "background_gradient_focal_x", l->gradient_focal_x), l->gradient_focal_x), -100.0, 100.0);
    l->background_gradient_focal_y = (float)std::clamp(finite_or(json_double(j, "background_gradient_focal_y", l->gradient_focal_y), l->gradient_focal_y), -100.0, 100.0);
    l->background_gradient_stops = gradient_stops_from_json(j.value("background_gradient_stops", json::array()));
    l->background_enabled_prop.static_value = l->background_enabled ? 1.0 : 0.0;
    l->background_opacity_prop.static_value = l->background_opacity;
    l->background_padding_x_prop.static_value = l->background_padding_x;
    l->background_padding_y_prop.static_value = l->background_padding_y;
    l->background_padding_left_prop.static_value = l->background_padding_left;
    l->background_padding_right_prop.static_value = l->background_padding_right;
    l->background_padding_top_prop.static_value = l->background_padding_top;
    l->background_padding_bottom_prop.static_value = l->background_padding_bottom;
    l->background_corner_radius_prop.static_value = l->background_corner_radius;
    l->background_corner_radius_tl_prop.static_value = l->background_corner_radius_tl;
    l->background_corner_radius_tr_prop.static_value = l->background_corner_radius_tr;
    l->background_corner_radius_br_prop.static_value = l->background_corner_radius_br;
    l->background_corner_radius_bl_prop.static_value = l->background_corner_radius_bl;
    l->background_stroke_width_prop.static_value = l->background_stroke_width;
    l->background_stroke_opacity_prop.static_value = l->background_stroke_opacity;
    set_background_color_channels(*l, l->background_color);
    set_argb_channels(l->background_stroke_color_a, l->background_stroke_color_r, l->background_stroke_color_g, l->background_stroke_color_b, l->background_stroke_color);
    if (j.contains("background_enabled_prop")) l->background_enabled_prop = aprop_from_json(j["background_enabled_prop"], "background_enabled");
    if (j.contains("background_opacity_prop")) l->background_opacity_prop = aprop_from_json(j["background_opacity_prop"], "background_opacity");
    if (j.contains("background_padding_x_prop")) l->background_padding_x_prop = aprop_from_json(j["background_padding_x_prop"], "background_padding_x");
    if (j.contains("background_padding_y_prop")) l->background_padding_y_prop = aprop_from_json(j["background_padding_y_prop"], "background_padding_y");
    if (j.contains("background_padding_left_prop")) l->background_padding_left_prop = aprop_from_json(j["background_padding_left_prop"], "background_padding_left");
    if (j.contains("background_padding_right_prop")) l->background_padding_right_prop = aprop_from_json(j["background_padding_right_prop"], "background_padding_right");
    if (j.contains("background_padding_top_prop")) l->background_padding_top_prop = aprop_from_json(j["background_padding_top_prop"], "background_padding_top");
    if (j.contains("background_padding_bottom_prop")) l->background_padding_bottom_prop = aprop_from_json(j["background_padding_bottom_prop"], "background_padding_bottom");
    if (j.contains("background_corner_radius_prop")) l->background_corner_radius_prop = aprop_from_json(j["background_corner_radius_prop"], "background_corner_radius");
    if (j.contains("background_corner_radius_tl_prop")) l->background_corner_radius_tl_prop = aprop_from_json(j["background_corner_radius_tl_prop"], "background_corner_radius_tl");
    if (j.contains("background_corner_radius_tr_prop")) l->background_corner_radius_tr_prop = aprop_from_json(j["background_corner_radius_tr_prop"], "background_corner_radius_tr");
    if (j.contains("background_corner_radius_br_prop")) l->background_corner_radius_br_prop = aprop_from_json(j["background_corner_radius_br_prop"], "background_corner_radius_br");
    if (j.contains("background_corner_radius_bl_prop")) l->background_corner_radius_bl_prop = aprop_from_json(j["background_corner_radius_bl_prop"], "background_corner_radius_bl");
    if (j.contains("background_stroke_width_prop")) l->background_stroke_width_prop = aprop_from_json(j["background_stroke_width_prop"], "background_stroke_width");
    if (j.contains("background_stroke_opacity_prop")) l->background_stroke_opacity_prop = aprop_from_json(j["background_stroke_opacity_prop"], "background_stroke_opacity");
    if (j.contains("background_color_a")) l->background_color_a = aprop_from_json(j["background_color_a"], "background_color_a");
    if (j.contains("background_color_r")) l->background_color_r = aprop_from_json(j["background_color_r"], "background_color_r");
    if (j.contains("background_color_g")) l->background_color_g = aprop_from_json(j["background_color_g"], "background_color_g");
    if (j.contains("background_color_b")) l->background_color_b = aprop_from_json(j["background_color_b"], "background_color_b");
    if (j.contains("background_stroke_color_a")) l->background_stroke_color_a = aprop_from_json(j["background_stroke_color_a"], "background_stroke_color_a");
    if (j.contains("background_stroke_color_r")) l->background_stroke_color_r = aprop_from_json(j["background_stroke_color_r"], "background_stroke_color_r");
    if (j.contains("background_stroke_color_g")) l->background_stroke_color_g = aprop_from_json(j["background_stroke_color_g"], "background_stroke_color_g");
    if (j.contains("background_stroke_color_b")) l->background_stroke_color_b = aprop_from_json(j["background_stroke_color_b"], "background_stroke_color_b");
    l->rect_width    = std::clamp(finite_or(json_double(j, "rect_width", 1920.0), 1920.0), 0.0, (double)kMaxCanvasDimension);
    l->rect_height   = std::clamp(finite_or(json_double(j, "rect_height", 100.0), 100.0), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius = std::clamp(finite_or(json_double(j, "corner_radius", 0.0), 0.0), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius_tl = (float)std::clamp(finite_or(json_double(j, "corner_radius_tl", l->corner_radius), l->corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius_tr = (float)std::clamp(finite_or(json_double(j, "corner_radius_tr", l->corner_radius), l->corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius_br = (float)std::clamp(finite_or(json_double(j, "corner_radius_br", l->corner_radius), l->corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius_bl = (float)std::clamp(finite_or(json_double(j, "corner_radius_bl", l->corner_radius), l->corner_radius), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius_locked = json_bool(j, "corner_radius_locked",
                                        l->corner_radius_tl == l->corner_radius_tr &&
                                        l->corner_radius_tl == l->corner_radius_br &&
                                        l->corner_radius_tl == l->corner_radius_bl);
    const int legacy_corner_type = std::clamp(json_int(j, "corner_type", 0), 0, (int)CornerType::Cutout);
    auto legacy_corner_roundness = [](int type) {
        switch ((CornerType)type) {
        case CornerType::Straight:
        case CornerType::Cutout:
            return 0.0;
        case CornerType::Concave:
            return -100.0;
        case CornerType::Round:
        default:
            return 100.0;
        }
    };
    l->corner_bevel_roundness =
        (float)std::clamp(finite_or(json_double(j, "corner_bevel_roundness",
                                                legacy_corner_roundness(legacy_corner_type)),
                                    legacy_corner_roundness(legacy_corner_type)),
                          -100.0, 100.0);
    l->shape_type = (ShapeType)std::clamp(json_int(j, "shape_type", 0), 0, (int)ShapeType::Path);
    l->path_points = bezier_path_points_from_json(j.value("path_points", json::array()));
    l->path_closed = json_bool(j, "path_closed", true);
    if (l->shape_type == ShapeType::Path && l->path_points.size() < 2) {
        l->shape_type = ShapeType::Rectangle;
        l->path_points.clear();
        l->path_closed = true;
    }
    l->shape_points = std::clamp(json_int(j, "shape_points", 5), 3, 64);
    l->shape_sides = std::clamp(json_int(j, "shape_sides", 6), 3, 64);
    l->shape_inner_radius = (float)std::clamp(finite_or(json_double(j, "shape_inner_radius", 0.20), 0.20), 0.0, 1.0);
    l->shape_outer_radius = (float)std::clamp(finite_or(json_double(j, "shape_outer_radius", 0.5), 0.5), 0.0, 1.0);
    l->shape_roundness = (float)std::clamp(finite_or(json_double(j, "shape_roundness", 0.0), 0.0), 0.0, (double)kMaxCanvasDimension);
    l->shape_inner_roundness = (float)std::clamp(finite_or(json_double(j, "shape_inner_roundness", l->shape_roundness), l->shape_roundness), 0.0, (double)kMaxCanvasDimension);
    l->scale_stroke_with_shape = json_bool(j, "scale_stroke_with_shape", false);
    l->scale_corners_with_shape = json_bool(j, "scale_corners_with_shape", false);
    l->size.static_value.x = l->rect_width;
    l->size.static_value.y = l->rect_height;
    if (j.contains("size")) vec2_aprop_from_json(j["size"], l->size);
    l->size.static_value.x = std::clamp(l->size.static_value.x, 0.0, (double)kMaxCanvasDimension);
    l->size.static_value.y = std::clamp(l->size.static_value.y, 0.0, (double)kMaxCanvasDimension);
    l->origin_x      = std::clamp(finite_or(json_double(j, "origin_x", 0.5), 0.5), 0.0, 1.0);
    l->origin_y      = std::clamp(finite_or(json_double(j, "origin_y", 0.5), 0.5), 0.0, 1.0);
    l->origin_prop.static_value.x = l->origin_x;
    l->origin_prop.static_value.y = l->origin_y;
    if (j.contains("origin")) vec2_aprop_from_json(j["origin"], l->origin_prop);
    l->origin_prop.static_value.x = std::clamp(l->origin_prop.static_value.x, 0.0, 1.0);
    l->origin_prop.static_value.y = std::clamp(l->origin_prop.static_value.y, 0.0, 1.0);
    l->shadow_enabled = json_bool(j, "shadow_enabled", false);
    l->shadow_color = json_color(j, "shadow_color", (uint32_t)0x99000000);
    l->shadow_opacity = std::clamp(finite_or(json_double(j, "shadow_opacity", 0.6), 0.6), 0.0, 1.0);
    l->shadow_distance = std::clamp(finite_or(json_double(j, "shadow_distance", 8.0), 8.0), 0.0, 4096.0);
    l->shadow_angle = finite_or(json_double(j, "shadow_angle", 135.0), 135.0);
    l->shadow_blur = std::clamp(finite_or(json_double(j, "shadow_blur", 4.0), 4.0), 0.0, 512.0);
    l->shadow_spread = std::clamp(finite_or(json_double(j, "shadow_spread", 0.0), 0.0), 0.0, 512.0);
    l->shadow_blur_type = (ShadowBlurType)std::clamp(json_int(j, "shadow_blur_type", (int)ShadowBlurType::StackFast), 0, (int)ShadowBlurType::DualKawase);
    l->long_shadow_enabled = json_bool(j, "long_shadow_enabled", false);
    l->long_shadow_color = json_color(j, "long_shadow_color", l->shadow_color);
    l->long_shadow_opacity = std::clamp(finite_or(json_double(j, "long_shadow_opacity", 0.45), 0.45), 0.0, 1.0);
    l->long_shadow_length = std::clamp(finite_or(json_double(j, "long_shadow_length", 0.0), 0.0), 0.0, 4096.0);
    l->long_shadow_angle = finite_or(json_double(j, "long_shadow_angle", l->shadow_angle), l->shadow_angle);
    l->long_shadow_falloff = std::clamp(finite_or(json_double(j, "long_shadow_falloff", 1.0), 1.0), 0.0, 4.0);
    l->long_shadow_blur_type = (LongShadowBlurType)std::clamp(json_int(j, "long_shadow_blur_type", (int)LongShadowBlurType::None), 0, (int)LongShadowBlurType::StackFast);
    l->long_shadow_blur = std::clamp(finite_or(json_double(j, "long_shadow_blur", 8.0), 8.0), 0.0, 512.0);
    l->shadow_enabled_prop.static_value = l->shadow_enabled ? 1.0 : 0.0;
    l->shadow_opacity_prop.static_value = l->shadow_opacity;
    l->shadow_distance_prop.static_value = l->shadow_distance;
    l->shadow_angle_prop.static_value = l->shadow_angle;
    l->shadow_blur_prop.static_value = l->shadow_blur;
    l->shadow_spread_prop.static_value = l->shadow_spread;
    l->shadow_color_a.static_value = (l->shadow_color >> 24) & 0xFF;
    l->shadow_color_r.static_value = (l->shadow_color >> 16) & 0xFF;
    l->shadow_color_g.static_value = (l->shadow_color >> 8) & 0xFF;
    l->shadow_color_b.static_value = l->shadow_color & 0xFF;
    if (j.contains("shadow_enabled_prop")) l->shadow_enabled_prop = aprop_from_json(j["shadow_enabled_prop"], "shadow_enabled");
    if (j.contains("shadow_opacity_prop")) l->shadow_opacity_prop = aprop_from_json(j["shadow_opacity_prop"], "shadow_opacity");
    if (j.contains("shadow_distance_prop")) l->shadow_distance_prop = aprop_from_json(j["shadow_distance_prop"], "shadow_distance");
    if (j.contains("shadow_angle_prop")) l->shadow_angle_prop = aprop_from_json(j["shadow_angle_prop"], "shadow_angle");
    if (j.contains("shadow_blur_prop")) l->shadow_blur_prop = aprop_from_json(j["shadow_blur_prop"], "shadow_blur");
    if (j.contains("shadow_spread_prop")) l->shadow_spread_prop = aprop_from_json(j["shadow_spread_prop"], "shadow_spread");
    if (j.contains("shadow_color_a")) l->shadow_color_a = aprop_from_json(j["shadow_color_a"], "shadow_color_a");
    if (j.contains("shadow_color_r")) l->shadow_color_r = aprop_from_json(j["shadow_color_r"], "shadow_color_r");
    if (j.contains("shadow_color_g")) l->shadow_color_g = aprop_from_json(j["shadow_color_g"], "shadow_color_g");
    if (j.contains("shadow_color_b")) l->shadow_color_b = aprop_from_json(j["shadow_color_b"], "shadow_color_b");
    auto seed_effect_from_layer = [l](LayerEffect &effect) {
        switch (effect.type) {
        case LayerEffectType::BackgroundColor:
            effect.effect_color = l->background_color;
            effect.effect_opacity = l->background_opacity;
            effect.effect_fill_type = l->background_fill_type;
            effect.effect_stroke_color = l->background_stroke_color;
            effect.effect_stroke_width = l->background_stroke_width;
            effect.effect_stroke_opacity = l->background_stroke_opacity;
            effect.effect_padding_left = l->background_padding_left;
            effect.effect_padding_right = l->background_padding_right;
            effect.effect_padding_top = l->background_padding_top;
            effect.effect_padding_bottom = l->background_padding_bottom;
            effect.effect_corner_radius_tl = l->background_corner_radius_tl;
            effect.effect_corner_radius_tr = l->background_corner_radius_tr;
            effect.effect_corner_radius_br = l->background_corner_radius_br;
            effect.effect_corner_radius_bl = l->background_corner_radius_bl;
            effect.effect_corner_type = (int)l->background_corner_type;
            effect.effect_gradient_type = l->background_gradient_type;
            effect.effect_gradient_spread = l->background_gradient_spread;
            effect.effect_gradient_start_color = l->background_gradient_start_color;
            effect.effect_gradient_end_color = l->background_gradient_end_color;
            effect.effect_gradient_start_pos = l->background_gradient_start_pos;
            effect.effect_gradient_end_pos = l->background_gradient_end_pos;
            effect.effect_gradient_start_opacity = l->background_gradient_start_opacity;
            effect.effect_gradient_end_opacity = l->background_gradient_end_opacity;
            effect.effect_gradient_opacity = l->background_gradient_opacity;
            effect.effect_gradient_angle = l->background_gradient_angle;
            effect.effect_gradient_center_x = l->background_gradient_center_x;
            effect.effect_gradient_center_y = l->background_gradient_center_y;
            effect.effect_gradient_scale = l->background_gradient_scale;
            effect.effect_gradient_focal_x = l->background_gradient_focal_x;
            effect.effect_gradient_focal_y = l->background_gradient_focal_y;
            effect.enabled_prop = l->background_enabled_prop;
            effect.enabled_prop.name = "effect_enabled";
            effect.opacity_prop = l->background_opacity_prop;
            effect.opacity_prop.name = "effect_opacity";
            effect.stroke_width_prop = l->background_stroke_width_prop;
            effect.stroke_width_prop.name = "effect_stroke_width";
            effect.stroke_opacity_prop = l->background_stroke_opacity_prop;
            effect.stroke_opacity_prop.name = "effect_stroke_opacity";
            effect.padding_left_prop = l->background_padding_left_prop;
            effect.padding_left_prop.name = "effect_padding_left";
            effect.padding_right_prop = l->background_padding_right_prop;
            effect.padding_right_prop.name = "effect_padding_right";
            effect.padding_top_prop = l->background_padding_top_prop;
            effect.padding_top_prop.name = "effect_padding_top";
            effect.padding_bottom_prop = l->background_padding_bottom_prop;
            effect.padding_bottom_prop.name = "effect_padding_bottom";
            effect.corner_radius_tl_prop = l->background_corner_radius_tl_prop;
            effect.corner_radius_tl_prop.name = "effect_corner_radius_tl";
            effect.corner_radius_tr_prop = l->background_corner_radius_tr_prop;
            effect.corner_radius_tr_prop.name = "effect_corner_radius_tr";
            effect.corner_radius_br_prop = l->background_corner_radius_br_prop;
            effect.corner_radius_br_prop.name = "effect_corner_radius_br";
            effect.corner_radius_bl_prop = l->background_corner_radius_bl_prop;
            effect.corner_radius_bl_prop.name = "effect_corner_radius_bl";
            effect.color_a = l->background_color_a;
            effect.color_a.name = "effect_color_a";
            effect.color_r = l->background_color_r;
            effect.color_r.name = "effect_color_r";
            effect.color_g = l->background_color_g;
            effect.color_g.name = "effect_color_g";
            effect.color_b = l->background_color_b;
            effect.color_b.name = "effect_color_b";
            effect.stroke_color_a = l->background_stroke_color_a;
            effect.stroke_color_a.name = "effect_stroke_color_a";
            effect.stroke_color_r = l->background_stroke_color_r;
            effect.stroke_color_r.name = "effect_stroke_color_r";
            effect.stroke_color_g = l->background_stroke_color_g;
            effect.stroke_color_g.name = "effect_stroke_color_g";
            effect.stroke_color_b = l->background_stroke_color_b;
            effect.stroke_color_b.name = "effect_stroke_color_b";
            break;
        case LayerEffectType::Outline:
            effect.effect_fill_type = l->stroke_fill_type;
            effect.effect_color = l->stroke_color;
            effect.effect_size = l->stroke_width;
            effect.effect_opacity = l->outline_opacity;
            effect.effect_join_style = l->outline_join_style;
            effect.effect_on_front = l->outline_on_front;
            effect.effect_antialias = l->outline_antialias;
            effect.effect_gradient_type = l->stroke_gradient_type;
            effect.effect_gradient_spread = l->stroke_gradient_spread;
            effect.effect_gradient_start_color = l->stroke_gradient_start_color;
            effect.effect_gradient_end_color = l->stroke_gradient_end_color;
            effect.effect_gradient_start_pos = l->stroke_gradient_start_pos;
            effect.effect_gradient_end_pos = l->stroke_gradient_end_pos;
            effect.effect_gradient_start_opacity = l->stroke_gradient_start_opacity;
            effect.effect_gradient_end_opacity = l->stroke_gradient_end_opacity;
            effect.effect_gradient_opacity = l->stroke_gradient_opacity;
            effect.effect_gradient_angle = l->stroke_gradient_angle;
            effect.effect_gradient_center_x = l->stroke_gradient_center_x;
            effect.effect_gradient_center_y = l->stroke_gradient_center_y;
            effect.effect_gradient_scale = l->stroke_gradient_scale;
            effect.effect_gradient_focal_x = l->stroke_gradient_focal_x;
            effect.effect_gradient_focal_y = l->stroke_gradient_focal_y;
            set_argb_channels(effect.color_a, effect.color_r, effect.color_g, effect.color_b, effect.effect_color);
            effect.opacity_prop.static_value = effect.effect_opacity;
            effect.size_prop.static_value = effect.effect_size;
            break;
        case LayerEffectType::DropShadow:
            effect.effect_color = l->shadow_color;
            effect.effect_opacity = l->shadow_opacity;
            effect.effect_distance = l->shadow_distance;
            effect.effect_angle = l->shadow_angle;
            effect.effect_size = l->shadow_blur;
            effect.effect_spread = l->shadow_spread;
            effect.effect_blur_type = (int)l->shadow_blur_type;
            effect.enabled_prop = l->shadow_enabled_prop;
            effect.enabled_prop.name = "effect_enabled";
            effect.opacity_prop = l->shadow_opacity_prop;
            effect.opacity_prop.name = "effect_opacity";
            effect.distance_prop = l->shadow_distance_prop;
            effect.distance_prop.name = "effect_distance";
            effect.angle_prop = l->shadow_angle_prop;
            effect.angle_prop.name = "effect_angle";
            effect.size_prop = l->shadow_blur_prop;
            effect.size_prop.name = "effect_size";
            effect.spread_prop = l->shadow_spread_prop;
            effect.spread_prop.name = "effect_spread";
            effect.color_a = l->shadow_color_a;
            effect.color_a.name = "effect_color_a";
            effect.color_r = l->shadow_color_r;
            effect.color_r.name = "effect_color_r";
            effect.color_g = l->shadow_color_g;
            effect.color_g.name = "effect_color_g";
            effect.color_b = l->shadow_color_b;
            effect.color_b.name = "effect_color_b";
            break;
        case LayerEffectType::LongShadow:
            effect.effect_color = l->long_shadow_color;
            effect.effect_opacity = l->long_shadow_opacity;
            effect.effect_distance = l->long_shadow_length;
            effect.effect_angle = l->long_shadow_angle;
            effect.effect_falloff = l->long_shadow_falloff;
            effect.effect_size = l->long_shadow_blur;
            effect.effect_blur_type = (int)l->long_shadow_blur_type;
            break;
        default:
            break;
        }
    };
    auto make_legacy_effect = [seed_effect_from_layer](LayerEffectType type) {
        LayerEffect effect;
        effect.type = type;
        effect.enabled = true;
        if (type == LayerEffectType::DropShadow || type == LayerEffectType::LongShadow || type == LayerEffectType::InnerShadow) {
            effect.blend_mode = EffectBlendMode::Multiply;
            seed_effect_from_layer(effect);
        } else if (type == LayerEffectType::ColorOverlay)
            effect.blend_mode = EffectBlendMode::Color;
        else if (type == LayerEffectType::Glow || type == LayerEffectType::InnerGlow)
            effect.blend_mode = EffectBlendMode::Additive;
        else
            seed_effect_from_layer(effect);
        return effect;
    };
    if (!j.contains("effects")) {
        if (l->background_enabled) l->effects.push_back(make_legacy_effect(LayerEffectType::BackgroundColor));
        if (l->outline_enabled) l->effects.push_back(make_legacy_effect(LayerEffectType::Outline));
        if (l->shadow_enabled) l->effects.push_back(make_legacy_effect(LayerEffectType::DropShadow));
        if (l->long_shadow_enabled) l->effects.push_back(make_legacy_effect(LayerEffectType::LongShadow));
    } else if (l->long_shadow_enabled) {
        bool has_long_shadow_effect = false;
        for (const auto &effect : l->effects) {
            if (effect.type == LayerEffectType::LongShadow) {
                has_long_shadow_effect = true;
                break;
            }
        }
        if (!has_long_shadow_effect)
            l->effects.push_back(make_legacy_effect(LayerEffectType::LongShadow));
    }
    for (auto &effect : l->effects) {
        if ((!effect.effect_owned_style_loaded &&
             (effect.type == LayerEffectType::BackgroundColor ||
              effect.type == LayerEffectType::Outline)) ||
            ((effect.type == LayerEffectType::DropShadow || effect.type == LayerEffectType::LongShadow) &&
             effect.effect_color == 0xFFFFFFFF))
            seed_effect_from_layer(effect);
    }
    set_color_channels(*l, true, l->text_color);
    set_color_channels(*l, false, l->fill_color);
    if (j.contains("text_color_a")) l->text_color_a = aprop_from_json(j["text_color_a"], "text_color_a");
    if (j.contains("text_color_r")) l->text_color_r = aprop_from_json(j["text_color_r"], "text_color_r");
    if (j.contains("text_color_g")) l->text_color_g = aprop_from_json(j["text_color_g"], "text_color_g");
    if (j.contains("text_color_b")) l->text_color_b = aprop_from_json(j["text_color_b"], "text_color_b");
    if (j.contains("fill_color_a")) l->fill_color_a = aprop_from_json(j["fill_color_a"], "fill_color_a");
    if (j.contains("fill_color_r")) l->fill_color_r = aprop_from_json(j["fill_color_r"], "fill_color_r");
    if (j.contains("fill_color_g")) l->fill_color_g = aprop_from_json(j["fill_color_g"], "fill_color_g");
    if (j.contains("fill_color_b")) l->fill_color_b = aprop_from_json(j["fill_color_b"], "fill_color_b");
    rich_text_document_sync_layer_mirrors(*l);
    l->image_path    = bounded_string(j, "image_path", "", 4096);
    if (object_member(j, "embedded_image") && !restore_embedded_image_asset(j, l->image_path) && require_embedded_assets) {
        if (error) *error = "Could not restore an embedded image asset from the template file.";
    }
    l->lock_aspect_ratio = json_bool(j, "lock_aspect_ratio", l->type == LayerType::Image);
    l->image_box_lock_aspect_ratio = json_bool(j, "image_box_lock_aspect_ratio", false);
    l->scale_filter = (ImageScaleFilter)std::clamp(json_int(j, "scale_filter", (int)ImageScaleFilter::Bilinear),
                                                   0, (int)ImageScaleFilter::Area);
    const int stored_image_box_mode = std::clamp(
        json_int(j, "image_box_mode", (int)ImageBoxMode::FitImageToBox),
        0, (int)ImageBoxMode::FitToShortSide);
    const bool legacy_horizontal_crop = stored_image_box_mode == (int)ImageBoxMode::LegacyFitHorizontalCrop;
    const bool legacy_vertical_crop = stored_image_box_mode == (int)ImageBoxMode::LegacyFitVerticalCrop;
    l->image_box_mode = legacy_horizontal_crop ? ImageBoxMode::FillHorizontal
                        : legacy_vertical_crop ? ImageBoxMode::FillVertical
                                               : (ImageBoxMode)stored_image_box_mode;
    l->image_size_auto_fit = json_bool(j, "image_size_auto_fit", true);
    l->image_crop_when_outside_box = json_bool(
        j, "image_crop_when_outside_box", legacy_horizontal_crop || legacy_vertical_crop);
    if (l->image_box_mode == ImageBoxMode::StretchToFill) {
        l->image_size_auto_fit = true;
        l->lock_aspect_ratio = false;
    }
    l->image_anchor_x = (float)std::clamp(finite_or(json_double(j, "image_anchor_x", 0.5), 0.5), 0.0, 1.0);
    l->image_anchor_y = (float)std::clamp(finite_or(json_double(j, "image_anchor_y", 0.5), 0.5), 0.0, 1.0);
    l->image_width = (float)std::clamp(finite_or(json_double(j, "image_width", 1920.0), 1920.0), 0.0, (double)kMaxCanvasDimension);
    l->image_height = (float)std::clamp(finite_or(json_double(j, "image_height", 1080.0), 1080.0), 0.0, (double)kMaxCanvasDimension);
    l->image_size.static_value.x = l->image_width;
    l->image_size.static_value.y = l->image_height;
    if (j.contains("image_size")) vec2_aprop_from_json(j["image_size"], l->image_size);
    l->image_size.static_value.x = std::clamp(l->image_size.static_value.x, 0.0, (double)kMaxCanvasDimension);
    l->image_size.static_value.y = std::clamp(l->image_size.static_value.y, 0.0, (double)kMaxCanvasDimension);
    return l;
}

static json title_to_json(const Title &t, bool include_embedded_assets = true,
                          bool require_embedded_assets = false, std::string *error = nullptr)
{
    json jt;
    jt["id"]       = t.id;
    jt["name"]     = t.name;
    if (!t.description.empty()) jt["description"] = t.description;
    if (!t.creator.empty()) jt["creator"] = t.creator;
    if (!t.creation_date.empty()) jt["creation_date"] = t.creation_date;
    jt["duration"] = t.duration;
    jt["loop_start"] = t.loop_start;
    jt["loop_end"] = t.loop_end;
    jt["playback_mode"] = t.playback_mode;
    jt["loop_type"] = t.loop_type;
    jt["cue_end_behavior"] = t.cue_end_behavior;
    jt["pause_time"] = t.pause_time;
    jt["bg_color"] = t.bg_color;
    jt["width"]    = t.width;
    jt["height"]   = t.height;
    if (t.editor_default_style_enabled) {
        json defaults = layer_to_json(t.editor_default_layer_style, false, false, nullptr, nullptr);
        defaults.erase("effects");
        defaults.erase("effect_stack_respects_masks");
        // Keep an explicit empty array so loading this defaults object never
        // reconstructs legacy shadow/background/outline effects.
        defaults["effects"] = json::array();
        jt["editor_default_layer_style"] = defaults;
        jt["editor_default_foreground_color"] = t.editor_default_foreground_color;
        jt["editor_default_background_color"] = t.editor_default_background_color;
    }
    if (!t.editor_recent_color_hexes.empty()) {
        json recent = json::array();
        for (const auto &hex : t.editor_recent_color_hexes) {
            if (!hex.empty())
                recent.push_back(hex);
        }
        jt["editor_recent_color_hexes"] = recent;
    }
    json layers = json::array();
    for (auto &l : t.layers) {
        bool asset_embed_failed = false;
        layers.push_back(layer_to_json(*l, include_embedded_assets, require_embedded_assets, error, &asset_embed_failed));
        if (require_embedded_assets && asset_embed_failed) {
            if (error && error->empty())
                *error = "Could not embed an image asset in the template file.";
            return {};
        }
    }
    jt["layers"] = layers;
    json live_rows = json::array();
    for (const auto &row : t.live_text_rows)
        live_rows.push_back(row);
    jt["live_text_rows"] = live_rows;
    jt["live_text_row_ids"] = t.live_text_row_ids;
    jt["live_text_column_order"] = t.live_text_column_order;
    jt["live_text_header_state"] = t.live_text_header_state;
    jt["external_data_enabled"] = t.external_data_enabled;
    jt["playlist_loop"] = t.playlist_loop;
    jt["playlist_reverse"] = t.playlist_reverse;
    jt["playlist_restart_on_source_active"] = t.playlist_restart_on_source_active;
    jt["playlist_stop_on_source_inactive"] = t.playlist_stop_on_source_inactive;
    jt["playlist_hold_seconds"] = t.playlist_hold_seconds;
    if (!t.preview_screenshot_png_base64.empty())
        jt["preview_screenshot_png_base64"] = t.preview_screenshot_png_base64;
    return jt;
}

static std::shared_ptr<Title> title_from_json(const json &jt, bool regenerate_ids,
                                               bool require_embedded_assets = false, std::string *error = nullptr)
{
    auto t = std::make_shared<Title>();
    if (!jt.is_object())
        return t;

    t->id       = bounded_string(jt, "id", TitleDataStore::make_uuid(), kMaxNameLength);
    t->name     = bounded_string(jt, "name", "Untitled", kMaxNameLength);
    t->description = bounded_string(jt, "description", "", kMaxTextLength);
    t->creator = bounded_string(jt, "creator", "", kMaxNameLength);
    t->creation_date = bounded_string(jt, "creation_date", "", kMaxNameLength);
    t->duration = std::clamp(finite_or(json_double(jt, "duration", 5.0), 5.0), 0.1, kMaxDuration);
    t->loop_start = std::clamp(finite_or(json_double(jt, "loop_start", std::min(1.0, t->duration)), 0.0), 0.0, t->duration);
    t->loop_end = std::clamp(finite_or(json_double(jt, "loop_end", std::max(t->loop_start, t->duration - 1.0)), t->duration), t->loop_start, t->duration);
    t->playback_mode = std::clamp(json_int(jt, "playback_mode", 0), 0, 2);
    t->loop_type = std::clamp(json_int(jt, "loop_type", 0), 0, 1);
    t->cue_end_behavior = std::clamp(json_int(jt, "cue_end_behavior", 0), 0, 2);
    t->pause_time = std::clamp(finite_or(json_double(jt, "pause_time", 0.0), 0.0), 0.0, t->duration);
    t->bg_color = json_color(jt, "bg_color", (uint32_t)0x00000000);
    t->width    = std::clamp(json_int(jt, "width", 1920), 1, kMaxCanvasDimension);
    t->height   = std::clamp(json_int(jt, "height", 1080), 1, kMaxCanvasDimension);
    if (jt.contains("editor_default_layer_style") && jt["editor_default_layer_style"].is_object()) {
        auto defaults = layer_from_json(jt["editor_default_layer_style"], false, nullptr);
        if (defaults) {
            defaults->effects.clear();
            defaults->effect_stack_respects_masks = false;
            t->editor_default_layer_style = *defaults;
            t->editor_default_style_enabled = true;
        }
        t->editor_default_foreground_color = json_color(jt, "editor_default_foreground_color", t->editor_default_foreground_color);
        t->editor_default_background_color = json_color(jt, "editor_default_background_color", t->editor_default_background_color);
    }
    if (jt.contains("editor_recent_color_hexes") && jt["editor_recent_color_hexes"].is_array()) {
        t->editor_recent_color_hexes.clear();
        const size_t n = std::min<size_t>(jt["editor_recent_color_hexes"].size(), 32);
        for (size_t i = 0; i < n; ++i) {
            if (jt["editor_recent_color_hexes"][i].is_string()) {
                auto value = jt["editor_recent_color_hexes"][i].get<std::string>();
                if (value.size() > 16) value.resize(16);
                if (!value.empty())
                    t->editor_recent_color_hexes.push_back(value);
            }
        }
    }
    if (jt.contains("layers") && jt["layers"].is_array()) {
        const size_t count = std::min(jt["layers"].size(), kMaxLayersPerTitle);
        t->layers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            t->layers.push_back(layer_from_json(jt["layers"][i], require_embedded_assets, error));
            if (require_embedded_assets && error && !error->empty())
                return t;
        }
    }
    if (jt.contains("live_text_rows") && jt["live_text_rows"].is_array()) {
        const size_t row_count = std::min(jt["live_text_rows"].size(), kMaxLiveTextRows);
        for (size_t r = 0; r < row_count; ++r) {
            const auto &jr = jt["live_text_rows"][r];
            if (!jr.is_array())
                continue;
            std::vector<std::string> row;
            const size_t col_count = std::min(jr.size(), kMaxLiveTextColumns);
            row.reserve(col_count);
            for (size_t c = 0; c < col_count; ++c) {
                if (!jr[c].is_string())
                    continue;
                std::string cell = jr[c].get<std::string>();
                if (cell.size() > kMaxTextLength)
                    cell.resize(kMaxTextLength);
                row.push_back(std::move(cell));
            }
            t->live_text_rows.push_back(std::move(row));
        }
    }
    if (jt.contains("live_text_row_ids") && jt["live_text_row_ids"].is_array()) {
        const size_t count = std::min(jt["live_text_row_ids"].size(), kMaxLiveTextRows);
        t->live_text_row_ids.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!jt["live_text_row_ids"][i].is_string()) {
                t->live_text_row_ids.push_back({});
                continue;
            }
            std::string row_id = jt["live_text_row_ids"][i].get<std::string>();
            if (row_id.size() > kMaxNameLength)
                row_id.resize(kMaxNameLength);
            t->live_text_row_ids.push_back(std::move(row_id));
        }
    }
    ensure_live_text_row_ids(*t);
    if (jt.contains("live_text_column_order") && jt["live_text_column_order"].is_array()) {
        const size_t count = std::min(jt["live_text_column_order"].size(), kMaxLiveTextColumns);
        t->live_text_column_order.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!jt["live_text_column_order"][i].is_string()) continue;
            std::string layer_id = jt["live_text_column_order"][i].get<std::string>();
            if (layer_id.size() > kMaxNameLength)
                layer_id.resize(kMaxNameLength);
            t->live_text_column_order.push_back(std::move(layer_id));
        }
    }
    t->live_text_header_state = bounded_string(jt, "live_text_header_state", "", kMaxTextLength);
    t->external_data_enabled = json_bool(jt, "external_data_enabled", false);
    t->playlist_loop = json_bool(jt, "playlist_loop", false);
    t->playlist_reverse = json_bool(jt, "playlist_reverse", false);
    t->playlist_restart_on_source_active = json_bool(jt, "playlist_restart_on_source_active", false);
    t->playlist_stop_on_source_inactive = json_bool(jt, "playlist_stop_on_source_inactive", false);
    t->playlist_hold_seconds = std::clamp(finite_or(json_double(jt, "playlist_hold_seconds", 5.0), 5.0), 0.0, 3600.0);
    t->preview_screenshot_png_base64 = bounded_string(jt, "preview_screenshot_png_base64", "",
                                                       kMaxScreenshotBase64Length);

    if (regenerate_ids) {
        std::unordered_map<std::string, std::string> layer_id_map;
        t->id = TitleDataStore::make_uuid();
        for (auto &layer : t->layers) {
            std::string old_id = layer->id;
            layer->id = TitleDataStore::make_uuid();
            if (!old_id.empty())
                layer_id_map[old_id] = layer->id;
        }
        for (auto &layer : t->layers) {
            auto it = layer_id_map.find(layer->parent_id);
            if (it != layer_id_map.end())
                layer->parent_id = it->second;
            else if (!layer->parent_id.empty())
                layer->parent_id.clear();
            auto mask_it = layer_id_map.find(layer->mask_source_id);
            if (mask_it != layer_id_map.end())
                layer->mask_source_id = mask_it->second;
            else if (!layer->mask_source_id.empty()) {
                layer->mask_source_id.clear();
                layer->mask_mode = MaskMode::None;
            }
        }
        for (auto &layer_id : t->live_text_column_order) {
            auto it = layer_id_map.find(layer_id);
            if (it != layer_id_map.end())
                layer_id = it->second;
        }
    }

    return t;
}

TitleDataStore::TitleDataStore() = default;

TitleDataStore::~TitleDataStore()
{
    {
        std::lock_guard<std::mutex> lock(save_mutex_);
        save_stop_ = true;
    }
    save_cv_.notify_all();
    if (save_thread_.joinable())
        save_thread_.join();
}

bool TitleDataStore::write_snapshot_atomic(
    const std::vector<std::shared_ptr<Title>> &snapshot,
    const std::string &path)
{
    json root = json::array();
    try {
        for (const auto &title : snapshot) {
            if (title)
                root.push_back(title_to_json(*title));
        }
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to serialize titles file: %s", e.what());
        return false;
    }

    std::string payload;
    try {
        payload = root.dump(2, ' ', false, json::error_handler_t::replace);
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to encode titles file: %s", e.what());
        return false;
    }

    /* QSaveFile writes to a temporary file in the destination directory and
     * atomically replaces the target only when commit() succeeds. Keeping
     * direct-write fallback disabled is intentional: a temporary filesystem
     * or antivirus lock must never cause the last valid titles file to be
     * truncated or deleted. */
    const QString destination = QString::fromUtf8(path.data(), static_cast<int>(path.size()));
    QSaveFile file(destination);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        blog(LOG_WARNING,
             "[OBS Graphics Studio Pro] Failed to open titles file for atomic saving: %s",
             file.errorString().toUtf8().constData());
        return false;
    }

    const qint64 expected = static_cast<qint64>(payload.size());
    const qint64 written = file.write(payload.data(), expected);
    if (written != expected) {
        blog(LOG_WARNING,
             "[OBS Graphics Studio Pro] Failed while writing titles file: %s",
             file.errorString().toUtf8().constData());
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        blog(LOG_WARNING,
             "[OBS Graphics Studio Pro] Failed to atomically replace titles file: %s",
             file.errorString().toUtf8().constData());
        return false;
    }

    return true;
}

void TitleDataStore::save() const
{
    std::vector<std::shared_ptr<Title>> snapshot;
    std::string path;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        snapshot = titles_;
        path = loaded_path_.empty() ? data_path() : loaded_path_;
    }

    /* Serialize synchronous and asynchronous writes. Otherwise a manual save
     * can race an outstanding background save through the same .tmp path. */
    std::lock_guard<std::mutex> write_lock(save_io_mutex_);
    write_snapshot_atomic(snapshot, path);
}

void TitleDataStore::save_worker_loop() const
{
    for (;;) {
        std::unique_ptr<PendingSave> request;
        {
            std::unique_lock<std::mutex> lock(save_mutex_);
            save_cv_.wait(lock, [this] { return save_stop_ || pending_save_; });
            if (save_stop_ && !pending_save_)
                return;
            request = std::move(pending_save_);
        }

        if (!request)
            continue;

        /* Serialization and I/O are done by one long-lived worker. A newer
         * request replaces the pending one instead of spawning another thread. */
        {
            std::lock_guard<std::mutex> write_lock(save_io_mutex_);
            write_snapshot_atomic(request->snapshot, request->path);
        }
    }
}

void TitleDataStore::save_async() const
{
    auto request = std::make_unique<PendingSave>();
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        request->snapshot.reserve(titles_.size());
        for (const auto &source : titles_) {
            if (!source) {
                request->snapshot.push_back(nullptr);
                continue;
            }
            auto copy = std::make_shared<Title>(*source);
            copy->layers.clear();
            copy->layers.reserve(source->layers.size());
            for (const auto &layer : source->layers)
                copy->layers.push_back(layer ? std::make_shared<Layer>(*layer) : nullptr);
            request->snapshot.push_back(std::move(copy));
        }
        request->path = loaded_path_.empty() ? data_path() : loaded_path_;
    }

    {
        std::lock_guard<std::mutex> lock(save_mutex_);
        if (save_stop_)
            return;
        request->generation = ++save_generation_;
        pending_save_ = std::move(request);
        if (!save_worker_started_) {
            save_worker_started_ = true;
            save_thread_ = std::thread(&TitleDataStore::save_worker_loop, this);
        }
    }
    save_cv_.notify_one();
}


bool TitleDataStore::export_title(const std::string &id, const std::string &path, std::string *error) const
{
    TitleTemplateExportMetadata metadata;
    return export_title(id, path, metadata, error);
}

bool TitleDataStore::export_title(const std::string &id, const std::string &path,
                                  const TitleTemplateExportMetadata &metadata,
                                  std::string *error) const
{
    if (error) error->clear();
    auto t = get_title(id);
    if (!t) {
        if (error) *error = "No title template is selected.";
        return false;
    }

    TitleTemplateExportMetadata export_metadata = metadata;
    if (export_metadata.title.empty()) export_metadata.title = t->name;
    if (export_metadata.description.empty()) export_metadata.description = t->description;
    if (export_metadata.creator.empty()) export_metadata.creator = t->creator;
    if (export_metadata.creation_date.empty()) {
        export_metadata.creation_date = t->creation_date.empty() ? current_iso_utc_string() : t->creation_date;
    }
    if (export_metadata.screenshot_png_base64.empty())
        export_metadata.screenshot_png_base64 = t->preview_screenshot_png_base64;

    json root;
    root["format"] = "obs-graphics-studio-pro-title-template";
    root["version"] = 3;
    root["template_title"] = export_metadata.title;
    root["description"] = export_metadata.description;
    root["creator"] = export_metadata.creator;
    root["creation_date"] = export_metadata.creation_date;
    root["screenshot"] = {
        {"mime_type", "image/png"},
        {"data_base64", export_metadata.screenshot_png_base64},
    };
    root["metadata"] = {
        {"title", export_metadata.title},
        {"description", export_metadata.description},
        {"creator", export_metadata.creator},
        {"creation_date", export_metadata.creation_date},
        {"screenshot", root["screenshot"]},
    };
    Title exported_copy = *t;
    exported_copy.name = export_metadata.title;
    exported_copy.description = export_metadata.description;
    exported_copy.creator = export_metadata.creator;
    exported_copy.creation_date = export_metadata.creation_date;
    exported_copy.preview_screenshot_png_base64 = export_metadata.screenshot_png_base64;
    json exported_title = title_to_json(exported_copy, true, true, error);
    if ((error && !error->empty()) || exported_title.empty()) {
        if (error && error->empty())
            *error = "Could not embed all title assets in the export file.";
        return false;
    }
    root["title"] = std::move(exported_title);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        if (error) *error = "Could not open the export file for writing.";
        return false;
    }
    try {
        f << root.dump(2, ' ', false, json::error_handler_t::replace);
    } catch (const std::exception &e) {
        if (error) *error = std::string("Failed to serialize the export file: ") + e.what();
        return false;
    }
    if (!f.good()) {
        if (error) *error = "Failed while writing the export file.";
        return false;
    }
    return true;
}

std::shared_ptr<Title> TitleDataStore::import_title(const std::string &path, std::string *error)
{
    if (error) error->clear();
    try {
        json root;
        if (!read_json_file(path, root, error))
            return nullptr;
        json jt;
        if (root.is_object() && root.contains("title"))
            jt = root["title"];
        else if (root.is_array() && !root.empty())
            jt = root.front();
        else if (root.is_object())
            jt = root;
        else
            throw std::runtime_error("Unsupported template file format.");

        auto imported = title_from_json(jt, true, true, error);
        if (imported && root.is_object()) {
            json meta = root.value("metadata", json::object());
            if (imported->name.empty())
                imported->name = bounded_string(meta, "title", bounded_string(root, "template_title", "Imported Title", kMaxNameLength), kMaxNameLength);
            if (imported->description.empty())
                imported->description = bounded_string(meta, "description", bounded_string(root, "description", "", kMaxTextLength), kMaxTextLength);
            if (imported->creator.empty())
                imported->creator = bounded_string(meta, "creator", bounded_string(root, "creator", "", kMaxNameLength), kMaxNameLength);
            if (imported->creation_date.empty())
                imported->creation_date = bounded_string(meta, "creation_date", bounded_string(root, "creation_date", "", kMaxNameLength), kMaxNameLength);
        }
        if (imported && imported->preview_screenshot_png_base64.empty() && root.is_object()) {
            json screenshot = root.value("screenshot", json::object());
            if (screenshot.empty() && root.contains("metadata") && root["metadata"].is_object())
                screenshot = root["metadata"].value("screenshot", json::object());
            if (screenshot.is_object()) {
                const std::string png_base64 = bounded_string(screenshot, "data_base64", "",
                                                              kMaxScreenshotBase64Length);
                imported->preview_screenshot_png_base64 = png_base64;
            }
        }
        if (error && !error->empty())
            throw std::runtime_error(*error);
        if (!imported || imported->layers.empty())
            throw std::runtime_error("Template data was empty.");
        std::unordered_set<std::string> seen_ids;
        ensure_unique_title_id(imported, seen_ids);

        std::string base_name = imported->name.empty() ? "Imported Title" : imported->name;
        std::string unique_name = base_name;
        int suffix = 2;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto name_exists = [this](const std::string &candidate) {
                return std::any_of(titles_.begin(), titles_.end(), [&](const auto &existing) {
                    return existing && existing->name == candidate;
                });
            };
            while (name_exists(unique_name))
                unique_name = base_name + " (imported " + std::to_string(suffix++) + ")";
            imported->name = unique_name;
            titles_.push_back(imported);
        }

        notify_change();
        save();
        return imported;
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

void TitleDataStore::load()
{
    const std::string path = data_path();
    json root;
    std::string error;
    if (!read_json_file(path, root, &error)) {
        bool preserved_existing_store = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            /* A reload of the same collection can briefly race filesystem or
             * security-software activity. Never turn that transient failure
             * into an empty in-memory store. A genuinely different collection
             * still starts empty when it has no saved file. */
            const bool same_collection = !loaded_path_.empty() && loaded_path_ == path;
            preserved_existing_store = same_collection && !titles_.empty();
            if (!preserved_existing_store) {
                loaded_path_ = path;
                titles_.clear();
            }
        }

        if (preserved_existing_store) {
            blog(LOG_WARNING,
                 "[OBS Graphics Studio Pro] Failed to reload titles for the current scene collection; "
                 "keeping the already loaded titles in memory: %s",
                 error.c_str());
            return;
        }

        notify_change();
        if (error == "Could not open the file.")
            blog(LOG_INFO, "[OBS Graphics Studio Pro] No saved titles found for this scene collection, starting fresh.");
        else
            blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to read scene collection titles file: %s", error.c_str());
        return;
    }

    try {
        if (!root.is_array())
            throw std::runtime_error("Saved titles root must be an array.");

        std::vector<std::shared_ptr<Title>> loaded;
        std::unordered_set<std::string> seen_ids;
        const size_t count = std::min(root.size(), kMaxTitles);
        loaded.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto title = title_from_json(root[i], false);
            ensure_unique_title_id(title, seen_ids);
            loaded.push_back(title);
        }
        size_t loaded_count = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            loaded_path_ = path;
            titles_ = std::move(loaded);
            loaded_count = titles_.size();
        }
        notify_change();
        blog(LOG_INFO, "[OBS Graphics Studio Pro] Loaded %zu title(s) for this scene collection.", loaded_count);
    } catch (const std::exception &e) {
        bool preserved_existing_store = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const bool same_collection = !loaded_path_.empty() && loaded_path_ == path;
            preserved_existing_store = same_collection && !titles_.empty();
            if (!preserved_existing_store) {
                loaded_path_ = path;
                titles_.clear();
            }
        }

        if (preserved_existing_store) {
            blog(LOG_WARNING,
                 "[OBS Graphics Studio Pro] Failed to parse the current scene collection titles file; "
                 "keeping the already loaded titles in memory: %s",
                 e.what());
            return;
        }

        notify_change();
        blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to parse scene collection titles file: %s", e.what());
    }
}
