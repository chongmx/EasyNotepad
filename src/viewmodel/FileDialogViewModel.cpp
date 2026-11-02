#include "enp/viewmodel/FileDialogViewModel.h"

#include "enp/viewmodel/NotepadViewModel.h"

#include <algorithm>
#include <cctype>

namespace enp::viewmodel {

namespace {

std::string slashes(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/' && !(path.size() == 3 && path[1] == ':'))
        path.pop_back();
    if (path.size() == 2 && path[1] == ':')
        path += '/'; // "C:" -> "C:/"
    return path;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

FileDialogViewModel::FileDialogViewModel(NotepadViewModel& owner, interfaces::IFileService* files)
    : owner_(owner), files_(files)
{
}

bool FileDialogViewModel::isAbsolute(const std::string& path)
{
    return !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
}

std::string FileDialogViewModel::join(const std::string& folder, const std::string& name)
{
    if (isAbsolute(name) || folder.empty())
        return slashes(name);
    return slashes(folder.back() == '/' ? folder + name : folder + "/" + name);
}

std::string FileDialogViewModel::parent(const std::string& path)
{
    const std::string p = slashes(path);
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos)
        return p;
    if (slash == 0)
        return "/";
    if (slash == 2 && p[1] == ':')
        return p.substr(0, 3); // "C:/"
    return p.substr(0, slash);
}

void FileDialogViewModel::show(FileDialogMode mode, const std::string& folder, const std::string& name)
{
    mode_ = mode;
    fileName = name;
    confirmOverwrite = false;
    error.set({});
    history_.clear();
    folder_ = slashes(files_ ? files_->absolute(folder.empty() ? "." : folder) : folder);
    findPlaces();
    load();
    visible.set(true);
}

void FileDialogViewModel::cancel()
{
    visible.set(false);
    closeAfterSave = -1;
}

void FileDialogViewModel::load()
{
    entries_.clear();
    if (!files_)
        return;
    for (const interfaces::DirectoryEntry& e : files_->list(folder_)) {
        if (e.hidden || (mode_ == FileDialogMode::OpenFolder && !e.isDirectory))
            continue;
        entries_.push_back({e.name, e.isDirectory});
    }
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory;
        return lower(a.name) < lower(b.name);
    });
}

std::vector<FileDialogViewModel::Entry> FileDialogViewModel::visibleEntries() const
{
    // In Open mode a typed name narrows the list; a path (with a slash) does not.
    const bool filter = mode_ == FileDialogMode::Open && !fileName.empty() &&
                        fileName.find_first_of("/\\") == std::string::npos;
    if (!filter)
        return entries_;
    const std::string needle = lower(fileName);
    std::vector<Entry> out;
    for (const Entry& e : entries_)
        if (lower(e.name).find(needle) != std::string::npos)
            out.push_back(e);
    return out;
}

void FileDialogViewModel::findPlaces()
{
    std::vector<std::pair<std::string, std::string>>& out = places_;
    out.clear();
    if (!owner_.workspaceRoot().empty())
        out.emplace_back("Workspace", slashes(owner_.workspaceRoot()));
    const std::string home = owner_.homeDirectory();
    if (!home.empty())
        out.emplace_back("Home", slashes(home));
#if defined(_WIN32)
    // Drive letters from the system's list: no drive is touched (a disconnected network drive or an
    // empty card reader can take seconds to answer, or ask for a disk).
    if (files_)
        for (const std::string& root : files_->roots())
            if (root.size() == 3 && root[1] == ':')
                out.emplace_back(root.substr(0, 2), root);
#else
    out.emplace_back("/", "/");
#endif
}

void FileDialogViewModel::goTo(const std::string& folder)
{
    const std::string target = join(folder_, folder);
    if (files_ && !files_->isDirectory(target)) {
        error.set("Not a folder: " + target);
        return;
    }
    if (target != folder_)
        history_.push_back(folder_);
    folder_ = target;
    error.set({});
    load();
}

void FileDialogViewModel::up()
{
    const std::string p = parent(folder_);
    if (p != folder_)
        goTo(p);
}

void FileDialogViewModel::back()
{
    if (history_.empty())
        return;
    folder_ = history_.back();
    history_.pop_back();
    load();
}

void FileDialogViewModel::choose(const Entry& entry, bool confirmNow)
{
    if (entry.isDirectory) {
        goTo(entry.name);
        if (mode_ == FileDialogMode::Open)
            fileName.clear();
        return;
    }
    fileName = entry.name;
    confirmOverwrite = false;
    if (confirmNow)
        confirm();
}

bool FileDialogViewModel::confirm()
{
    error.set({});
    if (mode_ == FileDialogMode::OpenFolder) {
        const std::string target = fileName.empty() ? folder_ : join(folder_, fileName);
        if (!owner_.openFolder(target)) {
            error.set("Cannot open the folder " + target);
            return false;
        }
        visible.set(false);
        return true;
    }
    if (fileName.empty()) {
        error.set(mode_ == FileDialogMode::Open ? "Choose a file" : "Type a file name");
        return false;
    }
    const std::string path = join(folder_, fileName);
    if (files_ && files_->isDirectory(path)) {
        goTo(path); // a typed folder: go there
        fileName.clear();
        return false;
    }
    if (mode_ == FileDialogMode::Open) {
        const std::optional<interfaces::FileStamp> stamp = files_ ? files_->stamp(path) : std::nullopt;
        if (files_ && stamp && !stamp->exists) {
            error.set("Cannot open " + path);
            return false;
        }
        owner_.openFile(path);
        visible.set(false);
        return true;
    }
    // Save As.
    const std::optional<interfaces::FileStamp> existing = files_ ? files_->stamp(path) : std::nullopt;
    if (existing && existing->exists && !confirmOverwrite) {
        confirmOverwrite = true;
        error.set(fileName + " already exists. Save again to replace it.");
        return false;
    }
    model::Document* doc = owner_.activeDocument();
    if (!doc || !owner_.saveDocumentAs(*doc, path)) {
        error.set("Cannot save " + path);
        return false;
    }
    visible.set(false);
    if (closeAfterSave >= 0) {
        const int index = owner_.documentIndexById(closeAfterSave);
        closeAfterSave = -1;
        if (index >= 0)
            owner_.closeDocument(index);
    }
    return true;
}

} // namespace enp::viewmodel
