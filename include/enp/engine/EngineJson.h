// The local engine's requests and results as JSON: what crosses between the app, the helper process
// (`--local-engine`) and an engine library (EnpEngine.h).
#pragma once

#include "enp/interfaces/ILocalEngineService.h"
#include "enp/model/Json.h"

namespace enp::engine {

model::Json toJson(const interfaces::LocalChatRequest& r);
model::Json toJson(const interfaces::LocalChatResult& r);
model::Json toJson(const interfaces::EngineRunSettings& s);
model::Json toJson(const interfaces::EngineRunResult& r);
model::Json toJson(const interfaces::EngineDevice& d);

interfaces::LocalChatRequest chatRequestFrom(const model::Json& j);
interfaces::LocalChatResult chatResultFrom(const model::Json& j);
interfaces::EngineRunSettings runSettingsFrom(const model::Json& j);
interfaces::EngineRunResult runResultFrom(const model::Json& j);
interfaces::EngineDevice deviceFrom(const model::Json& j);

} // namespace enp::engine
