#include "rist.hpp"
#include "../utils/json_utils.hpp"
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
    
    // Navigate to peers array
    if (!parser.navigateTo({"receiver-stats", "flowinstant", "peers"})) {
        blog(LOG_WARNING, "[RistServer] Failed to find peers array for %s", name_.c_str());
        return info;
    }

    int64_t totalBitrate = 0;
    double totalRtt = 0.0;
    int peerCount = 0;

    parser.forEachInArray([&](JsonUtils::Parser& peerParser) {
        // Check if peer is dead
        if (peerParser.navigateTo("dead")) {
            int dead = peerParser.getInt64(0);
            if (dead != 0) {
                return; // Skip dead peers
            }
        }
        
        // Navigate to peer's stats
        if (!peerParser.navigateTo("stats")) {
            return;
        }
        
        // Get bitrate
        if (peerParser.navigateTo("bitrate")) {
            int64_t bitrate = peerParser.getInt64(0);
            totalBitrate += bitrate;
        }
        
        // Get RTT
        peerParser.reset(); // Go back to start of stats object
        if (peerParser.navigateTo("rtt")) {
            double rtt = peerParser.getDouble(0.0);
            if (rtt > 0.0) {
                totalRtt += rtt;
            }
        }
        
        peerCount++;
    });

    if (peerCount == 0) {
        blog(LOG_INFO, "[RistServer] No active peers for %s", name_.c_str());
        return info;
    }

    // Convert bitrate from bits/s to kbps
    info.bitrateKbps = totalBitrate / 1024;
    info.rttMs = (totalRtt > 0) ? (totalRtt / peerCount) : 0.0;
    info.isOnline = info.bitrateKbps > 0;
    
    blog(LOG_DEBUG, "[RistServer] %s: %lld kbps, %.1f ms RTT, %d peers", 
         name_.c_str(), info.bitrateKbps, info.rttMs, peerCount);

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
    if (!info.isOnline) {
        return "Offline";
    }

    std::ostringstream ss;
    ss << info.bitrateKbps << " Kbps, " 
       << static_cast<int>(std::round(info.rttMs)) << " ms";
    return ss.str();
}

} // namespace BitrateSwitch