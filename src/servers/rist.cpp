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

    blog(LOG_DEBUG, "[RistServer] Raw JSON response: %s", response.body.c_str());

    JsonUtils::Parser parser(response.body);
    
    if (!parser.navigateTo("receiver-stats")) {
        blog(LOG_WARNING, "[RistServer] 'receiver-stats' not found for %s", name_.c_str());
        return info;
    }
    blog(LOG_DEBUG, "[RistServer] Found 'receiver-stats'");
    
    if (!parser.enterObject()) {
        blog(LOG_WARNING, "[RistServer] Failed to enter 'receiver-stats' object");
        return info;
    }
    blog(LOG_DEBUG, "[RistServer] Entered 'receiver-stats' object");
    
    if (!parser.navigateTo("flowinstant")) {
        blog(LOG_WARNING, "[RistServer] 'flowinstant' not found for %s", name_.c_str());
        return info;
    }
    blog(LOG_DEBUG, "[RistServer] Found 'flowinstant'");
    
    if (!parser.enterObject()) {
        blog(LOG_WARNING, "[RistServer] Failed to enter 'flowinstant' object");
        return info;
    }
    blog(LOG_DEBUG, "[RistServer] Entered 'flowinstant' object");
    
    if (!parser.navigateTo("peers")) {
        blog(LOG_WARNING, "[RistServer] 'peers' array not found for %s", name_.c_str());
        return info;
    }
    blog(LOG_DEBUG, "[RistServer] Found 'peers' array");

    int64_t totalBitrate = 0;
    double totalRtt = 0.0;
    int activePeers = 0;

    parser.forEachInArray([&](JsonUtils::Parser& peerParser) {
        blog(LOG_DEBUG, "[RistServer] Processing peer element...");
        
        if (!peerParser.enterObject()) {
            blog(LOG_DEBUG, "[RistServer] Failed to enter peer object");
            return;
        }
        blog(LOG_DEBUG, "[RistServer] Entered peer object");
        
        int dead = 0;
        if (peerParser.navigateTo("dead")) {
            dead = peerParser.getInt64(0);
            blog(LOG_DEBUG, "[RistServer] Peer dead flag: %d", dead);
        } else {
            blog(LOG_DEBUG, "[RistServer] 'dead' key not found in peer");
        }
        if (dead != 0) {
            blog(LOG_DEBUG, "[RistServer] Skipping dead peer");
            return;
        }
        
        if (!peerParser.navigateTo("stats")) {
            blog(LOG_DEBUG, "[RistServer] 'stats' key not found in peer");
            return;
        }
        blog(LOG_DEBUG, "[RistServer] Found 'stats' key in peer");
        
        std::string statsStr = peerParser.extractObjectString();
        if (statsStr.empty()) {
            blog(LOG_DEBUG, "[RistServer] Failed to extract stats object string");
            return;
        }
        blog(LOG_DEBUG, "[RistServer] Extracted stats object: %s", statsStr.c_str());
        
        // Parse bitrate
        JsonUtils::Parser statsParser(statsStr);
        if (!statsParser.enterObject()) {
            blog(LOG_DEBUG, "[RistServer] Failed to enter stats object");
            return;
        }
        
        int64_t bitrate = 0;
        if (statsParser.navigateTo("bitrate")) {
            bitrate = statsParser.getInt64(0);
            blog(LOG_DEBUG, "[RistServer] Peer bitrate: %lld", bitrate);
            totalBitrate += bitrate;
        } else {
            blog(LOG_DEBUG, "[RistServer] 'bitrate' not found in stats");
        }
        
        // Parse RTT using a fresh parser
        JsonUtils::Parser rttParser(statsStr);
        if (!rttParser.enterObject()) {
            blog(LOG_DEBUG, "[RistServer] Failed to enter stats object for RTT");
            return;
        }
        
        double rtt = 0.0;
        if (rttParser.navigateTo("rtt")) {
            rtt = rttParser.getDouble(0.0);
            blog(LOG_DEBUG, "[RistServer] Peer RTT: %f", rtt);
            if (rtt > 0.0) {
                totalRtt += rtt;
            }
        } else {
            blog(LOG_DEBUG, "[RistServer] 'rtt' not found in stats");
        }
        
        activePeers++;
    });

    blog(LOG_DEBUG, "[RistServer] Active peers counted: %d", activePeers);

    if (activePeers == 0) {
        blog(LOG_INFO, "[RistServer] No active peers for %s", name_.c_str());
        return info;
    }

    info.bitrateKbps = totalBitrate / 1024;
    info.rttMs = totalRtt / activePeers;
    info.isOnline = info.bitrateKbps > 0;
    
    blog(LOG_DEBUG, "[RistServer] %s: %lld kbps, %.1f ms RTT, %d peers", 
         name_.c_str(), info.bitrateKbps, info.rttMs, activePeers);

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