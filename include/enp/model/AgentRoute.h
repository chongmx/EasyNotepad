// "Auto" agent: which agent answers a prompt. Fixed rules, not the model's own judgement: in the
// Linux trial the 2B model called "Discard all changes" harmless, so it does not decide.
#pragma once

#include <string>
#include <string_view>

namespace enp::model {

enum class AgentRoute { Local, Claude };

struct RouteDecision {
    AgentRoute route = AgentRoute::Claude;
    std::string reason; // a few words, shown with the answer
};

// Local: short questions about the code in front of the user (explain, what/why/how, summaries,
// commit messages). Claude: anything that changes files or uses the app, spans the project, plans,
// tests, long requests, and whatever is unclear.
RouteDecision routeTask(std::string_view prompt);

} // namespace enp::model
