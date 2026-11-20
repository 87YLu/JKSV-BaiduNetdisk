#include <curl/curl.h>
#include <cstdlib>
#include <string>

// The production client uses real curl and JSON parsing, but every request is
// redirected to the local fixture. No test can contact Baidu or use real tokens.
extern "C" CURLcode jksv_test_curl_easy_perform(CURL *curl)
{
    char *url{};
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
    const char *port = std::getenv("JKSV_TEST_PORT");
    if (!url || !port) { return CURLE_URL_MALFORMAT; }
    const std::string original = url;
    const size_t path = original.find('/', original.find("://") + 3);
    if (path == std::string::npos) { return CURLE_URL_MALFORMAT; }
    const std::string local = "http://127.0.0.1:" + std::string(port) + original.substr(path);
    curl_easy_setopt(curl, CURLOPT_URL, local.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    return curl_easy_perform(curl);
}
