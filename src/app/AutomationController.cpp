#include "AutomationController.h"

#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/DummyData.h"

#include <algorithm>

namespace enp::app {

using model::Json;
using viewmodel::CommandId;

namespace {

Json schema(std::initializer_list<std::pair<const char*, const char*>> stringProps, std::vector<const char*> required = {},
            std::initializer_list<std::pair<const char*, const char*>> numberProps = {})
{
    Json properties = Json::object();
    for (const auto& [name, description] : stringProps)
        properties.set(name, Json::object().set("type", "string").set("description", description));
    for (const auto& [name, description] : numberProps)
        properties.set(name, Json::object().set("type", "number").set("description", description));
    Json s = Json::object().set("type", "object").set("properties", std::move(properties));
    if (!required.empty()) {
        Json r = Json::array();
        for (const char* name : required)
            r.push(name);
        s.set("required", std::move(r));
    }
    return s;
}

Json error(const std::string& message)
{
    return Json::object().set("error", message);
}

const char* panelTabName(viewmodel::PanelTab tab)
{
    switch (tab) {
    case viewmodel::PanelTab::Problems: return "Problems";
    case viewmodel::PanelTab::Diff: return "Diff";
    case viewmodel::PanelTab::Comments: return "Comments";
    case viewmodel::PanelTab::Checklist: return "Checklist";
    case viewmodel::PanelTab::Terminal: return "Terminal";
    case viewmodel::PanelTab::Search: return "Search";
    }
    return "";
}

Json documentInfo(const model::Document& doc)
{
    return Json::object()
        .set("path", doc.path.empty() ? doc.title : doc.path)
        .set("title", doc.title)
        .set("language", model::languageName(doc.language))
        .set("lines", doc.buffer.lineCount())
        .set("modified", doc.modified)
        .set("caret", Json::object().set("line", doc.caret.line + 1).set("column", doc.caret.column + 1));
}

Json getState(viewmodel::NotepadViewModel& vm)
{
    Json documents = Json::array();
    for (const auto& doc : vm.documents())
        documents.push(documentInfo(*doc));
    Json state = Json::object()
                     .set("workspace", vm.workspace.get() == viewmodel::WorkspaceMode::Agent ? "Agent" : "Code")
                     .set("detailLevel", viewmodel::NotepadViewModel::detailLevelName(vm.detailLevel.get()))
                     .set("documents", std::move(documents))
                     .set("panel", Json::object()
                                       .set("visible", vm.showPanel.get())
                                       .set("tab", panelTabName(vm.panelTab.get()))
                                       .set("maximized", vm.panelMaximized.get()))
                     .set("openFindings", int(vm.review().visibleFindings().size()))
                     .set("terminals", int(vm.terminal().terminals().size()))
                     .set("status", vm.statusText.get());
    if (const model::Document* doc = vm.activeDocument())
        state.set("activeDocument", documentInfo(*doc));
    return state;
}

Json listElements(const Json& params)
{
    const std::string role = params.getString("role");
    const std::string contains = params.getString("contains");
    Json elements = Json::array();
    for (const view::UiElement& e : view::UiAutomation::instance().elements()) {
        if (!role.empty() && e.role != role)
            continue;
        if (!contains.empty() && e.id.find(contains) == std::string::npos && e.label.find(contains) == std::string::npos)
            continue;
        elements.push(Json::object()
                          .set("id", e.id)
                          .set("role", e.role)
                          .set("label", e.label)
                          .set("enabled", e.enabled)
                          .set("clickable", bool(e.activate))
                          .set("rect", Json::object()
                                           .set("x", int(e.min.x))
                                           .set("y", int(e.min.y))
                                           .set("width", int(e.max.x - e.min.x))
                                           .set("height", int(e.max.y - e.min.y))));
    }
    return Json::object().set("elements", std::move(elements));
}

Json click(viewmodel::NotepadViewModel& vm, const Json& params)
{
    const std::string id = params.getString("id");
    const std::string message = params.getString("message");
    if (const view::UiElement* e = view::UiAutomation::instance().find(id)) {
        if (!e->enabled)
            return error("'" + id + "' is disabled");
        if (!e->activate)
            return error("'" + id + "' cannot be clicked");
        const std::string label = e->label;
        auto activate = e->activate; // copy: activation may rebuild the registry
        vm.pointAt(id, message.empty() ? "Clicking " + label : message, true, 3.0);
        activate();
        return Json::object().set("clicked", id).set("label", label);
    }
    CommandId command;
    if (id.rfind("command:", 0) == 0 && viewmodel::commandFromKey(std::string_view(id).substr(8), command)) {
        if (!vm.command(command).canExecute())
            return error("'" + id + "' is not available right now");
        vm.pointAt(id, message.empty() ? std::string("Running ") + viewmodel::commandInfo(command).label : message, true, 3.0);
        vm.execute(command);
        return Json::object().set("clicked", id).set("note", "not on screen; the command was run directly");
    }
    return error("no element '" + id + "' on screen; call list_elements to see what is there");
}

Json readOutline(viewmodel::NotepadViewModel& vm)
{
    const model::Document* doc = vm.activeDocument();
    if (!doc)
        return error("no document is open");
    Json nodes = Json::array();
    for (const model::OutlineNode& n : vm.outline())
        nodes.push(Json::object()
                       .set("title", n.title)
                       .set("kind", model::outlineKindName(n.kind))
                       .set("startLine", n.startLine + 1)
                       .set("endLine", n.endLine + 1)
                       .set("depth", n.depth)
                       .set("folded", vm.isFolded(n))
                       .set("summary", vm.summary(n)));
    return Json::object().set("path", doc->path.empty() ? doc->title : doc->path).set("regions", std::move(nodes));
}

} // namespace

const std::vector<AutomationTool>& automationTools()
{
    static const std::vector<AutomationTool> tools = {
        {"get_state", "Current state of EasyNotepad: workspace, open documents, active document and caret, detail level, bottom panel, findings, terminals.",
         schema({})},
        {"list_elements",
         "Interactive elements currently on screen, each with an id usable by click/point (e.g. command:ToggleTerminal, "
         "ribbon-tab:Read, document:src/main.cpp, file:..., panel-tab:Terminal, layer:20, fold:20, agent-prompt).",
         schema({{"role", "Only this role: button, tab, file, region, panel, textbox"},
                 {"contains", "Only ids or labels containing this text"}})},
        {"click",
         "Clicks a UI element by id, without using the user's mouse; the agent cursor shows the click. "
         "command:<Key> ids also work when the button is not on screen.",
         schema({{"id", "Element id from list_elements, or command:<Key>"}, {"message", "Optional note shown next to the agent cursor"}},
                {"id"})},
        {"point",
         "Moves the glowing agent cursor to an element and shows a message, to show the user where to click or to explain something. Does not click.",
         schema({{"id", "Element id"}, {"message", "What to tell the user"}}, {"id", "message"},
                {{"seconds", "How long to show it (default 8; 0 keeps it until hide_pointer)"}})},
        {"hide_pointer", "Hides the agent cursor.", schema({})},
        {"list_commands", "All commands with their keys (for execute_command), labels, availability and toggle state.", schema({})},
        {"execute_command", "Runs a command by key, e.g. ToggleTerminal, DetailOverview, RunAllReviews, FoldAll.",
         schema({{"command", "Command key from list_commands"}}, {"command"})},
        {"open_file", "Opens a workspace file in the editor.", schema({{"path", "Workspace-relative path, e.g. src/main.cpp"}}, {"path"})},
        {"read_document", "Full text of a document (the active one when no path is given).",
         schema({{"path", "Workspace-relative path; optional"}})},
        {"read_outline", "Structure of the active document: regions with line ranges, fold state and their AI summaries.", schema({})},
        {"set_detail_level", "Sets how much code the user sees: Full, Balanced, Overview or Outline.",
         schema({{"level", "Full | Balanced | Overview | Outline"}}, {"level"})},
        {"type_text", "Inserts text at the caret of the active document (replacing the selection). With as_keys "
                      "\"true\" it is typed key by key like a user: Enter and closing brackets / keywords get smart indentation.",
         schema({{"text", "Text to insert"}, {"as_keys", "\"true\" to type key by key; default: insert as is"}}, {"text"})},
        {"run_in_terminal", "Runs a shell command in the integrated terminal (opening it if needed).",
         schema({{"command", "Command line to run"}}, {"command"})},
        {"set_language", "Sets the active document's language (colouring, folding, indentation) by its key or name, "
                         "e.g. cpp, python, cmake, markdown, yaml, csv.",
         schema({{"language", "Language key or name"}}, {"language"})},
        {"search_files", "Find in Files over the workspace (open files in their unsaved text); shows the results in the "
                         "Search tab and returns them: path, line, column and the line's text.",
         schema({{"query", "Text or regular expression"},
                 {"include", "File filter such as *.cpp;*.h (optional)"},
                 {"regex", "\"true\" for a regular expression"},
                 {"match_case", "\"true\" to match case"},
                 {"whole_word", "\"true\" for whole words"}},
                {"query"})},
        {"read_terminal", "The active terminal's screen as text (what the user sees: a prompt, or a full-screen program "
                          "such as top), its title, cursor and whether it is on the alternate screen.",
         schema({{"scrollback", "Lines of scrollback to include above the screen (default 0)"}})},
        {"send_keys", "Sends raw keys to the active terminal: text, or named keys {Enter}, {Esc}, {Tab}, {Up}, {Down}, "
                      "{Left}, {Right}, {F1}..{F12}, {Ctrl+C} (for interactive programs like top, htop, vim).",
         schema({{"keys", "Text with optional {Key} names"}}, {"keys"})},
    };
    return tools;
}

Json handleAutomation(viewmodel::NotepadViewModel& vm, const std::string& method, const Json& params)
{
    if (method == "get_state")
        return getState(vm);
    if (method == "list_elements")
        return listElements(params);
    if (method == "click")
        return click(vm, params);
    if (method == "point") {
        const std::string id = params.getString("id");
        if (id.empty())
            return error("'id' is required");
        vm.pointAt(id, params.getString("message"), false, params.getNumber("seconds", 8.0));
        const bool visible = view::UiAutomation::instance().find(id) != nullptr || id.rfind("command:", 0) == 0;
        return Json::object().set("pointing", id).set("onScreen", visible);
    }
    if (method == "hide_pointer") {
        vm.hidePointer();
        return Json::object().set("hidden", true);
    }
    if (method == "list_commands") {
        Json commands = Json::array();
        for (int i = 0; i < int(CommandId::Count); ++i) {
            const CommandId id = CommandId(i);
            const viewmodel::CommandInfo& info = viewmodel::commandInfo(id);
            commands.push(Json::object()
                              .set("key", viewmodel::commandKey(id))
                              .set("label", info.label)
                              .set("description", info.tooltip)
                              .set("shortcut", info.shortcut)
                              .set("enabled", vm.command(id).canExecute())
                              .set("checked", vm.isChecked(id)));
        }
        return Json::object().set("commands", std::move(commands));
    }
    if (method == "execute_command") {
        CommandId id;
        if (!viewmodel::commandFromKey(params.getString("command"), id))
            return error("unknown command '" + params.getString("command") + "'");
        if (!vm.command(id).canExecute())
            return error(std::string("'") + viewmodel::commandKey(id) + "' is not available right now");
        vm.execute(id);
        return Json::object().set("executed", viewmodel::commandKey(id)).set("checked", vm.isChecked(id));
    }
    if (method == "open_file") {
        const std::string path = params.getString("path");
        if (path.empty())
            return error("'path' is required");
        vm.openFile(path);
        return documentInfo(*vm.activeDocument());
    }
    if (method == "read_document") {
        const std::string path = params.getString("path");
        for (const auto& doc : vm.documents())
            if (path.empty() ? doc.get() == vm.activeDocument() : (doc->path == path || doc->title == path))
                return documentInfo(*doc).set("text", doc->buffer.text());
        if (path.empty())
            return error("no document is open");
        return Json::object().set("path", path).set("open", false).set("text", viewmodel::dummy::fileContent(path));
    }
    if (method == "read_outline")
        return readOutline(vm);
    if (method == "set_detail_level") {
        const std::string level = params.getString("level");
        for (int i = 0; i < viewmodel::NotepadViewModel::kDetailLevels; ++i)
            if (level == viewmodel::NotepadViewModel::detailLevelName(i)) {
                vm.detailLevel.set(i);
                return Json::object().set("detailLevel", level);
            }
        return error("level must be Full, Balanced, Overview or Outline");
    }
    if (method == "type_text") {
        if (!vm.activeDocument())
            return error("no document is open");
        const std::string text = params.getString("text");
        const Json* keys = params.find("as_keys");
        if (keys && (keys->asBool() || keys->asString() == "true")) {
            for (char c : text) {
                if (c == '\n')
                    vm.newline();
                else if (c != '\r')
                    vm.insertText(std::string(1, c));
            }
        } else {
            vm.insertText(text);
        }
        return documentInfo(*vm.activeDocument());
    }
    if (method == "run_in_terminal") {
        const std::string command = params.getString("command");
        if (command.empty())
            return error("'command' is required");
        if (!vm.terminal().supported())
            return error("terminals are not available on this platform");
        vm.showPanelTab(viewmodel::PanelTab::Terminal);
        if (!vm.terminal().active() || vm.terminal().active()->exited)
            vm.terminal().newTerminal(0);
        if (!vm.terminal().active())
            return error(vm.terminal().lastError.get());
        vm.terminal().sendInput(command + "\r");
        return Json::object().set("sent", command);
    }
    if (method == "set_language") {
        if (!vm.activeDocument())
            return error("no document is open");
        std::string wanted = params.getString("language");
        for (char& c : wanted)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        for (const model::LanguageSpec& spec : model::languages()) {
            std::string name = spec.name;
            for (char& c : name)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            if (wanted == spec.key || wanted == name) {
                vm.setDocumentLanguage(spec.id);
                return documentInfo(*vm.activeDocument());
            }
        }
        std::string keys;
        for (const model::LanguageSpec& spec : model::languages())
            keys += (keys.empty() ? "" : ", ") + std::string(spec.key);
        return error("unknown language '" + params.getString("language") + "'; one of: " + keys);
    }
    if (method == "search_files") {
        auto flag = [&](const char* key) {
            const Json* v = params.find(key);
            return v && (v->asBool() || v->asString() == "true");
        };
        viewmodel::FindInFilesViewModel& find = vm.findInFiles();
        find.query = params.getString("query");
        find.include = params.getString("include");
        find.options.regex = flag("regex");
        find.options.matchCase = flag("match_case");
        find.options.wholeWord = flag("whole_word");
        find.searchNow();
        vm.showPanelTab(viewmodel::PanelTab::Search);
        if (!find.error.get().empty())
            return error(find.error.get());
        Json hits = Json::array();
        int shown = 0;
        for (const auto& file : find.results())
            for (const auto& hit : file.hits) {
                if (++shown > 200)
                    break;
                hits.push(Json::object().set("path", file.path).set("line", hit.line + 1).set("column", hit.column + 1).set("text", hit.text));
            }
        return Json::object()
            .set("matches", find.hitCount())
            .set("files", int(find.results().size()))
            .set("searched", find.filesSearched())
            .set("results", std::move(hits));
    }
    if (method == "read_terminal") {
        const viewmodel::TerminalInstance* term = vm.terminal().active();
        if (!term)
            return error("no terminal is open");
        const model::TerminalScreen& screen = term->screen;
        const int scrollback = std::clamp(int(params.getNumber("scrollback", 0)), 0, screen.scrollbackSize());
        Json lines = Json::array();
        for (int l = screen.scrollbackSize() - scrollback; l < screen.totalLines(); ++l)
            lines.push(screen.text(l, 0, l, screen.columns()));
        return Json::object()
            .set("name", term->name)
            .set("title", screen.title())
            .set("columns", screen.columns())
            .set("rows", screen.rows())
            .set("alternateScreen", screen.alternateScreen())
            .set("cursor", Json::object().set("row", screen.cursorRow()).set("column", screen.cursorColumn()))
            .set("exited", term->exited)
            .set("lines", std::move(lines));
    }
    if (method == "send_keys") {
        if (!vm.terminal().active())
            return error("no terminal is open");
        const std::string keys = params.getString("keys");
        static const std::pair<const char*, const char*> kNamed[] = {
            {"Enter", "\r"}, {"Esc", "\x1b"}, {"Tab", "\t"}, {"Backspace", "\x7f"}, {"Up", "\x1b[A"}, {"Down", "\x1b[B"},
            {"Right", "\x1b[C"}, {"Left", "\x1b[D"}, {"Home", "\x1b[H"}, {"End", "\x1b[F"}, {"PageUp", "\x1b[5~"},
            {"PageDown", "\x1b[6~"}, {"F1", "\x1bOP"}, {"F2", "\x1bOQ"}, {"F3", "\x1bOR"}, {"F4", "\x1bOS"},
            {"F5", "\x1b[15~"}, {"F6", "\x1b[17~"}, {"F7", "\x1b[18~"}, {"F8", "\x1b[19~"}, {"F9", "\x1b[20~"},
            {"F10", "\x1b[21~"}, {"F11", "\x1b[23~"}, {"F12", "\x1b[24~"}, {"Ctrl+C", "\x03"}, {"Ctrl+D", "\x04"},
            {"Ctrl+Z", "\x1a"}, {"Ctrl+L", "\x0c"}};
        std::string bytes;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == '{') {
                const size_t close = keys.find('}', i);
                if (close != std::string::npos) {
                    const std::string name = keys.substr(i + 1, close - i - 1);
                    bool known = false;
                    for (const auto& [n, seq] : kNamed)
                        if (name == n) {
                            bytes += seq;
                            known = true;
                        }
                    if (!known)
                        return error("unknown key {" + name + "}");
                    i = close;
                    continue;
                }
            }
            bytes += keys[i];
        }
        vm.terminal().sendInput(bytes);
        return Json::object().set("sent", int(bytes.size()));
    }
    return error("unknown method '" + method + "'");
}

} // namespace enp::app
