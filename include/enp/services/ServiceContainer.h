// Composition root: owns the concrete services and hands out their interfaces.
// Services are added here as the dummy data in the view-models is replaced.
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/interfaces/IDocumentReaderService.h"
#include "enp/interfaces/IFileService.h"
#include "enp/interfaces/ILocalEngineService.h"
#include "enp/interfaces/IProcessService.h"
#include "enp/interfaces/ITerminalService.h"

#include <memory>

namespace enp::services {

struct ServiceContainer {
    std::unique_ptr<interfaces::ITerminalService> terminal;
    std::unique_ptr<interfaces::IFileService> files;
    std::unique_ptr<interfaces::IAgentCliService> agentCli;
    std::unique_ptr<interfaces::IProcessService> processes;
    std::unique_ptr<interfaces::ILocalEngineService> localEngine;
    std::unique_ptr<interfaces::IDocumentReaderService> documentReader; // reads through `files`

    // Production services backed by the OS layer.
    static ServiceContainer createDefault();
};

} // namespace enp::services
