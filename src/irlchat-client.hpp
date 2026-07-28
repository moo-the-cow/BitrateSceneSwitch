#pragma once

#include "chat-client.hpp"
#include "config.hpp"
#include "ws-client.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace BitrateSwitch {

class IrlChatClient {
public:
    using CommandCallback = std::function<void(const ChatMessage &)>;
    using RaidCallback = std::function<void(const std::string &, const std::string &)>;
    using AccountCallback = std::function<void(const std::string &)>;

    IrlChatClient();
    ~IrlChatClient();

    void setConfig(const ChatConfig &config);
    void setCommandCallback(CommandCallback callback);
    void setRaidCallback(RaidCallback callback);
    void setAccountCallback(AccountCallback callback);

    bool connect();
    void disconnect();
    bool isConnected() const;
    void sendMessage(ChatPlatform platform, const std::string &message);

    static bool pair(const std::string &code, std::string &token, std::string &error);

private:
    void workerMain();
    void dispatchMessage(const std::string &message);
    void flushOutgoing();
    bool isAllowed(const std::string &username, ChatPlatform platform) const;

    ChatConfig config_;
    CommandCallback commandCallback_;
    RaidCallback raidCallback_;
    AccountCallback accountCallback_;
    WsClient ws_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> twitchAvailable_{false};
    std::atomic<bool> kickAvailable_{false};
    std::mutex outgoingMutex_;
    std::queue<std::string> outgoing_;
    std::string twitchUsername_;
    std::string kickUsername_;
};

} // namespace BitrateSwitch
