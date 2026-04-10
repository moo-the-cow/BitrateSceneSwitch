#pragma once

#include <string>
#include <functional>
#include <vector>
#include <cctype>
#include <stdexcept>

namespace JsonUtils {

class Parser {
private:
    const std::string& json_;
    size_t pos_;

    void skipWhitespace() {
        while (pos_ < json_.length() && std::isspace(json_[pos_])) {
            pos_++;
        }
    }

    bool expect(char c) {
        skipWhitespace();
        if (pos_ < json_.length() && json_[pos_] == c) {
            pos_++;
            return true;
        }
        return false;
    }

    bool findKeyInCurrentObject(const std::string& key) {
        std::string searchKey = "\"" + key + "\"";
        
        int braceDepth = 0;
        bool inString = false;
        bool escaped = false;
        
        for (size_t i = pos_; i < json_.length(); i++) {
            char c = json_[i];
            
            if (escaped) {
                escaped = false;
                continue;
            }
            
            if (c == '\\') {
                escaped = true;
                continue;
            }
            
            if (c == '"' && !escaped) {
                inString = !inString;
                continue;
            }
            
            if (inString) continue;
            
            if (c == '{') braceDepth++;
            if (c == '}') {
                braceDepth--;
                if (braceDepth < 0) {
                    // We've exited the current object without finding the key
                    return false;
                }
            }
            
            // Only look for keys at the current object level (braceDepth == 0)
            if (braceDepth == 0 && json_.substr(i, searchKey.length()) == searchKey) {
                // Verify it's followed by colon with optional whitespace
                size_t colonPos = i + searchKey.length();
                while (colonPos < json_.length() && std::isspace(json_[colonPos])) {
                    colonPos++;
                }
                if (colonPos < json_.length() && json_[colonPos] == ':') {
                    pos_ = colonPos + 1;
                    return true;
                }
            }
        }
        
        return false;
    }

public:
    explicit Parser(const std::string& json) : json_(json), pos_(0) {}
    
    void reset() {
        pos_ = 0;
    }
    
    size_t position() const {
        return pos_;
    }
    
    bool navigateTo(const std::string& key) {
        skipWhitespace();
        return findKeyInCurrentObject(key);
    }
    
    bool navigateTo(const std::vector<std::string>& path) {
        size_t savedPos = pos_;
        
        for (size_t i = 0; i < path.size(); i++) {
            if (i > 0) {
                if (!enterObject()) {
                    pos_ = savedPos;
                    return false;
                }
            }
            if (!findKeyInCurrentObject(path[i])) {
                pos_ = savedPos;
                return false;
            }
        }
        return true;
    }
    
    bool enterObject() {
        skipWhitespace();
        if (pos_ >= json_.length() || json_[pos_] != '{') {
            return false;
        }
        pos_++; // Skip the '{'
        return true;
    }
    
    bool enterArray() {
        skipWhitespace();
        return expect('[');
    }
    
    void forEachInArray(const std::function<void(Parser&)>& callback) {
        size_t savedPos = pos_;
        
        if (!enterArray()) {
            return;
        }
        
        int depth = 1;
        bool inString = false;
        bool escaped = false;
        size_t elementStart = pos_;
        
        for (size_t i = pos_; i < json_.length() && depth > 0; i++) {
            char c = json_[i];
            
            if (escaped) {
                escaped = false;
                continue;
            }
            
            if (c == '\\') {
                escaped = true;
                continue;
            }
            
            if (c == '"' && !escaped) {
                inString = !inString;
                continue;
            }
            
            if (inString) continue;
            
            if (c == '[') depth++;
            if (c == ']') {
                depth--;
                if (depth == 0) {
                    if (elementStart < i) {
                        size_t len = i - elementStart;
                        while (len > 0 && (json_[elementStart + len - 1] == ',' || 
                                          std::isspace(json_[elementStart + len - 1]))) {
                            len--;
                        }
                        if (len > 0) {
                            std::string elementStr = json_.substr(elementStart, len);
                            Parser elementParser(elementStr);
                            callback(elementParser);
                        }
                    }
                    break;
                }
            }
            
            if (depth == 1 && c == ',') {
                size_t len = i - elementStart;
                while (len > 0 && (json_[elementStart + len - 1] == ',' || 
                                  std::isspace(json_[elementStart + len - 1]))) {
                    len--;
                }
                if (len > 0) {
                    std::string elementStr = json_.substr(elementStart, len);
                    Parser elementParser(elementStr);
                    callback(elementParser);
                }
                elementStart = i + 1;
            }
        }
        
        pos_ = savedPos;
    }
    
    std::string getString() {
        skipWhitespace();
        if (!expect('"')) return "";
        
        std::string result;
        bool escaped = false;
        
        while (pos_ < json_.length()) {
            char c = json_[pos_++];
            if (escaped) {
                result += c;
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return result;
            } else {
                result += c;
            }
        }
        return result;
    }
    
    std::string getNumberString() {
        skipWhitespace();
        std::string result;
        
        while (pos_ < json_.length()) {
            char c = json_[pos_];
            if (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || 
                (c >= '0' && c <= '9')) {
                result += c;
                pos_++;
            } else {
                break;
            }
        }
        return result;
    }
    
    std::string getValueAsString() {
        skipWhitespace();
        if (pos_ >= json_.length()) return "";
        
        if (json_[pos_] == '"') {
            return getString();
        } else if (json_[pos_] == '-' || (json_[pos_] >= '0' && json_[pos_] <= '9')) {
            return getNumberString();
        } else {
            std::string result;
            while (pos_ < json_.length() && std::isalpha(json_[pos_])) {
                result += json_[pos_++];
            }
            return result;
        }
    }
    
    int64_t getInt64(int64_t defaultValue = 0) {
        std::string str = getValueAsString();
        if (str.empty() || str == "null") return defaultValue;
        
        try {
            return std::stoll(str);
        } catch (...) {
            return defaultValue;
        }
    }
    
    double getDouble(double defaultValue = 0.0) {
        std::string str = getValueAsString();
        if (str.empty() || str == "null") return defaultValue;
        
        try {
            return std::stod(str);
        } catch (...) {
            return defaultValue;
        }
    }
    
    bool getBool(bool defaultValue = false) {
        std::string str = getValueAsString();
        if (str == "true") return true;
        if (str == "false") return false;
        return defaultValue;
    }
    
    bool isNull() {
        skipWhitespace();
        if (pos_ + 4 <= json_.length() && json_.substr(pos_, 4) == "null") {
            return true;
        }
        return false;
    }
    
    void skipValue() {
        skipWhitespace();
        if (pos_ >= json_.length()) return;
        
        char c = json_[pos_];
        
        if (c == '"') {
            getString();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            getNumberString();
        } else if (c == '{') {
            int depth = 1;
            bool inString = false;
            bool escaped = false;
            pos_++;
            while (pos_ < json_.length() && depth > 0) {
                char ch = json_[pos_++];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    continue;
                }
                if (ch == '"' && !escaped) {
                    inString = !inString;
                    continue;
                }
                if (inString) continue;
                if (ch == '{') depth++;
                if (ch == '}') depth--;
            }
        } else if (c == '[') {
            int depth = 1;
            bool inString = false;
            bool escaped = false;
            pos_++;
            while (pos_ < json_.length() && depth > 0) {
                char ch = json_[pos_++];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    continue;
                }
                if (ch == '"' && !escaped) {
                    inString = !inString;
                    continue;
                }
                if (inString) continue;
                if (ch == '[') depth++;
                if (ch == ']') depth--;
            }
        } else {
            while (pos_ < json_.length() && std::isalpha(json_[pos_])) {
                pos_++;
            }
        }
    }
    
    std::string extractObjectString() {
        skipWhitespace();
        if (pos_ >= json_.length() || json_[pos_] != '{') return "";
        
        size_t start = pos_;
        int depth = 1;
        bool inString = false;
        bool escaped = false;
        
        pos_++;
        
        while (pos_ < json_.length()) {
            char c = json_[pos_++];
            
            if (escaped) {
                escaped = false;
                continue;
            }
            
            if (c == '\\') {
                escaped = true;
                continue;
            }
            
            if (c == '"' && !escaped) {
                inString = !inString;
                continue;
            }
            
            if (inString) continue;
            
            if (c == '{') depth++;
            if (c == '}') {
                depth--;
                if (depth == 0) {
                    return json_.substr(start, pos_ - start);
                }
            }
        }
        
        return "";
    }
};

} // namespace JsonUtils