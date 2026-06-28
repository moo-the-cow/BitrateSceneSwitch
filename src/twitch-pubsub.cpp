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

void TwitchPubSubClient::setAuthToken(const std::string &token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	authToken_ = token;
	// If we're connected, resend LISTEN with new token
	if (connected_) {
		resendListen_ = true;
	}
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
	worker_ = std::thread([this]() { workerMain(); });
}

void TwitchPubSubClient::stop()
{
	running_ = false;
	if (worker_.joinable())
		worker_.join();
	ws_.disconnect();
	connected_ = false;
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

	// Validate token
	if (token.empty()) {
		blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Cannot LISTEN - no auth token set");
		return;
	}

	// Remove "oauth:" prefix if present (PubSub expects raw token)
	if (token.find("oauth:") == 0) {
		token = token.substr(6);
	}

	QJsonArray qtopics;
	for (const auto &t : copy)
		qtopics.append(QString::fromStdString(t));

	QJsonObject data;
	data["topics"] = qtopics;
	data["auth_token"] = QString::fromStdString(token); // USE THE ACTUAL TOKEN

	QJsonObject root;
	root["type"] = QStringLiteral("LISTEN");
	root["nonce"] = QString::number(++nonce_);
	root["data"] = data;

	std::string json =
		QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
	
	// Don't log the actual token for security
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: sending LISTEN for %zu topic(s) with auth token",
	     copy.size());
	
	if (ws_.isConnected()) {
		if (ws_.send(json)) {
			blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: LISTEN sent successfully");
		} else {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Failed to send LISTEN");
		}
	} else {
		blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Cannot send LISTEN - not connected");
	}
}

void TwitchPubSubClient::workerMain()
{
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connecting to %s",
	     kPubSubUrl);

	if (!ws_.connect(kPubSubUrl)) {
		blog(LOG_WARNING,
		     "[BitrateSceneSwitch] PubSub: failed to connect");
		connected_ = false;
		return;
	}

	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connected");
	connected_ = true;

	// Wait a moment for connection to stabilize, then flush LISTEN
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	flushListen();

	// Send initial PING
	QJsonObject initPing;
	initPing["type"] = QStringLiteral("PING");
	std::string pingJson = QJsonDocument(initPing)
				   .toJson(QJsonDocument::Compact)
				   .toStdString();
	ws_.send(pingJson);

	auto lastPing = std::chrono::steady_clock::now();

	while (running_) {
		// Check if we need to resend LISTEN
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
				std::string pj =
					QJsonDocument(ping)
						.toJson(QJsonDocument::Compact)
						.toStdString();
				ws_.send(pj);
				lastPing = std::chrono::steady_clock::now();
				blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: PING sent");
			}
			continue;
		}

		if (result != WsClient::RecvResult::Message)
			break;

		QJsonParseError err{};
		QJsonDocument doc =
			QJsonDocument::fromJson(QByteArray::fromStdString(raw),
						&err);
		if (err.error != QJsonParseError::NoError || !doc.isObject())
			continue;
			
		QJsonObject o = doc.object();
		QString msgType = o.value(QLatin1String("type")).toString();

		// Handle PONG
		if (msgType == QLatin1String("PONG")) {
			blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: PONG received");
			continue;
		}

		// Handle RECONNECT message from server
		if (msgType == QLatin1String("RECONNECT")) {
			blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Server requested reconnect");
			break;
		}

		// Handle RESPONSE (LISTEN acknowledgment)
		if (msgType == QLatin1String("RESPONSE")) {
			QString error = o.value(QLatin1String("error")).toString();
			if (!error.isEmpty()) {
				blog(LOG_WARNING,
				     "[BitrateSceneSwitch] PubSub LISTEN error: %s",
				     error.toUtf8().constData());
				
				// Check for authentication errors
				if (error.contains("ERR_BADAUTH", Qt::CaseInsensitive)) {
					blog(LOG_WARNING,
					     "[BitrateSceneSwitch] PubSub: Authentication failed! Check your OAuth token. "
					     "Make sure it has the 'channel:read:redemptions' scope and is valid.");
				} else if (error.contains("ERR_BADTOPIC", Qt::CaseInsensitive)) {
					blog(LOG_WARNING,
					     "[BitrateSceneSwitch] PubSub: Bad topic. Make sure the broadcaster ID is correct.");
				}
				
				// Don't break on error - let the caller decide whether to reconnect
				continue;
			}
			blog(LOG_INFO,
			     "[BitrateSceneSwitch] PubSub LISTEN acknowledged successfully");
			continue;
		}

		// Handle MESSAGE (actual events)
		if (msgType != QLatin1String("MESSAGE"))
			continue;

		RaidCallback cbCopy;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			cbCopy = raidCb_;
		}

		QJsonObject data = o.value(QLatin1String("data")).toObject();
		
		// The message can be either a string or a JSON object
		QString innerStr;
		if (data.contains(QLatin1String("message"))) {
			QJsonValue msgVal = data.value(QLatin1String("message"));
			if (msgVal.isString()) {
				innerStr = msgVal.toString();
			} else if (msgVal.isObject()) {
				QJsonObject msgObj = msgVal.toObject();
				innerStr = QJsonDocument(msgObj).toJson(QJsonDocument::Compact);
			}
		}
		
		if (innerStr.isEmpty()) {
			// Try parsing the data directly
			QString dataTopic = data.value(QLatin1String("topic")).toString();
			if (dataTopic.startsWith("raid.")) {
				// The data itself might contain the raid info
				innerStr = QJsonDocument(data).toJson(QJsonDocument::Compact);
			} else {
				continue;
			}
		}

		QJsonDocument innerDoc =
			QJsonDocument::fromJson(innerStr.toUtf8(), &err);
		if (err.error != QJsonParseError::NoError || !innerDoc.isObject())
			continue;
			
		QJsonObject innerObj = innerDoc.object();
		QString eventType = innerObj.value(QLatin1String("type")).toString();
		
		if (eventType != QLatin1String("raid_go_v2") && 
		    eventType != QLatin1String("raid_go"))
			continue;
			
		QJsonObject raid = innerObj.value(QLatin1String("raid")).toObject();
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

	blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Disconnected");
	ws_.disconnect();
	connected_ = false;
}

} // namespace BitrateSwitch
