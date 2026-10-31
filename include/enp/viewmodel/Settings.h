// The settings store: one JSON document in the app's data folder, read once and written through
// the file service shortly after something changes. Owners bind observable properties to keys
// (loaded into them, saved when they change) and register sections for structured data. Sections
// nobody registered (from another version, say) are kept as they are.
#pragma once

#include "enp/interfaces/IFileService.h"
#include "enp/model/Json.h"
#include "enp/viewmodel/Observable.h"

#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace enp::viewmodel {

class Settings {
public:
    explicit Settings(interfaces::IFileService* files);
    ~Settings(); // writes pending changes

    // Loads the file (a corrupt one is kept beside it as "<path>.bad"); empty: session only.
    void setPath(const std::string& path);
    const std::string& path() const { return path_; }

    // "section.name" keys. bool, numbers and enums (stored as their number).
    template <class T>
    void bind(const std::string& key, Property<T>& property)
    {
        // Applied now and again whenever a file is loaded (bindings usually precede setPath).
        auto load = [this, key, &property]() {
            if (const model::Json* value = find(key))
                property.set(fromJson<T>(*value, property.get()));
        };
        load();
        loaders_.push_back(load);
        subscriptions_.push_back(property.subscribe([this, key](const T& v) {
            put(key, toJson(v));
            markDirty();
        }));
        put(key, toJson(property.get()));
    }

    // Structured data: `load` gets the stored section (now, if there is one, and after every
    // setPath); `save` produces it whenever the settings are written.
    void section(const std::string& name, std::function<void(const model::Json&)> load, std::function<model::Json()> save);
    // Keeps the section's current data and stops calling its owner (which is going away).
    void removeSection(const std::string& name);

    void markDirty();               // write soon (see flush)
    void flush(double now);         // writes when dirty for more than a moment
    void save();                    // writes now
    bool dirty() const { return dirty_; }

private:
    struct Section {
        std::string name;
        std::function<void(const model::Json&)> load;
        std::function<model::Json()> save;
    };

    const model::Json* find(const std::string& key) const;
    void put(const std::string& key, model::Json value);

    template <class T>
    static model::Json toJson(const T& v)
    {
        if constexpr (std::is_same_v<T, bool>)
            return model::Json(v);
        else if constexpr (std::is_enum_v<T>)
            return model::Json(int(v));
        else if constexpr (std::is_same_v<T, std::string>)
            return model::Json(v);
        else
            return model::Json(double(v));
    }
    template <class T>
    static T fromJson(const model::Json& j, const T& fallback)
    {
        if constexpr (std::is_same_v<T, bool>)
            return j.asBool(fallback);
        else if constexpr (std::is_enum_v<T>)
            return j.isNumber() ? T(int(j.asNumber())) : fallback;
        else if constexpr (std::is_same_v<T, std::string>)
            return j.isString() ? j.asString() : fallback;
        else
            return j.isNumber() ? T(j.asNumber()) : fallback;
    }

    interfaces::IFileService* files_;
    std::string path_;
    model::Json root_ = model::Json::object();
    std::vector<Section> sections_;
    std::vector<std::function<void()>> loaders_; // bound properties
    std::vector<Subscription> subscriptions_;
    bool dirty_ = false;
    bool loading_ = false;
    double dirtySince_ = -1.0;
};

} // namespace enp::viewmodel
