// Automation methods behind the MCP connector, implemented on the view-model and the UI registry.
// Every method runs on the UI thread (AutomationServer marshals requests there).
#pragma once

#include "enp/model/Json.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>
#include <vector>

namespace enp::app {

struct AutomationTool {
    const char* name;
    const char* description;
    model::Json inputSchema; // JSON Schema of the arguments object
};

// The methods, with descriptions and argument schemas (also the MCP tool list).
const std::vector<AutomationTool>& automationTools();

// Returns the method's result, or {"error": "..."}.
model::Json handleAutomation(viewmodel::NotepadViewModel& vm, const std::string& method, const model::Json& params);

} // namespace enp::app
