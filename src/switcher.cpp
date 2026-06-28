#include "switcher.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>
#include <curl/curl.h>          // for fetching broadcaster-id

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace BitrateSwitch {

std::atomic<bool> g_pluginAlive{true};

Switcher::Switcher(Config *config)
    : config_(config)
    , sameTypeStart_(std::chrono::steady_clock::now())
    , offlineStart_(std::chrono::steady_clock::now())
    , streamStartTime_(std::chrono::steady_clock::now())
    , cachedStatusString_("Status: Not started")
    , cachedBitrateString_("Bitrate: --")
{
    prevScene_ = config_->scenes.normal;
    reloadServers();
}

Switcher::~Switcher()
{
    stop();
}

void Switcher::reloadServers()
{
    std::lock_guard<std::mutex> lock(mutex_);
    servers_.clear();
    
    for (const auto &serverConfig : config_->servers) {
        if (serverConfig.enabled) {
            servers_.push_back(StreamServer::create(serverConfig));
        }
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] Loaded %zu servers", servers_.size());
}

void Switcher::start()
{
    if (running_)
        return;

    running_ = true;
    switcherThread_ = std::thread(&Switcher::switcherThread, this);
    blog(LOG_INFO, "[BitrateSceneSwitch] Switcher started");
}

void Switcher::stop()
{
    g_pluginAlive = false;
    running_ = false;
    disconnectChat();
    if (switcherThread_.joinable())
        switcherThread_.join();
    if (refreshThread_.joinable())
        refreshThread_.join();
    blog(LOG_INFO, "[BitrateSceneSwitch] Switcher stopped");
}

void Switcher::onStreamingStarted()
{
    isStreaming_ = true;
    manualOverride_ = false;
    sameTypeStart_ = std::chrono::steady_clock::now();
    offlineStart_ = std::chrono::steady_clock::now();
    streamStartTime_ = std::chrono::steady_clock::now();
    blog(LOG_INFO, "[BitrateSceneSwitch] Streaming started");

    if (config_->options.switchToStartingOnStreamStart && 
        !config_->optionalScenes.starting.empty()) {
        switchToScene(config_->optionalScenes.starting);
        wasOnStartingScene_ = true;
    }

    if (config_->options.recordWhileStreaming && !isRecording_) {
        obs_frontend_recording_start();
    }
}

void Switcher::onStreamingStopped()
{
    isStreaming_ = false;
    wasOnStartingScene_ = false;
    blog(LOG_INFO, "[BitrateSceneSwitch] Streaming stopped");

    if (config_->options.recordWhileStreaming && isRecording_) {
        obs_frontend_recording_stop();
    }

    if (!config_->optionalScenes.ending.empty()) {
        switchToScene(config_->optionalScenes.ending);
    }
}

void Switcher::onSceneChanged()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    obs_source_t *sceneSource = obs_frontend_get_current_scene();
    if (sceneSource) {
        const char *name = obs_source_get_name(sceneSource);
        if (name) {
            currentScene_ = name;
        }
        obs_source_release(sceneSource);
    }
}

void Switcher::onRecordingStarted()
{
    isRecording_ = true;
    blog(LOG_INFO, "[BitrateSceneSwitch] Recording started");
}

void Switcher::onRecordingStopped()
{
    isRecording_ = false;
    blog(LOG_INFO, "[BitrateSceneSwitch] Recording stopped");
}

// ===================================================================
// NEW: Fetch broadcaster-id from Twitch API and start PubSub
// ===================================================================
void Switcher::fetchBroadcasterIdAndStartPubsub(const ChatConfig &chatCfg)
{
    if (chatCfg.oauthToken.empty()) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] Cannot fetch broadcaster-id: OAuth token is empty");
        return;
    }

    std::string token = chatCfg.oauthToken;
    // Strip "oauth:" prefix if present (API expects raw token)
    if (token.rfind("oauth:", 0) == 0)
        token = token.substr(6);

    std::string apiUrl = "https://api.twitch.tv/helix/users?login=" + chatCfg.channel;
    blog(LOG_INFO, "[BitrateSceneSwitch] Fetching broadcaster-id from %s", apiUrl.c_str());

    CURL *curl = curl_easy_init();
    if (!curl) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] Failed to init curl for broadcaster-id fetch");
        return;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, apiUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, 
        [](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
            auto *resp = static_cast<std::string*>(userdata);
            resp->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Client-Id: kimne78kx3ncx0kll9rx3z1q0x2pvl");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] Broadcaster-id fetch failed: %s", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (httpCode != 200) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] Broadcaster-id fetch returned HTTP %ld. Response: %s",
             httpCode, response.c_str());
        return;
    }

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(response), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] Failed to parse broadcaster-id response");
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray dataArr = root.value("data").toArray();
    if (dataArr.isEmpty()) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] No user found for channel '%s' – check channel name", chatCfg.channel.c_str());
        return;
    }

    QString userId = dataArr[0].toObject().value("id").toString();
    if (userId.isEmpty()) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] Broadcaster-id empty in API response");
        return;
    }

    blog(LOG_INFO, "[BitrateSceneSwitch] Fetched broadcaster-id: %s", userId.toUtf8().constData());

    std::lock_guard<std::mutex> lock(chatMutex_);
    if (!twitchPubSub_) return;

    twitchPubSub_->subscribeRaid(userId.toStdString());
    twitchPubSub_->start();
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub started via API lookup");
}

// ===================================================================
// connectChat modified to decouple PubSub from chat and log token
// ===================================================================
void Switcher::connectChat()
{
    ChatConfig chatCfg;
    bool wantPubSub = false;
    {
        config_->lockRead();
        if (!config_->chat.enabled) {
            config_->unlockRead();
            return;
        }
        chatCfg = config_->chat;
        wantPubSub = config_->chat.autoStopStreamOnRaid;
        config_->unlockRead();
    }

    // --- ADDED: log token status ---
    if (chatCfg.oauthToken.empty()) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] Chat OAuth token is EMPTY – chat and PubSub will fail!");
    } else {
        blog(LOG_INFO, "[BitrateSceneSwitch] Chat OAuth token present (length=%zu)", chatCfg.oauthToken.size());
    }

    std::lock_guard<std::mutex> lock(chatMutex_);

    if (twitchPubSub_) {
        twitchPubSub_->stop();
        twitchPubSub_.reset();
    }
    twitchChat_.reset();
    kickChat_.reset();
    pubsubWasConnected_ = false;
    pubsubRetryDelay_ = 0;

    if (chatCfg.platform == ChatPlatform::Kick) {
        kickChat_ = std::make_unique<KickChatClient>();
        kickChat_->setConfig(chatCfg);
        kickChat_->setCommandCallback([this](const ChatMessage &msg) {
            handleChatCommand(msg);
        });
        kickChat_->setRaidCallback([this](const std::string &slug, const std::string &disp) {
            handleRaidStop(slug, disp);
        });
        if (kickChat_->connect())
            blog(LOG_INFO, "[BitrateSceneSwitch] Chat connected (Kick)");
        return;
    }

    twitchChat_ = std::make_unique<ChatClient>();
    twitchChat_->setCommandCallback([this](const ChatMessage &msg) {
        handleChatCommand(msg);
    });
    twitchChat_->setConfig(chatCfg);

    if (wantPubSub) {
        twitchPubSub_ = std::make_unique<TwitchPubSubClient>();
        twitchPubSub_->setRaidCallback([this](const std::string &login, const std::string &disp) {
            handleRaidStop(login, disp);
        });

        // --- ADDED: start PubSub immediately via API lookup ---
        fetchBroadcasterIdAndStartPubsub(chatCfg);
    }

    if (twitchChat_->connect())
        blog(LOG_INFO, "[BitrateSceneSwitch] Chat connected (Twitch)");
}

void Switcher::disconnectChat()
{
    std::lock_guard<std::mutex> lock(chatMutex_);
    if (twitchPubSub_) {
        twitchPubSub_->stop();
        twitchPubSub_.reset();
    }
    twitchChat_.reset();
    kickChat_.reset();
    pubsubWasConnected_ = false;
    blog(LOG_INFO, "[BitrateSceneSwitch] Chat disconnected");
}

bool Switcher::isChatConnected() const
{
    std::lock_guard<std::mutex> lock(chatMutex_);
    if (kickChat_)
        return kickChat_->isConnected();
    if (twitchChat_)
        return twitchChat_->isConnected();
    return false;
}

void Switcher::sendChatMessage(const std::string &text)
{
    std::lock_guard<std::mutex> lock(chatMutex_);
    if (twitchChat_ && twitchChat_->isConnected())
        twitchChat_->sendMessage(text);
}

void Switcher::handleRaidStop(const std::string &targetLogin, const std::string &displayName)
{
    bool autoStop = false;
    bool announce = false;
    ChatPlatform plat = ChatPlatform::Twitch;
    std::string tmpl;

    config_->lockRead();
    autoStop = config_->chat.autoStopStreamOnRaid;
    announce = config_->chat.announceRaidStop;
    plat = config_->chat.platform;
    tmpl = config_->messages.raidStop;
    config_->unlockRead();

    blog(LOG_INFO,
         "[BitrateSceneSwitch] Raid event received: target=%s display=%s",
         targetLogin.c_str(), displayName.c_str());

    if (!autoStop) {
        blog(LOG_INFO, "[BitrateSceneSwitch] Raid stop disabled, ignoring");
        return;
    }
    if (!isStreaming_) {
        blog(LOG_INFO, "[BitrateSceneSwitch] Not streaming, ignoring raid");
        return;
    }
    if (std::chrono::steady_clock::now() - streamStartTime_ < std::chrono::seconds(60)) {
        blog(LOG_INFO, "[BitrateSceneSwitch] Stream started <60s ago, ignoring raid");
        return;
    }

    blog(LOG_INFO, "[BitrateSceneSwitch] Stopping stream due to raid -> %s",
         targetLogin.c_str());

    if (announce && plat == ChatPlatform::Twitch) {
        std::string msg = tmpl;
        const std::string &sub = !targetLogin.empty() ? targetLogin : displayName;
        for (;;) {
            size_t pos = msg.find("{target}");
            if (pos == std::string::npos)
                break;
            msg.replace(pos, 8, sub);
        }
        sendChatMessage(msg);
    }

    obs_queue_task(
        OBS_TASK_UI,
        [](void *) { obs_frontend_streaming_stop(); }, nullptr, false);
}

void Switcher::handleChatCommand(const ChatMessage& msg)
{
    blog(LOG_INFO, "[BitrateSceneSwitch] Chat command from %s: %s", 
         msg.username.c_str(), msg.message.c_str());

    auto reply = [this](const std::string &text) { sendChatMessage(text); };
    auto announce = [this, &reply](const std::string &text) {
        if (config_->chat.announceSceneChanges)
            reply(text);
    };

    switch (msg.command) {
    case ChatCommand::Live:
        manualOverride_ = false;
        switchToLive();
        announce(formatTemplate(config_->messages.sceneSwitched, config_->scenes.normal));
        break;
    case ChatCommand::Low:
        manualOverride_ = true;
        switchToLow();
        announce(formatTemplate(config_->messages.sceneSwitched, config_->scenes.low));
        break;
    case ChatCommand::Brb:
        manualOverride_ = true;
        switchToBrb();
        announce(formatTemplate(config_->messages.sceneSwitched, config_->scenes.offline));
        break;
    case ChatCommand::Privacy:
        if (config_->optionalScenes.privacy.empty()) {
            reply("No privacy scene configured");
        } else {
            manualOverride_ = true;
            switchToPrivacy();
            announce(formatTemplate(config_->messages.sceneSwitched,
                                    config_->optionalScenes.privacy));
        }
        break;
    case ChatCommand::Refresh:
        refreshScene();
        announce(formatTemplate(config_->messages.refreshing));
        break;
    case ChatCommand::Status:
        if (lastBitrateInfo_.isOnline)
            reply(formatTemplate(config_->messages.statusResponse));
        else
            reply(formatTemplate(config_->messages.statusOffline));
        break;
    case ChatCommand::Trigger:
        manualOverride_ = false;
        triggerSwitch();
        announce("Triggered switch check");
        break;
    case ChatCommand::Fix:
        fixMediaSources();
        announce(formatTemplate(config_->messages.fixAttempt));
        break;
    case ChatCommand::SwitchScene:
        if (msg.args.empty()) {
            reply("Usage: " + config_->chat.cmdSwitchScene + " <scene_name>");
        } else if (switchToSceneByName(msg.args)) {
            manualOverride_ = true;
            announce(formatTemplate(config_->messages.sceneSwitched, msg.args));
        } else {
            reply("Scene not found: " + msg.args);
        }
        break;
    case ChatCommand::Start:
        if (isStreaming_) {
            reply("Stream is already running");
        } else {
            obs_queue_task(OBS_TASK_UI, [](void*) {
                obs_frontend_streaming_start();
            }, nullptr, false);
            reply(formatTemplate(config_->messages.streamStarted));
            blog(LOG_INFO, "[BitrateSceneSwitch] Stream started via chat");
        }
        break;
    case ChatCommand::Stop:
        if (!isStreaming_) {
            reply("Stream is not running");
        } else {
            obs_queue_task(OBS_TASK_UI, [](void*) {
                obs_frontend_streaming_stop();
            }, nullptr, false);
            reply(formatTemplate(config_->messages.streamStopped));
            blog(LOG_INFO, "[BitrateSceneSwitch] Stream stopped via chat");
        }
        break;
    case ChatCommand::None:
        handleCustomCommands(msg);
        break;
    default:
        break;
    }
}

void Switcher::announceSceneChange(SwitchType type)
{
    if (!config_->chat.announceSceneChanges)
        return;

    std::string tmpl;
    switch (type) {
    case SwitchType::Normal:
        tmpl = config_->messages.switchedToLive;
        break;
    case SwitchType::Low:
        tmpl = config_->messages.switchedToLow;
        break;
    case SwitchType::Offline:
        tmpl = config_->messages.switchedToOffline;
        break;
    default:
        return;
    }

    sendChatMessage(formatTemplate(tmpl));
}

std::string Switcher::formatTemplate(const std::string &tmpl, const std::string &sceneOverride)
{
    std::string result = tmpl;
    
    auto replaceAll = [](std::string &str, const std::string &from, const std::string &to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };
    
    BitrateInfo info = lastBitrateInfo_;
    std::string scene = sceneOverride.empty() ? getCurrentScene() : sceneOverride;
    
    replaceAll(result, "{bitrate}", std::to_string(info.bitrateKbps));
    replaceAll(result, "{rtt}", std::to_string(static_cast<int>(info.rttMs)));
    replaceAll(result, "{scene}", scene);
    replaceAll(result, "{prev_scene}", prevScene_);
    replaceAll(result, "{server}", info.serverName);
    replaceAll(result, "{status}", info.isOnline ? "Online" : "Offline");
    replaceAll(result, "{uptime}", isStreaming_ ? "Live" : "Not streaming");
    
    return result;
}

void Switcher::handleCustomCommands(const ChatMessage& msg)
{
    if (config_->customCommands.empty()) return;
    
    std::string msgLower = msg.message;
    std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);
    
    for (const auto &cmd : config_->customCommands) {
        if (!cmd.enabled || cmd.trigger.empty()) continue;
        
        std::string triggerLower = cmd.trigger;
        std::transform(triggerLower.begin(), triggerLower.end(), triggerLower.begin(), ::tolower);
        
        if (msgLower == triggerLower || msgLower.rfind(triggerLower + " ", 0) == 0) {
            sendChatMessage(formatTemplate(cmd.response));
            return;
        }
    }
}

BitrateInfo Switcher::getLastBitrateInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastBitrateInfo_;
}

BitrateInfo Switcher::getCurrentBitrate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastBitrateInfo_;
}

std::string Switcher::getStatusString()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!config_->enabled)
        return "Disabled";
    
    if (config_->onlyWhenStreaming && !isStreaming_)
        return "Waiting for stream";
    
    if (servers_.empty())
        return "No servers configured";
    
    if (lastBitrateInfo_.isOnline) {
        return formatTemplate(config_->messages.statusResponse);
    }
    
    return formatTemplate(config_->messages.statusOffline);
}

std::string Switcher::getCachedStatusLine()
{
    std::lock_guard<std::mutex> lock(statusCacheMutex_);
    return cachedStatusString_;
}

std::string Switcher::getCachedBitrateLine()
{
    std::lock_guard<std::mutex> lock(statusCacheMutex_);
    return cachedBitrateString_;
}

// ---- The rest of the Switcher methods (switcherThread, doSwitchCheck, etc.) remain exactly as before ----
// [They are not shown here for brevity, but they are unchanged from the original you provided.]

} // namespace BitrateSwitch
