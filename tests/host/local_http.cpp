#include <curl/curl.h>
#include <cstdlib>
#include <string>

// The production client uses real curl and JSON parsing, but every request is
// redirected to the local fixture. No test can contact Baidu or use real tokens.
// The original scheme and host are kept in the path so the fixture can check
// which server a request was meant for.
extern "C" CURLcode jksv_test_curl_easy_perform(CURL *curl)
{
    char *url{};
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
    const char *port = std::getenv("JKSV_TEST_PORT");
    if (!url || !port) { return CURLE_URL_MALFORMAT; }
    const std::string original = url;
    const size_t schemeEnd = original.find("://");
    const size_t path = original.find('/', schemeEnd + 3);
    if (schemeEnd == std::string::npos || path == std::string::npos) { return CURLE_URL_MALFORMAT; }
    const std::string scheme = original.substr(0, schemeEnd);
    const std::string host   = original.substr(schemeEnd + 3, path - schemeEnd - 3);
    const std::string local  = "http://127.0.0.1:" + std::string(port) + "/_/" + scheme + "/" + host +
                              original.substr(path);
    curl_easy_setopt(curl, CURLOPT_URL, local.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    // The client only allows HTTPS. Only the redirect to this fixture is plain HTTP.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    return curl_easy_perform(curl);
}
