#include "ws-client.hpp"
#include <obs-module.h>
#include <algorithm>
#include <cstring>

namespace BitrateSwitch {

size_t WsClient::writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buffer = static_cast<RecvBuffer*>(userdata);
    size_t totalSize = size * nmemb;
    if (totalSize > 0) {
        std::lock_guard<std::mutex> lock(buffer->mutex);
        buffer->data.append(ptr, totalSize);
        buffer->hasData = true;
        buffer->cv.notify_one();
    }
    return totalSize;
}

int WsClient::debugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp)
{
    (void)handle;
    (void)userp;
    if (type == CURLINFO_TEXT) {
        std::string text(data, size);
        text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        if (!text.empty())
            blog(LOG_DEBUG, "[BitrateSceneSwitch] WebSocket: %s", text.c_str());
    } else if (type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT) {
        std::string text(data, size);
        text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        if (!text.empty()) {
            const char *direction = (type == CURLINFO_HEADER_IN) ? "RECV" : "SEND";
            blog(LOG_DEBUG, "[BitrateSceneSwitch] WebSocket %s: %s", direction, text.c_str());
        }
    }
    return 0;
}

WsClient::WsClient()
{
    easy_ = curl_easy_init();
    if (!easy_)
        blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: Failed to initialize curl");
}

WsClient::~WsClient()
{
    disconnect();
    if (easy_) {
        curl_easy_cleanup(easy_);
        easy_ = nullptr;
    }
}

bool WsClient::connect(const std::string &url)
{
    if (!easy_) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: No curl handle");
        return false;
    }
    
    url_ = url;
    blog(LOG_INFO, "[BitrateSceneSwitch] WebSocket: Connecting to %s", url.c_str());
    
    curl_easy_reset(easy_);
    curl_easy_setopt(easy_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy_, CURLOPT_WS_OPTIONS, CURLWS_RAW_MODE);
    curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(easy_, CURLOPT_PROTOCOLS, CURLPROTO_WS | CURLPROTO_WSS);
    curl_easy_setopt(easy_, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_WS | CURLPROTO_WSS);
    curl_easy_setopt(easy_, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(easy_, CURLOPT_DEBUGFUNCTION, debugCallback);
    curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(easy_, CURLOPT_WRITEDATA, &recvBuffer_);
    curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy_, CURLOPT_USERAGENT, "OBS-Studio/31.0.0");
    curl_easy_setopt(easy_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy_, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(easy_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy_, CURLOPT_SSL_VERIFYHOST, 2L);
    
    CURLcode res = curl_easy_perform(easy_);
    if (res != CURLE_OK) {
        lastError_ = curl_easy_strerror(res);
        blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: Connection failed: %s (code: %d)", 
             lastError_.c_str(), (int)res);
        long httpCode = 0;
        curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &httpCode);
        if (httpCode > 0)
            blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: HTTP response code: %ld", httpCode);
        char *effectiveUrl = nullptr;
        curl_easy_getinfo(easy_, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (effectiveUrl)
            blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: Effective URL: %s", effectiveUrl);
        connected_ = false;
        return false;
    }
    
    long wsStatus = 0;
    curl_easy_getinfo(easy_, CURLINFO_WS_STATUS, &wsStatus);
    if (wsStatus != 101) {
        blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: Upgrade failed, status: %ld (expected 101)", wsStatus);
        connected_ = false;
        return false;
    }
    
    blog(LOG_INFO, "[BitrateSceneSwitch] WebSocket: Connected successfully (status: %ld)", wsStatus);
    connected_ = true;
    return true;
}

void WsClient::disconnect()
{
    if (!connected_) return;
    blog(LOG_INFO, "[BitrateSceneSwitch] WebSocket: Disconnecting...");
    if (easy_) {
        size_t sent;
        curl_ws_send(easy_, "", 0, &sent, 0, CURLWS_CLOSE);
    }
    connected_ = false;
    std::lock_guard<std::mutex> lock(recvBuffer_.mutex);
    recvBuffer_.data.clear();
    recvBuffer_.hasData = false;
    blog(LOG_INFO, "[BitrateSceneSwitch] WebSocket: Disconnected");
}

bool WsClient::send(const std::string &data)
{
    if (!connected_ || !easy_) {
        blog(LOG_WARNING, "[BitrateSceneSwitch] WebSocket: Cannot send - not connected");
        return false;
    }
    std::lock_guard<std::mutex> lock(sendMutex_);
    size_t sent = 0;
    CURLcode res = curl_ws_send(easy_, data.c_str(), data.size(), &sent, 0, CURLWS_TEXT);
    if (res != CURLE_OK) {
        lastError_ = curl_easy_strerror(res);
        blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: Send failed: %s (code: %d)", 
             lastError_.c_str(), (int)res);
        if (res == CURLE_SEND_ERROR) {
            blog(LOG_ERROR, "[BitrateSceneSwitch] WebSocket: Connection appears to be dead");
            connected_ = false;
        }
        return false;
    }
    blog(LOG_DEBUG, "[BitrateSceneSwitch] WebSocket: Sent %zu bytes", sent);
    return true;
}

WsClient::RecvResult WsClient::recv(std::string &out, int timeoutSeconds)
{
    if (!connected_ || !easy_) {
        blog(LOG_DEBUG, "[BitrateSceneSwitch] WebSocket: recv called but not connected");
        return RecvResult::Closed;
    }
    
    std::lock_guard<std::mutex> lock(recvMutex_);
    {
        std::unique_lock<std::mutex> waitLock(recvBuffer_.mutex);
        if (!recvBuffer_.hasData) {
            auto timeout = std::chrono::seconds(timeoutSeconds);
            if (recvBuffer_.cv.wait_for(waitLock, timeout) == std::cv_status::timeout) {
                if (!connected_)
                    return RecvResult::Closed;
                return RecvResult::Timeout;
            }
        }
        if (!recvBuffer_.hasData) {
            if (!connected_)
                return RecvResult::Closed;
            return RecvResult::Timeout;
        }
        out = std::move(recvBuffer_.data);
        recvBuffer_.data.clear();
        recvBuffer_.hasData = false;
    }
    
    if (out.empty()) {
        const struct curl_ws_frame *meta = curl_ws_meta(easy_);
        if (meta) {
            blog(LOG_INFO, "[BitrateSceneSwitch] WebSocket: Received frame - age: %d, flags: %d, offset: %d, bytesleft: %d",
                 meta->age, meta->flags, meta->offset, meta->bytesleft);
            if (meta->flags & CURLWS_CLOSE) {
                blog(LOG_INFO, "[BitrateSceneSwitch] WebSocket: Received close frame");
                connected_ = false;
                return RecvResult::Closed;
            }
        }
    }
    
    blog(LOG_DEBUG, "[BitrateSceneSwitch] WebSocket: Received %zu bytes", out.size());
    return RecvResult::Message;
}

bool WsClient::isConnected() const
{
    return connected_;
}

} // namespace BitrateSwitch
