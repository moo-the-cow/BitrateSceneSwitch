#include "rist.hpp"
#include "../utils/jsonutils.hpp"
#include "../switcher.hpp"
#include <cmath>
#include <sstream>
#include <obs-module.h>

namespace BitrateSwitch {

RistServer::RistServer(const StreamServerConfig &config)
{
    statsUrl_ = config.statsUrl;
    name_ = config.name;
    overrideScenes_ = config.overrideScenes;
}

BitrateInfo RistServer::fetchStats()
{
    BitrateInfo info;
    info.serverName = name_;

    HttpResponse response = httpClient_.get(statsUrl_);
    if (!response.success) {
        blog(LOG_WARNING, "[RistServer] HTTP request failed for %s", name_.c_str());
        return info;
    }

    JsonUtils::Parser parser(response.body);
    
    // Navigate directly using the path
    if (!parser.navigateTo({"receiver-stats", "flowinstant", "peers"})) {
        blog(LOG_WARNING, "[RistServer] Failed to navigate to peers array for %s", name_.c_str());
        return info;
    }

    int64_t totalBitrate = 0;
    double totalRtt = 0.0;
    int activePeers = 0;

    parser.forEachInArray([&](JsonUtils::Parser& peerParser) {
        // Check dead flag
        if (peerParser.navigateTo("dead")) {
            int dead = peerParser.getInt64(0);
            if (dead != 0) return;
        }
        
        // Navigate to stats
        if (!peerParser.navigateTo("stats")) return;
        
        std::string statsStr = peerParser.extractObjectString();
        if (statsStr.empty()) return;
        
        // Parse bitrate
        JsonUtils::Parser statsParser(statsStr);
        if (statsParser.navigateTo("bitrate")) {
            totalBitrate += statsParser.getInt64(0);
        }
        
        // Parse RTT
        JsonUtils::Parser rttParser(statsStr);
        if (rttParser.navigateTo("rtt")) {
            double rtt = rttParser.getDouble(0.0);
            if (rtt > 0.0) totalRtt += rtt;
        }
        
        activePeers++;
    });

    if (activePeers == 0) {
        blog(LOG_INFO, "[RistServer] No active peers for %s", name_.c_str());
        return info;
    }

    info.bitrateKbps = totalBitrate / 1024;
    info.rttMs = totalRtt / activePeers;
    info.isOnline = info.bitrateKbps > 0;

    return info;
}

SwitchType RistServer::checkSwitch(const Triggers &triggers)
{
    BitrateInfo info = fetchStats();
    return evaluateTriggers(info, triggers);
}

BitrateInfo RistServer::getBitrate()
{
    BitrateInfo info = fetchStats();
    if (info.bitrateKbps > 0) {
        std::ostringstream ss;
        ss << info.bitrateKbps << " kbps, " 
           << static_cast<int>(std::round(info.rttMs)) << " ms";
        info.message = ss.str();
    }
    return info;
}

std::string RistServer::getSourceInfo()
{
    BitrateInfo info = fetchStats();
    if (!info.isOnline) return "Offline";

    std::ostringstream ss;
    ss << info.bitrateKbps << " Kbps, " 
       << static_cast<int>(std::round(info.rttMs)) << " ms";
    return ss.str();
}

} // namespace BitrateSwitch