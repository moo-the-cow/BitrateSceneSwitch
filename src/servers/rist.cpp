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
    
    if (!parser.navigateTo("receiver-stats")) {
        blog(LOG_WARNING, "[RistServer] 'receiver-stats' not found for %s", name_.c_str());
        return info;
    }
    parser.enterObject();
    
    if (!parser.navigateTo("flowinstant")) {
        blog(LOG_WARNING, "[RistServer] 'flowinstant' not found for %s", name_.c_str());
        return info;
    }
    parser.enterObject();
    
    if (!parser.navigateTo("peers")) {
        blog(LOG_WARNING, "[RistServer] 'peers' array not found for %s", name_.c_str());
        return info;
    }

    int64_t totalBitrate = 0;
    double totalRtt = 0.0;
    int activePeers = 0;

    parser.forEachInArray([&](JsonUtils::Parser& peerParser) {
        if (!peerParser.enterObject()) return;
        
        int dead = 0;
        if (peerParser.navigateTo("dead")) {
            dead = peerParser.getInt64(0);
        }
        if (dead != 0) return;
        
        if (!peerParser.navigateTo("stats")) return;
        
        std::string statsStr = peerParser.extractObjectString();
        if (statsStr.empty()) return;
        
        JsonUtils::Parser statsParser(statsStr);
        statsParser.enterObject();
        
        int64_t bitrate = 0;
        if (statsParser.navigateTo("bitrate")) {
            bitrate = statsParser.getInt64(0);
            totalBitrate += bitrate;
        }
        
        JsonUtils::Parser rttParser(statsStr);
        rttParser.enterObject();
        
        double rtt = 0.0;
        if (rttParser.navigateTo("rtt")) {
            rtt = rttParser.getDouble(0.0);
            if (rtt > 0.0) {
                totalRtt += rtt;
            }
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