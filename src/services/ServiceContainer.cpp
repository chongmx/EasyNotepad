#include "enp/services/ServiceContainer.h"

#include "enp/services/AgentCliService.h"
#include "enp/services/ProcessService.h"
#include "enp/services/FileService.h"
#include "enp/services/LocalEngineService.h"
#include "enp/services/TerminalService.h"

namespace enp::services {

ServiceContainer ServiceContainer::createDefault()
{
    ServiceContainer c;
    c.terminal = std::make_unique<TerminalService>();
    c.files = std::make_unique<FileService>();
    c.agentCli = std::make_unique<AgentCliService>();
    c.processes = std::make_unique<ProcessService>();
    c.localEngine = std::make_unique<LocalEngineService>();
    return c;
}

} // namespace enp::services
