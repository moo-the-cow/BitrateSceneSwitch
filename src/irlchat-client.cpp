#include "irlchat-client.hpp"
#include "http-client.hpp"
#include "switcher.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cctype>
#include <utility>

namespace BitrateSwitch {

struct IrlChatCommandPack {
    IrlChatClient::CommandCallback callback;
    ChatMessage message;
};

struct IrlChatRaidPack {
    IrlChatClient::RaidCallback callback;
    std::string username;
    std::string displayName;
};

struct IrlChatAccountPack {
    IrlChatClient::AccountCallback callback;
    std::string twitchUserId;
};

static constexpr const char *kIrlChatApiUrl = "https://irlchat.app";
static constexpr const char *kIrlChatWebSocketUrl = "wss://irlchat.app/ws";

static bool isValidIntegrationToken(const std::string &token)
{
    return token.size() == 64 && std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

static ChatPlatform platformFromString(const QString &platform)
{
    return platform.compare(QLatin1String("kick"), Qt::CaseInsensitive) == 0
               ? ChatPlatform::Kick
               : ChatPlatform::Twitch;
}

IrlChatClient::IrlChatClient() = default;

IrlChatClient::~IrlChatClient()
{
    disconnect();
}

void IrlChatClient::setConfig(const ChatConfig &config)
{
    config_ = config;
}

void IrlChatClient::setCommandCallback(CommandCallback callback)
{
    commandCallback_ = std::move(callback);
}

void IrlChatClient::setRaidCallback(RaidCallback callback)
{
    raidCallback_ = std::move(callback);
}

void IrlChatClient::setAccountCallback(AccountCallback callback)
{
    accountCallback_ = std::move(callback);
}

bool IrlChatClient::connect()
{
    if (running_)
        return true;
    if (!isValidIntegrationToken(config_.irlChatToken)) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] IRLchat: Pairing is required or invalid");
        return false;
    }

    running_ = true;
    worker_ = std::thread(&IrlChatClient::workerMain, this);
    return true;
}

void IrlChatClient::disconnect()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
    ws_.disconnect();
    connected_ = false;
    twitchAvailable_ = false;
    kickAvailable_ = false;
}

bool IrlChatClient::isConnected() const
{
    return connected_;
}

void IrlChatClient::sendMessage(ChatPlatform platform, const std::string &message)
{
    if (message.empty() || message.size() > 500)
        return;
    if ((platform == ChatPlatform::Twitch && !twitchAvailable_) ||
        (platform == ChatPlatform::Kick && !kickAvailable_))
        return;

    QJsonObject payload;
    payload.insert(QLatin1String("type"), QLatin1String("send"));
    payload.insert(QLatin1String("platform"),
                   platform == ChatPlatform::Kick ? QLatin1String("kick")
                                                  : QLatin1String("twitch"));
    payload.insert(QLatin1String("message"), QString::fromStdString(message));
    std::string serialized = QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();

    std::lock_guard<std::mutex> lock(outgoingMutex_);
    if (outgoing_.size() < 100)
        outgoing_.push(std::move(serialized));
}

bool IrlChatClient::pair(const std::string &code, std::string &token, std::string &error)
{
    QJsonObject payload;
    payload.insert(QLatin1String("code"), QString::fromStdString(code));
    payload.insert(QLatin1String("label"), QLatin1String("BitrateSceneSwitch"));

    HttpClient http;
    HttpResponse response = http.post(
        std::string(kIrlChatApiUrl) + "/api/pair/validate",
        QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString(),
        "application/json", 10000);

    QJsonParseError parseError{};
    QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(response.body), &parseError);
    QJsonObject object = document.isObject() ? document.object() : QJsonObject();
    std::string returnedToken = object.value(QLatin1String("token")).toString().toStdString();
    if (response.success && isValidIntegrationToken(returnedToken)) {
        token = std::move(returnedToken);
        error.clear();
        return true;
    }

    QString message = object.value(QLatin1String("error")).toString();
    error = message.isEmpty() ? "Unable to pair with IRLchat" : message.toStdString();
    return false;
}

void IrlChatClient::workerMain()
{
    if (!ws_.connect(kIrlChatWebSocketUrl,
                     "X-Device-Token: " + config_.irlChatToken)) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] IRLchat: Connection failed");
        running_ = false;
        return;
    }

    connected_ = true;
    while (running_) {
        flushOutgoing();
        std::string message;
        WsClient::RecvResult result = ws_.recv(message);
        if (result == WsClient::RecvResult::Message)
            dispatchMessage(message);
        else if (result == WsClient::RecvResult::Error || result == WsClient::RecvResult::Closed)
            break;
    }

    connected_ = false;
    twitchAvailable_ = false;
    kickAvailable_ = false;
    ws_.disconnect();
    running_ = false;
}

void IrlChatClient::flushOutgoing()
{
    for (;;) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(outgoingMutex_);
            if (outgoing_.empty())
                return;
            message = std::move(outgoing_.front());
            outgoing_.pop();
        }
        if (!ws_.send(message))
            return;
    }
}

bool IrlChatClient::isAllowed(const std::string &username, ChatPlatform platform) const
{
    std::string lower = username;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string owner = platform == ChatPlatform::Kick ? kickUsername_ : twitchUsername_;
    std::transform(owner.begin(), owner.end(), owner.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!owner.empty() && lower == owner)
        return true;
    for (const auto &admin : config_.admins) {
        std::string candidate = admin;
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == candidate)
            return true;
    }
    return false;
}

void IrlChatClient::dispatchMessage(const std::string &message)
{
    QJsonParseError error{};
    QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(message), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;

    QJsonObject object = document.object();
    QString type = object.value(QLatin1String("type")).toString();
    if (type == QLatin1String("bss_ready")) {
        QJsonObject accounts = object.value(QLatin1String("accounts")).toObject();
        QJsonObject twitch = accounts.value(QLatin1String("twitch")).toObject();
        QJsonObject kick = accounts.value(QLatin1String("kick")).toObject();
        twitchAvailable_ = !twitch.isEmpty();
        kickAvailable_ = !kick.isEmpty();
        twitchUsername_ = twitch.value(QLatin1String("username")).toString().toStdString();
        kickUsername_ = kick.value(QLatin1String("username")).toString().toStdString();
        std::string twitchUserId = twitch.value(QLatin1String("user_id")).toString().toStdString();
        if (accountCallback_ && !twitchUserId.empty()) {
            auto *pack = new IrlChatAccountPack{accountCallback_, twitchUserId};
            obs_queue_task(OBS_TASK_UI, [](void *data) {
                auto *queued = static_cast<IrlChatAccountPack *>(data);
                if (g_pluginAlive && queued->callback)
                    queued->callback(queued->twitchUserId);
                delete queued;
            }, pack, false);
        }
        return;
    }

    if (type == QLatin1String("error")) {
        std::string code = object.value(QLatin1String("code")).toString().toStdString();
        blog(LOG_WARNING, "[BitrateSceneSwitch] IRLchat: Request failed (%s)",
             code.empty() ? "unknown" : code.c_str());
        return;
    }

    if (type == QLatin1String("event")) {
        QString platform = object.value(QLatin1String("platform")).toString();
        QString eventType = object.value(QLatin1String("event_type")).toString();
        if (platform == QLatin1String("kick") && eventType == QLatin1String("raid_out") && raidCallback_) {
            std::string username = object.value(QLatin1String("username")).toString().toStdString();
            std::string display = object.value(QLatin1String("display_name")).toString().toStdString();
            auto *pack = new IrlChatRaidPack{raidCallback_, std::move(username), std::move(display)};
            obs_queue_task(OBS_TASK_UI, [](void *data) {
                auto *queued = static_cast<IrlChatRaidPack *>(data);
                if (g_pluginAlive && queued->callback)
                    queued->callback(queued->username, queued->displayName);
                delete queued;
            }, pack, false);
        }
        return;
    }

    if (type != QLatin1String("message"))
        return;

    QString platformName = object.value(QLatin1String("platform")).toString();
    if (platformName != QLatin1String("twitch") && platformName != QLatin1String("kick"))
        return;

    ChatMessage chatMessage;
    chatMessage.platform = platformFromString(platformName);
    chatMessage.username = object.value(QLatin1String("username")).toString().toStdString();
    chatMessage.message = object.value(QLatin1String("message")).toString().toStdString();
    if (chatMessage.message.empty() || chatMessage.message.front() != '!' ||
        !isAllowed(chatMessage.username, chatMessage.platform))
        return;

    chatMessage.command = ChatClient::parseCommandForConfig(
        config_, chatMessage.message, chatMessage.args);
    if (!commandCallback_)
        return;

    auto *pack = new IrlChatCommandPack{commandCallback_, std::move(chatMessage)};
    obs_queue_task(OBS_TASK_UI, [](void *data) {
        auto *queued = static_cast<IrlChatCommandPack *>(data);
        if (g_pluginAlive && queued->callback)
            queued->callback(queued->message);
        delete queued;
    }, pack, false);
}

} // namespace BitrateSwitch
