#include "twitch-pubsub.hpp"
#include "switcher.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <fstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BitrateSwitch {

static const char *kPubSubUrl = "wss://pubsub-edge.twitch.tv";

// Helper to get the plugin's config file path (cross‑platform)
static std::string getConfigFilePath()
{
	// OBS stores plugin configs relative to the config directory
	char *configPath = obs_module_config_path(nullptr);
	std::string path;
	if (configPath) {
		path = configPath;
		bfree(configPath);
	}
	// The file is saved as "BitrateSceneSwitch.json" by the plugin's Config::save()
	if (path.empty())
		path = "BitrateSceneSwitch.json";
	else
		path += "/BitrateSceneSwitch.json";
	return path;
}

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

void TwitchPubSubClient::loadTokenFromConfig()
{
	std::string path = getConfigFilePath();
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Loading token from %s", path.c_str());

	obs_data_t *data = obs_data_create_from_json_file(path.c_str());
	if (!data) {
		blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Config file not found or invalid JSON");
		return;
	}

	const char *token = obs_data_get_string(data, "chat_oauth_token");
	if (token && *token) {
		std::lock_guard<std::mutex> lock(mutex_);
		authToken_ = token;
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Token loaded from config (length=%zu)", authToken_.size());
	} else {
		blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: No 'chat_oauth_token' in config file");
	}

	obs_data_release(data);
	tokenLoaded_ = true;
}

void TwitchPubSubClient::subscribeRaid(const std::string &broadcasterUserId)
{
	if (broadcasterUserId.empty())
		return;
	std::string topic = "raid." + broadcasterUserId;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &t : topics_) {
		if (t == topic)
			return;
	}
	topics_.push_back(std::move(topic));
	resendListen_ = true;
}

void TwitchPubSubClient::start()
{
	if (running_.exchange(true))
		return;
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Starting worker thread");
	worker_ = std::thread([this]() { workerMain(); });
}

void TwitchPubSubClient::stop()
{
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: stop() called, running_=%d, connected_=%d",
	     (int)running_, (int)connected_);
	running_ = false;
	if (worker_.joinable())
		worker_.join();
	ws_.disconnect();
	connected_ = false;
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: stop() complete");
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
	if (copy.empty())
		return;

	// Auto-load token if not done yet
	if (!tokenLoaded_) {
		loadTokenFromConfig();
		std::lock_guard<std::mutex> lock(mutex_);
		token = authToken_;
	}

	if (token.empty()) {
		blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: No auth token available – Twitch will reject LISTEN with ERR_BADAUTH");
		blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Make sure you have set a valid OAuth token in the plugin settings");
		return;
	}
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Using auth token (length=%zu)", token.size());

	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Building LISTEN for %zu topic(s):", copy.size());
	for (const auto &t : copy)
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub:   Topic: %s", t.c_str());

	QJsonArray qtopics;
	for (const auto &t : copy)
		qtopics.append(QString::fromStdString(t));

	QJsonObject data;
	data["topics"] = qtopics;
	data["auth_token"] = QString::fromStdString(token);

	QJsonObject root;
	root["type"] = QStringLiteral("LISTEN");
	root["nonce"] = QString::number(++nonce_);
	root["data"] = data;

	std::string json = QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
	// Redact token in log
	QJsonObject logRoot = root;
	QJsonObject logData = data;
	logData["auth_token"] = QStringLiteral("***");
	logRoot["data"] = logData;
	std::string logJson = QJsonDocument(logRoot).toJson(QJsonDocument::Compact).toStdString();
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending LISTEN (nonce=%d): %s",
	     nonce_, logJson.c_str());
	ws_.send(json);
}

void TwitchPubSubClient::workerMain()
{
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connecting to %s",
	     kPubSubUrl);

	if (!ws_.connect(kPubSubUrl)) {
		blog(LOG_WARNING,
		     "[BitrateSceneSwitch] PubSub: Failed to connect to %s",
		     kPubSubUrl);
		connected_ = false;
		return;
	}

	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connected");
	connected_ = true;
	flushListen();

	QJsonObject initPing;
	initPing["type"] = QStringLiteral("PING");
	ws_.send(QJsonDocument(initPing)
			 .toJson(QJsonDocument::Compact)
			 .toStdString());
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sent initial PING");

	auto lastPing = std::chrono::steady_clock::now();
	int pingCount = 0;
	int pongCount = 0;
	int messageCount = 0;

	while (running_) {
		bool flush = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			flush = resendListen_;
		}
		if (flush && ws_.isConnected())
			flushListen();

		std::string raw;
		auto result = ws_.recv(raw);

		if (result == WsClient::RecvResult::Timeout) {
			auto elapsed =
				std::chrono::duration_cast<
					std::chrono::seconds>(
					std::chrono::steady_clock::now() -
					lastPing)
					.count();
			if (elapsed >= 280) {
				QJsonObject ping;
				ping["type"] = QStringLiteral("PING");
				ping["nonce"] = QString::number(++nonce_);
				std::string pj =
					QJsonDocument(ping)
						.toJson(QJsonDocument::Compact)
						.toStdString();
				pingCount++;
				blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending PING #%d (nonce=%d)",
				     pingCount, nonce_);
				ws_.send(pj);
				lastPing = std::chrono::steady_clock::now();
			}
			continue;
		}

		if (result != WsClient::RecvResult::Message) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Recv result=%d (0=Message,1=Timeout,2=Error) - breaking loop. Stats: pings=%d, pongs=%d, messages=%d",
			     (int)result, pingCount, pongCount, messageCount);
			break;
		}

		messageCount++;
		blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Received message #%d (%zu bytes): %s",
		     messageCount, raw.size(),
		     raw.size() > 500 ? (raw.substr(0, 500) + "...").c_str() : raw.c_str());

		RaidCallback cbCopy;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			cbCopy = raidCb_;
		}

		QJsonParseError err{};
		QJsonDocument doc =
			QJsonDocument::fromJson(QByteArray::fromStdString(raw),
						&err);
		if (err.error != QJsonParseError::NoError || !doc.isObject()) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: JSON parse error: %s",
			     err.errorString().toUtf8().constData());
			continue;
		}
		QJsonObject o = doc.object();
		QString msgType = o.value(QLatin1String("type")).toString();

		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Received type=%s", msgType.toUtf8().constData());

		if (msgType == QLatin1String("RESPONSE")) {
			QString error = o.value(QLatin1String("error")).toString();
			QString nonceStr = o.value(QLatin1String("nonce")).toString();
			if (!error.isEmpty()) {
				blog(LOG_ERROR,
				     "[BitrateSceneSwitch] PubSub: LISTEN error (nonce=%s): %s",
				     nonceStr.toUtf8().constData(),
				     error.toUtf8().constData());
				if (error.contains("ERR_BADAUTH", Qt::CaseInsensitive))
					blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Auth failed - check OAuth token and scopes");
				else if (error.contains("ERR_BADTOPIC", Qt::CaseInsensitive))
					blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Bad topic - check broadcaster ID");
				running_ = false;
				break;
			}
			blog(LOG_INFO,
			     "[BitrateSceneSwitch] PubSub: LISTEN acknowledged (nonce=%s)",
			     nonceStr.toUtf8().constData());
			continue;
		}

		if (msgType == QLatin1String("PONG")) {
			pongCount++;
			blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: PONG received #%d", pongCount);
			continue;
		}

		if (msgType != QLatin1String("MESSAGE"))
			continue;
		QJsonObject data = o.value(QLatin1String("data")).toObject();
		QString topic = data.value(QLatin1String("topic")).toString();
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Message topic: %s", topic.toUtf8().constData());
		
		QString innerStr =
			data.value(QLatin1String("message")).toString();
		if (innerStr.isEmpty())
			continue;

		QJsonDocument innerDoc =
			QJsonDocument::fromJson(innerStr.toUtf8(), &err);
		if (err.error != QJsonParseError::NoError ||
		    !innerDoc.isObject())
			continue;
		QJsonObject innerObj = innerDoc.object();
		QString eventType = innerObj.value(QLatin1String("type")).toString();
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Event type: %s", eventType.toUtf8().constData());
		
		if (innerObj.value(QLatin1String("type")).toString() !=
		    QLatin1String("raid_go_v2"))
			continue;
		QJsonObject raid =
			innerObj.value(QLatin1String("raid")).toObject();
		QString targetLogin =
			raid.value(QLatin1String("target_login")).toString();
		QString display =
			raid.value(QLatin1String("target_display_name"))
				.toString();
		if (targetLogin.isEmpty())
			continue;

		blog(LOG_INFO,
		     "[BitrateSceneSwitch] PubSub: raid_go_v2 detected -> %s (%s)",
		     targetLogin.toUtf8().constData(),
		     display.toUtf8().constData());

		auto now = std::chrono::steady_clock::now();
		if (haveLastRaidEmit_ &&
		    std::chrono::duration_cast<std::chrono::seconds>(
			    now - lastRaidEmit_)
				    .count() < 10) {
			blog(LOG_INFO,
			     "[BitrateSceneSwitch] PubSub: duplicate raid suppressed (10s cooldown)");
			continue;
		}
		haveLastRaidEmit_ = true;
		lastRaidEmit_ = now;

		if (cbCopy)
			queueRaidCallback(std::move(cbCopy),
					  targetLogin.toStdString(),
					  display.toStdString());
	}

	blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Disconnected (stats: pings=%d, pongs=%d, messages=%d)",
	     pingCount, pongCount, messageCount);
	ws_.disconnect();
	connected_ = false;
}

} // namespace BitrateSwitch
