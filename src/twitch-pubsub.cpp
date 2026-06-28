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
	if (running_.exchange(true)) {
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: start() called but already running");
		return;
	}
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
	{
		std::lock_guard<std::mutex> lock(mutex_);
		copy = topics_;
		resendListen_ = false;
	}
	if (copy.empty()) {
		blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: flushListen() called with no topics");
		return;
	}

	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Building LISTEN for %zu topic(s):", copy.size());
	for (const auto &t : copy)
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub:   Topic: %s", t.c_str());

	QJsonArray qtopics;
	for (const auto &t : copy)
		qtopics.append(QString::fromStdString(t));

	QJsonObject data;
	data["topics"] = qtopics;
	data["auth_token"] = QStringLiteral("");

	QJsonObject root;
	root["type"] = QStringLiteral("LISTEN");
	root["nonce"] = QString::number(++nonce_);
	root["data"] = data;

	std::string json =
		QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending LISTEN (nonce=%d, %zu bytes): %s",
	     nonce_, json.size(), json.c_str());
	
	if (!ws_.isConnected()) {
		blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Cannot send LISTEN - WebSocket is not connected!");
		return;
	}
	
	bool sent = ws_.send(json);
	if (!sent)
		blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Failed to send LISTEN!");
	else
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: LISTEN sent successfully");
}

void TwitchPubSubClient::workerMain()
{
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Worker thread started");
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connecting to %s", kPubSubUrl);

	if (!ws_.connect(kPubSubUrl)) {
		blog(LOG_ERROR,
		     "[BitrateSceneSwitch] PubSub: Failed to connect to %s - check network/firewall/DNS",
		     kPubSubUrl);
		connected_ = false;
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Worker exiting (connection failed)");
		return;
	}

	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Connected successfully to %s", kPubSubUrl);
	connected_ = true;
	
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Flushing initial LISTEN");
	flushListen();

	QJsonObject initPing;
	initPing["type"] = QStringLiteral("PING");
	std::string pingJson = QJsonDocument(initPing)
				   .toJson(QJsonDocument::Compact)
				   .toStdString();
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending initial PING: %s", pingJson.c_str());
	ws_.send(pingJson);

	auto lastPing = std::chrono::steady_clock::now();
	int pingCount = 0;
	int pongCount = 0;
	int messageCount = 0;
	int timeoutCount = 0;

	while (running_) {
		bool flush = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			flush = resendListen_;
		}
		if (flush && ws_.isConnected()) {
			blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Resending LISTEN (topics changed)");
			flushListen();
		}

		std::string raw;
		auto result = ws_.recv(raw);

		if (result == WsClient::RecvResult::Timeout) {
			timeoutCount++;
			auto elapsed =
				std::chrono::duration_cast<
					std::chrono::seconds>(
					std::chrono::steady_clock::now() -
					lastPing)
					.count();
			
			blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Recv timeout (count=%d, elapsed since last ping=%llds)",
			     timeoutCount, elapsed);
			
			if (elapsed >= 280) {
				QJsonObject ping;
				ping["type"] = QStringLiteral("PING");
				ping["nonce"] = QString::number(++nonce_);
				std::string pj =
					QJsonDocument(ping)
						.toJson(QJsonDocument::Compact)
						.toStdString();
				pingCount++;
				blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Sending PING #%d (nonce=%d, %llds since last): %s",
				     pingCount, nonce_, elapsed, pj.c_str());
				
				if (!ws_.send(pj)) {
					blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Failed to send PING - connection may be dead");
					break;
				}
				lastPing = std::chrono::steady_clock::now();
			}
			continue;
		}

		if (result == WsClient::RecvResult::Error) {
			blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Receive error - disconnecting (stats: pings=%d, pongs=%d, messages=%d, timeouts=%d)",
			     pingCount, pongCount, messageCount, timeoutCount);
			break;
		}

		if (result == WsClient::RecvResult::Disconnected) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Server closed connection (stats: pings=%d, pongs=%d, messages=%d, timeouts=%d)",
			     pingCount, pongCount, messageCount, timeoutCount);
			break;
		}

		if (result != WsClient::RecvResult::Message) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Unexpected recv result: %d", (int)result);
			break;
		}

		// Reset timeout counter on successful receive
		timeoutCount = 0;
		messageCount++;
		
		blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Received raw message #%d (%zu bytes): %s",
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
		if (err.error != QJsonParseError::NoError) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Failed to parse JSON: %s (offset=%d)",
			     err.errorString().toUtf8().constData(), err.offset);
			continue;
		}
		
		if (!doc.isObject()) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Received non-object JSON");
			continue;
		}
		
		QJsonObject o = doc.object();
		QString msgType = o.value(QLatin1String("type")).toString();
		
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Received message type: %s", msgType.toUtf8().constData());

		if (msgType == QLatin1String("RESPONSE")) {
			QString error = o.value(QLatin1String("error")).toString();
			QString nonceStr = o.value(QLatin1String("nonce")).toString();
			
			if (!error.isEmpty()) {
				blog(LOG_ERROR,
				     "[BitrateSceneSwitch] PubSub: LISTEN error (nonce=%s): %s",
				     nonceStr.toUtf8().constData(),
				     error.toUtf8().constData());
				
				// Provide more context for common errors
				if (error.contains("ERR_BADAUTH", Qt::CaseInsensitive)) {
					blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Authentication failed! Check your OAuth token and scopes.");
				} else if (error.contains("ERR_BADTOPIC", Qt::CaseInsensitive)) {
					blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Bad topic! Check your broadcaster ID.");
				} else if (error.contains("ERR_SERVER", Qt::CaseInsensitive)) {
					blog(LOG_ERROR, "[BitrateSceneSwitch] PubSub: Server error! Twitch may be experiencing issues.");
				}
				
				blog(LOG_WARNING,
				     "[BitrateSceneSwitch] PubSub: Giving up after LISTEN error (fix config and reconnect)");
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
			blog(LOG_INFO,
			     "[BitrateSceneSwitch] PubSub: PONG received #%d", pongCount);
			continue;
		}

		if (msgType == QLatin1String("RECONNECT")) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Server requested RECONNECT");
			break;
		}

		if (msgType != QLatin1String("MESSAGE")) {
			blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Ignoring message type: %s",
			     msgType.toUtf8().constData());
			continue;
		}
		
		QJsonObject data = o.value(QLatin1String("data")).toObject();
		QString topic = data.value(QLatin1String("topic")).toString();
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Message topic: %s", topic.toUtf8().constData());
		
		QString innerStr =
			data.value(QLatin1String("message")).toString();
		if (innerStr.isEmpty()) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Empty message content");
			continue;
		}

		blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Inner message: %s", innerStr.toUtf8().constData());

		QJsonDocument innerDoc =
			QJsonDocument::fromJson(innerStr.toUtf8(), &err);
		if (err.error != QJsonParseError::NoError) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Failed to parse inner JSON: %s",
			     err.errorString().toUtf8().constData());
			continue;
		}
		
		if (!innerDoc.isObject()) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Inner message is not an object");
			continue;
		}
		
		QJsonObject innerObj = innerDoc.object();
		QString eventType = innerObj.value(QLatin1String("type")).toString();
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Event type: %s", eventType.toUtf8().constData());
		
		if (innerObj.value(QLatin1String("type")).toString() !=
		    QLatin1String("raid_go_v2")) {
			blog(LOG_DEBUG, "[BitrateSceneSwitch] PubSub: Not a raid event, skipping");
			continue;
		}
		
		QJsonObject raid =
			innerObj.value(QLatin1String("raid")).toObject();
		QString targetLogin =
			raid.value(QLatin1String("target_login")).toString();
		QString display =
			raid.value(QLatin1String("target_display_name"))
				.toString();
		if (targetLogin.isEmpty()) {
			blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Empty target_login in raid event");
			continue;
		}

		blog(LOG_INFO,
		     "[BitrateSceneSwitch] PubSub: *** RAID DETECTED *** -> %s (%s)",
		     targetLogin.toUtf8().constData(),
		     display.toUtf8().constData());

		auto now = std::chrono::steady_clock::now();
		if (haveLastRaidEmit_ &&
		    std::chrono::duration_cast<std::chrono::seconds>(
			    now - lastRaidEmit_)
				    .count() < 10) {
			blog(LOG_INFO,
			     "[BitrateSceneSwitch] PubSub: Duplicate raid suppressed (10s cooldown)");
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

	blog(LOG_WARNING, "[BitrateSceneSwitch] PubSub: Exiting worker loop (running_=%d, connected_=%d, stats: pings=%d, pongs=%d, messages=%d, timeouts=%d)",
	     (int)running_, (int)connected_, pingCount, pongCount, messageCount, timeoutCount);
	
	// Log WHY we exited
	if (!running_)
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Exit reason: stop() was called");
	else if (!ws_.isConnected())
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Exit reason: WebSocket disconnected");
	else
		blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Exit reason: unknown");
	
	ws_.disconnect();
	connected_ = false;
	blog(LOG_INFO, "[BitrateSceneSwitch] PubSub: Worker thread finished");
}

} // namespace BitrateSwitch
