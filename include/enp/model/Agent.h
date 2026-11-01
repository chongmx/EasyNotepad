// Agent session data: conversation, plan and proposed file changes.
#pragma once

#include "enp/model/Diff.h"

#include <string>
#include <utility>
#include <vector>

namespace enp::model {

enum class ChatRole { User, Agent, Tool };

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::string text;
    std::string by; // an answer: "Local" or "Claude" (empty: sample data or unknown)

    ChatMessage() = default;
    ChatMessage(ChatRole r, std::string t, std::string answeredBy = {})
        : role(r), text(std::move(t)), by(std::move(answeredBy))
    {
    }
};

enum class TaskState { Pending, Running, Done };

struct AgentTask {
    std::string title;
    TaskState state = TaskState::Pending;
};

enum class ChangeState { Proposed, Accepted, Rejected };

struct FileChange {
    std::string path;
    std::string before;
    std::string after;
    std::vector<DiffRow> diff;
    DiffStats stats;
    ChangeState state = ChangeState::Proposed;
};

} // namespace enp::model
