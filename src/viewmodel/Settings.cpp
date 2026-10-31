#include "enp/viewmodel/Settings.h"

namespace enp::viewmodel {

namespace {

constexpr double kWriteDelay = 0.75; // seconds of quiet before a write

} // namespace

Settings::Settings(interfaces::IFileService* files) : files_(files)
{
}

Settings::~Settings()
{
    if (dirty_)
        save();
}

void Settings::setPath(const std::string& path)
{
    path_ = path;
    if (!files_ || path.empty())
        return;
    loading_ = true;
    if (const std::optional<std::string> text = files_->read(path)) {
        std::optional<model::Json> json = model::Json::parse(*text);
        if (json && json->isObject()) {
            // Keys bound before the file was read keep their in-memory value unless stored.
            for (const auto& [key, value] : json->members())
                root_.set(key, value);
        } else {
            files_->write(path + ".bad", *text); // keep what was there; start fresh
        }
    }
    for (const auto& load : loaders_)
        load();
    for (const Section& s : sections_)
        if (const model::Json* stored = root_.find(s.name))
            s.load(*stored);
    loading_ = false;
    dirty_ = false;
}

void Settings::section(const std::string& name, std::function<void(const model::Json&)> load, std::function<model::Json()> save)
{
    sections_.push_back({name, std::move(load), std::move(save)});
    if (const model::Json* stored = root_.find(name)) {
        loading_ = true;
        sections_.back().load(*stored);
        loading_ = false;
    }
}

void Settings::removeSection(const std::string& name)
{
    for (auto it = sections_.begin(); it != sections_.end(); ++it)
        if (it->name == name) {
            root_.set(name, it->save());
            sections_.erase(it);
            return;
        }
}

const model::Json* Settings::find(const std::string& key) const
{
    const size_t dot = key.find('.');
    const model::Json* group = root_.find(key.substr(0, dot));
    if (!group || dot == std::string::npos)
        return group;
    return group->find(key.substr(dot + 1));
}

void Settings::put(const std::string& key, model::Json value)
{
    const size_t dot = key.find('.');
    if (dot == std::string::npos) {
        root_.set(key, std::move(value));
        return;
    }
    const std::string groupName = key.substr(0, dot);
    model::Json group = root_.find(groupName) && root_.find(groupName)->isObject() ? *root_.find(groupName) : model::Json::object();
    group.set(key.substr(dot + 1), std::move(value));
    root_.set(groupName, std::move(group));
}

void Settings::markDirty()
{
    if (loading_)
        return;
    if (!dirty_)
        dirtySince_ = -1.0; // stamped by the next flush
    dirty_ = true;
}

void Settings::flush(double now)
{
    if (!dirty_)
        return;
    if (dirtySince_ < 0.0) {
        dirtySince_ = now;
        return;
    }
    if (now - dirtySince_ >= kWriteDelay)
        save();
}

void Settings::save()
{
    dirty_ = false;
    if (!files_ || path_.empty())
        return;
    for (const Section& s : sections_)
        root_.set(s.name, s.save());
    const size_t slash = path_.find_last_of("/\\");
    if (slash != std::string::npos)
        files_->createDirectories(path_.substr(0, slash));
    files_->write(path_, root_.dump());
}

} // namespace enp::viewmodel
