#include "twitch-pubsub.hpp"
#include "switcher.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BitrateSwitch {

static const char *kPubSubUrl = "wss://pubsub-edge.twitch.tv";
static const int PING_INTERVAL_SECONDS = 240;  // Twitch recommends every 5 minutes
static const int RECV_TIMEOUT_SECONDS = 10;

struct RaidPack {
    TwitchPubSubClient::RaidCallback cb;
    std::string login;
    std::string display;
};

static void queueRaidCallback(TwitchPubSubClient::RaidCallback cb,
                               std::string login, std::string display)
{
    auto *p = new RaidPack{std::move(cb), std::move(login),
                           std::move(display)};
    obs_queue_task(
        OBS_TASK_UI,
        [](void *vp) {
            auto *pack = static_cast<RaidPack *>(vp);
            if (g_pluginAlive && pack->cb)
                pack->cb(pack->login, pack->display);
            delete pack;
        },
        p, false);
}

TwitchPubSubClient::TwitchPubSubClient() = default;

TwitchPubSubClient::~TwitchPubSubClient()
{
    stop();
}

void TwitchPubSubClient::setRaidCallback(RaidCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    raidCb_ = std::move(cb);
}

bool TwitchPubSubClient::validateToken(const std::string &token)
{
    if (token.empty()) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Token is EMPTY");
        return false;
    }
    
    // Check if token has oauth: prefix
    if (token.find("oauth:") == 0) {
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Token has 'oauth:' prefix (will be stripped for PubSub)");
    } else {
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Token does not have 'oauth:' prefix");
    }
    
    // Log token length and first/last few chars for debugging (never log full token)
    std::string cleanToken = token;
    if (cleanToken.find("oauth:") == 0) {
        cleanToken = cleanToken.substr(6);
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Token length: %zu chars, starts with: %s..., ends with: ...%s",
         cleanToken.length(),
         cleanToken.substr(0, std::min(size_t(6), cleanToken.length())).c_str(),
         cleanToken.substr(std::max(size_t(0), cleanToken.length() - 4)).c_str());
    
    // Basic validation
    if (cleanToken.length() < 10) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Token is too short (%zu chars), likely invalid", 
             cleanToken.length());
        return false;
    }
    
    // Check for common issues
    if (cleanToken.find(' ') != std::string::npos) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Token contains spaces - invalid!");
        return false;
    }
    
    if (cleanToken.find('\n') != std::string::npos || cleanToken.find('\r') != std::string::npos) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Token contains newlines - invalid!");
        return false;
    }
    
    return true;
}

void TwitchPubSubClient::setAuthToken(const std::string &token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Setting auth token...");
    
    if (!validateToken(token)) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Token validation FAILED");
        authToken_ = "";
        return;
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Token validation PASSED");
    authToken_ = token;
    
    // If we're connected, resend LISTEN with new token
    if (connected_) {
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Already connected, will resend LISTEN with new token");
        resendListen_ = true;
    }
}

void TwitchPubSubClient::subscribeRaid(const std::string &broadcasterUserId)
{
    if (broadcasterUserId.empty()) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Cannot subscribe - empty broadcaster ID");
        return;
    }
    
    std::string topic = "raid." + broadcasterUserId;
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto &t : topics_) {
        if (t == topic) {
            blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Already subscribed to %s", topic.c_str());
            return;
        }
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Subscribing to topic: %s", topic.c_str());
    topics_.push_back(std::move(topic));
    resendListen_ = true;
}

void TwitchPubSubClient::start()
{
    if (running_.exchange(true)) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Already running");
        return;
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Starting worker thread...");
    worker_ = std::thread([this]() { workerMain(); });
}

void TwitchPubSubClient::stop()
{
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Stopping... (reconnects: %d, pings: %d, pongs: %d, messages: %d)",
         reconnects_, pingsSent_, pongsReceived_, messagesReceived_);
    
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
    
    ws_.disconnect();
    connected_ = false;
    
    // Reset stats
    pingsSent_ = 0;
    pongsReceived_ = 0;
    messagesReceived_ = 0;
    reconnects_ = 0;
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Stopped");
}

bool TwitchPubSubClient::isConnected() const
{
    return connected_;
}

void TwitchPubSubClient::flushListen()
{
    std::vector<std::string> copy;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = topics_;
        token = authToken_;
        resendListen_ = false;
    }
    
    if (copy.empty()) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Cannot LISTEN - no topics subscribed");
        return;
    }
    
    if (token.empty()) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Cannot LISTEN - no auth token set! "
             "You MUST provide a valid OAuth token with 'channel:read:redemptions' scope");
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: To generate a token, go to: "
             "https://twitchtokengenerator.com/ and select 'channel:read:redemptions' scope");
        return;
    }
    
    // Clean token - remove "oauth:" prefix if present (PubSub expects raw token)
    std::string cleanToken = token;
    bool hadOauthPrefix = false;
    if (cleanToken.find("oauth:") == 0) {
        cleanToken = cleanToken.substr(6);
        hadOauthPrefix = true;
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Stripped 'oauth:' prefix for PubSub (IRC needs it, PubSub doesn't)");
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Building LISTEN message for %zu topic(s)", copy.size());
    for (const auto &t : copy) {
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub:   Topic: %s", t.c_str());
    }
    
    QJsonArray qtopics;
    for (const auto &t : copy)
        qtopics.append(QString::fromStdString(t));

    QJsonObject data;
    data["topics"] = qtopics;
    data["auth_token"] = QString::fromStdString(cleanToken);

    QJsonObject root;
    root["type"] = QStringLiteral("LISTEN");
    root["nonce"] = QString::number(++nonce_);
    root["data"] = data;

    std::string json =
        QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
    
    // Log the message without the actual token
    QJsonObject logRoot = root;
    QJsonObject logData = data;
    logData["auth_token"] = QStringLiteral("***REDACTED***");
    logRoot["data"] = logData;
    std::string logJson = QJsonDocument(logRoot).toJson(QJsonDocument::Compact).toStdString();
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending LISTEN: %s", logJson.c_str());
    
    if (!ws_.isConnected()) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Cannot send LISTEN - WebSocket not connected!");
        return;
    }
    
    if (ws_.send(json)) {
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: LISTEN sent successfully (nonce: %d)", nonce_);
    } else {
        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Failed to send LISTEN!");
    }
}

void TwitchPubSubClient::workerMain()
{
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Worker thread started");
    
    // Check token before connecting
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (authToken_.empty()) {
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: No auth token set! Worker exiting.");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: You need to configure an OAuth token in the plugin settings.");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Required scope: channel:read:redemptions");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Generate token at: https://twitchtokengenerator.com/");
            running_ = false;
            return;
        }
    }
    
    while (running_) {
        reconnects_++;
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connection attempt #%d to %s", 
             reconnects_, kPubSubUrl);
        
        // Log system info for debugging
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Using URL: %s", kPubSubUrl);
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Protocol: WSS (WebSocket Secure)");
        
        if (!ws_.connect(kPubSubUrl)) {
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Connection FAILED!");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Possible reasons:");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub:   1. No internet connection");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub:   2. Firewall blocking WebSocket");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub:   3. DNS resolution failed");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub:   4. SSL certificate issues");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub:   5. Proxy configuration issues");
            blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Retrying in 5 seconds...");
            
            connected_ = false;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connected successfully!");
        connected_ = true;

        // Wait for connection to stabilize
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Send LISTEN
        flushListen();

        // Send initial PING to start the keep-alive cycle
        QJsonObject initPing;
        initPing["type"] = QStringLiteral("PING");
        std::string pingJson = QJsonDocument(initPing)
                                   .toJson(QJsonDocument::Compact)
                                   .toStdString();
        blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending initial PING");
        if (ws_.send(pingJson)) {
            pingsSent_++;
        }

        auto lastPing = std::chrono::steady_clock::now();

        while (running_ && ws_.isConnected()) {
            // Check if we need to resend LISTEN
            bool flush = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                flush = resendListen_;
            }
            if (flush) {
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Resending LISTEN due to changes");
                flushListen();
            }

            std::string raw;
            auto result = ws_.recv(raw, RECV_TIMEOUT_SECONDS);

            if (result == WsClient::RecvResult::Timeout) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - lastPing).count();
                
                blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Recv timeout (elapsed since last ping: %llds)", 
                     elapsed);
                
                if (elapsed >= PING_INTERVAL_SECONDS) {
                    QJsonObject ping;
                    ping["type"] = QStringLiteral("PING");
                    std::string pj = QJsonDocument(ping)
                                         .toJson(QJsonDocument::Compact)
                                         .toStdString();
                    
                    blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Sending PING (interval)");
                    if (ws_.send(pj)) {
                        pingsSent_++;
                        lastPing = std::chrono::steady_clock::now();
                    } else {
                        blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Failed to send PING - connection may be dead");
                        break;
                    }
                }
                continue;
            }

            if (result == WsClient::RecvResult::Disconnected) {
                blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: WebSocket disconnected by server");
                break;
            }

            if (result != WsClient::RecvResult::Message) {
                blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Receive error");
                break;
            }

            messagesReceived_++;
            blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Received raw message (%zu bytes): %s", 
                 raw.size(), raw.c_str());

            QJsonParseError err{};
            QJsonDocument doc =
                QJsonDocument::fromJson(QByteArray::fromStdString(raw), &err);
            
            if (err.error != QJsonParseError::NoError) {
                blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Failed to parse JSON: %s (offset: %d)", 
                     err.errorString().toUtf8().constData(), err.offset);
                blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Raw data: %s", raw.c_str());
                continue;
            }
            
            if (!doc.isObject()) {
                blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Received non-object JSON");
                continue;
            }
            
            QJsonObject o = doc.object();
            QString msgType = o.value(QLatin1String("type")).toString();
            
            blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Received message type: %s", 
                 msgType.toUtf8().constData());

            if (msgType == QLatin1String("PONG")) {
                pongsReceived_++;
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: PONG received (total: %d)", pongsReceived_);
                continue;
            }

            if (msgType == QLatin1String("RECONNECT")) {
                blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Server requested RECONNECT");
                break;
            }

            if (msgType == QLatin1String("RESPONSE")) {
                QJsonValue nonceVal = o.value(QLatin1String("nonce"));
                QString nonceStr = nonceVal.toString();
                QString error = o.value(QLatin1String("error")).toString();
                
                if (!error.isEmpty()) {
                    blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: LISTEN error for nonce %s: %s", 
                         nonceStr.toUtf8().constData(), error.toUtf8().constData());
                    
                    // Detailed error analysis
                    if (error.contains("ERR_BADAUTH", Qt::CaseInsensitive)) {
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: AUTHENTICATION FAILED!");
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Your OAuth token is invalid or expired.");
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Make sure you have the 'channel:read:redemptions' scope.");
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Generate a new token at: https://twitchtokengenerator.com/");
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: IMPORTANT: Select 'channel:read:redemptions' when generating!");
                    } else if (error.contains("ERR_BADTOPIC", Qt::CaseInsensitive)) {
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: BAD TOPIC! The broadcaster ID might be wrong.");
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Make sure you're using the correct broadcaster ID (numeric).");
                    } else if (error.contains("ERR_TOOMANY", Qt::CaseInsensitive)) {
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: TOO MANY TOPICS! You're subscribed to too many topics.");
                    } else if (error.contains("ERR_SERVER", Qt::CaseInsensitive)) {
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: SERVER ERROR! Twitch might be having issues.");
                    } else {
                        blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Unknown error: %s", error.toUtf8().constData());
                    }
                    
                    // Don't break on error - let the user fix the token
                    continue;
                }
                
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: LISTEN acknowledged successfully (nonce: %s)", 
                     nonceStr.toUtf8().constData());
                continue;
            }

            if (msgType == QLatin1String("MESSAGE")) {
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Processing MESSAGE...");
                
                RaidCallback cbCopy;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cbCopy = raidCb_;
                }

                QJsonObject data = o.value(QLatin1String("data")).toObject();
                QString topic = data.value(QLatin1String("topic")).toString();
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Message topic: %s", topic.toUtf8().constData());
                
                // The message can be either a string or a JSON object
                QString innerStr;
                QJsonValue msgVal = data.value(QLatin1String("message"));
                
                if (msgVal.isString()) {
                    innerStr = msgVal.toString();
                } else if (msgVal.isObject()) {
                    QJsonObject msgObj = msgVal.toObject();
                    innerStr = QJsonDocument(msgObj).toJson(QJsonDocument::Compact);
                } else if (msgVal.isArray()) {
                    QJsonArray msgArr = msgVal.toArray();
                    innerStr = QJsonDocument(msgArr).toJson(QJsonDocument::Compact);
                }
                
                blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Inner message: %s", 
                     innerStr.toUtf8().constData());
                
                if (innerStr.isEmpty()) {
                    blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Empty message content");
                    continue;
                }

                QJsonDocument innerDoc =
                    QJsonDocument::fromJson(innerStr.toUtf8(), &err);
                
                if (err.error != QJsonParseError::NoError) {
                    blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Failed to parse inner JSON: %s", 
                         err.errorString().toUtf8().constData());
                    continue;
                }
                
                if (!innerDoc.isObject()) {
                    blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Inner message is not an object");
                    continue;
                }
                
                QJsonObject innerObj = innerDoc.object();
                QString eventType = innerObj.value(QLatin1String("type")).toString();
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Event type: %s", eventType.toUtf8().constData());
                
                if (eventType != QLatin1String("raid_go_v2") && 
                    eventType != QLatin1String("raid_go")) {
                    blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Not a raid event (type: %s)", 
                         eventType.toUtf8().constData());
                    continue;
                }
                
                QJsonObject raid = innerObj.value(QLatin1String("raid")).toObject();
                QString targetLogin = raid.value(QLatin1String("target_login")).toString();
                QString display = raid.value(QLatin1String("target_display_name")).toString();
                
                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Raid data - target_login: %s, display: %s", 
                     targetLogin.toUtf8().constData(), display.toUtf8().constData());
                
                if (targetLogin.isEmpty()) {
                    blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Empty target_login in raid event");
                    continue;
                }

                blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: *** RAID DETECTED *** %s (%s)", 
                     targetLogin.toUtf8().constData(), display.toUtf8().constData());

                auto now = std::chrono::steady_clock::now();
                if (haveLastRaidEmit_ &&
                    std::chrono::duration_cast<std::chrono::seconds>(now - lastRaidEmit_).count() < 10) {
                    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Duplicate raid suppressed (10s cooldown)");
                    continue;
                }
                haveLastRaidEmit_ = true;
                lastRaidEmit_ = now;

                if (cbCopy) {
                    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Triggering raid callback");
                    queueRaidCallback(std::move(cbCopy),
                                    targetLogin.toStdString(),
                                    display.toStdString());
                } else {
                    blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: No raid callback set!");
                }
            }
        }

        blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Connection ended (stats: pings=%d, pongs=%d, messages=%d)", 
             pingsSent_, pongsReceived_, messagesReceived_);
        ws_.disconnect();
        connected_ = false;
        
        // Wait before reconnecting
        if (running_) {
            blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Reconnecting in 5 seconds...");
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Worker thread exiting");
    connected_ = false;
}

} // namespace BitrateSwitch
