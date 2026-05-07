#pragma once
#include <string>

namespace TyrianHomeAndGarden {

struct HttpResponse {
    int         status_code = 0;
    std::string body;
};

class HttpClient {
public:
    static std::string  Get(const std::string& url);
    static HttpResponse GetEx(const std::string& url);
    static bool         DownloadToFile(const std::string& url, const std::string& destPath);
};

} // namespace TyrianHomeAndGarden
