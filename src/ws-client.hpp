#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>   // <-- ADDED
#include <curl/curl.h>

namespace BitrateSwitch {

class WsClient {
public:
    enum class RecvResult {
        Message,
        Timeout,
        Error,
        Disconnected
    };

    WsClient();
    ~WsClient();
    
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;
    
    bool connect(const std::string &url);
    void disconnect();
    bool send(const std::string &data);
    RecvResult recv(std::string &out, int timeoutSeconds = 10);
    bool isConnected() const;

private:
    static size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata);
    static int debugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp);
    
    CURL *easy_ = nullptr;
    std::atomic<bool> connected_{false};
    std::mutex sendMutex_;
    std::mutex recvMutex_;
    
    struct RecvBuffer {
        std::string data;
        std::mutex mutex;
        std::condition_variable cv;
        bool hasData = false;
    };
    RecvBuffer recvBuffer_;
    
    std::string lastError_;
    std::string url_;
};

} // namespace BitrateSwitch
