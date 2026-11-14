// The in-app file dialog (the same on every platform, the web build included, and drivable by
// agents): browse folders, then Open a file, Save As, or Open a folder.
#pragma once

#include "enp/interfaces/IFileService.h"
#include "enp/viewmodel/Observable.h"

#include <string>
#include <utility>
#include <vector>

namespace enp::viewmodel {

class NotepadViewModel;

enum class FileDialogMode { Open, SaveAs, OpenFolder, OpenWorkspace }; // OpenWorkspace: a .code-workspace, .sln or .slnx

class FileDialogViewModel {
public:
    struct Entry {
        std::string name;
        bool isDirectory = false;
    };

    FileDialogViewModel(NotepadViewModel& owner, interfaces::IFileService* files);

    Property<bool> visible{false};
    Property<std::string> error;
    std::string fileName;          // Save As: the name; Open: a name, a path, or a filter
    bool confirmOverwrite = false; // Save As: the file exists, the next confirm() overwrites it

    void show(FileDialogMode mode, const std::string& folder, const std::string& name = {});
    void cancel();
    FileDialogMode mode() const { return mode_; }
    const std::string& folder() const { return folder_; } // absolute, '/' separated

    // Folders first, then files; in Open mode a typed name (not a path) filters them. A name picked
    // from the list (choose()) is not a typed one: the list keeps all its entries around it.
    std::vector<Entry> visibleEntries() const;
    // Workspace, Home and (Windows) drives: label and absolute path.
    // Computed when the dialog opens: checking drives can be slow (network, empty card readers).
    const std::vector<std::pair<std::string, std::string>>& places() const { return places_; }

    // Android without "All files access": only the app's own folders can be listed. The view says
    // so and offers requestStorageAccess() (the system settings); checkStorageAccess(), called
    // every frame, shows the user's files once the access is turned on.
    bool storageAccessMissing() const { return !storageAccess_; }
    void requestStorageAccess();
    void checkStorageAccess();

    void goTo(const std::string& folder); // absolute, or relative to the current folder
    void up();
    void back();
    bool canGoBack() const { return !history_.empty(); }
    // A folder entry navigates; a file entry selects it (and confirms when `confirm` is true).
    void choose(const Entry& entry, bool confirm);
    // Open / Save / Open Folder with the current name; false keeps the dialog open (error, or the
    // overwrite question was raised).
    bool confirm();

    // After a successful Save As, close this document (the "save changes?" prompt chose Save for
    // an untitled document).
    int closeAfterSave = -1;

    static std::string join(const std::string& folder, const std::string& name);
    static std::string parent(const std::string& path);
    static bool isAbsolute(const std::string& path);

private:
    void load();

    NotepadViewModel& owner_;
    interfaces::IFileService* files_;
    FileDialogMode mode_ = FileDialogMode::Open;
    std::string folder_;
    std::vector<Entry> entries_;
    std::string pickedName_; // fileName as set by choose(); the list is filtered only by other text
    std::vector<std::string> history_;
    std::vector<std::pair<std::string, std::string>> places_;
    bool storageAccess_ = true;
    void findPlaces();
};

} // namespace enp::viewmodel
