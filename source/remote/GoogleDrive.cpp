#include "remote/GoogleDrive.hpp"

#include "error.hpp"
#include "logging/logger.hpp"
#include "remote/Form.hpp"
#include "remote/URL.hpp"
#include "remote/remote.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"

#include <algorithm>
#include <cstring>

namespace
{
    // This is the sort-of template for the authorization/token header.
    constexpr std::string_view HEADER_AUTH_BEARER = "Authorization: Bearer ";

    // Content type headers used.
    constexpr const char *HEADER_CONTENT_TYPE_JSON = "Content-Type: application/json";
    constexpr const char *HEADER_CONTENT_TYPE_FORM = "Content-Type: application/x-www-form-urlencoded";

    // This is used for upload/patch for grabbing the upload location.
    constexpr std::string_view HEADER_UPLOAD_LOCATION = "Location";

    // These are API endpoints used in multiple request calls.
    constexpr const char *URL_OAUTH2_TOKEN_URL = "https://oauth2.googleapis.com/token";
    constexpr const char *URL_DRIVE_FILE_API   = "https://www.googleapis.com/drive/v3/files";
    constexpr const char *URL_DRIVE_UPLOAD_API = "https://www.googleapis.com/upload/drive/v3/files";

    // These are json keys that are used for various requests.
    constexpr const char *JSON_KEY_ACCESS_TOKEN  = "access_token";
    constexpr const char *JSON_KEY_CLIENT_ID     = "client_id";
    constexpr const char *JSON_KEY_CLIENT_SECRET = "client_secret";
    constexpr const char *JSON_KEY_DEVICE_CODE   = "device_code";
    constexpr const char *JSON_KEY_EXPIRES_IN    = "expires_in";
    constexpr const char *JSON_KEY_GRANT_TYPE    = "grant_type";
    constexpr const char *JSON_KEY_ID            = "id";
    constexpr const char *JSON_KEY_INSTALLED     = "installed";
    constexpr const char *JSON_KEY_MIMETYPE      = "mimeType";
    constexpr const char *JSON_KEY_NAME          = "name";
    constexpr const char *JSON_KEY_PARENTS       = "parents";
    constexpr const char *JSON_KEY_REFRESH_TOKEN = "refresh_token";

    /// @brief Folder mimetype string.
    constexpr const char *MIME_TYPE_DIRECTORY = "application/vnd.google-apps.folder";
} // namespace

//                      ---- Construction ----

remote::GoogleDrive::GoogleDrive()
    : Storage("[GD]", true)
{
    static constexpr const char *STRING_ERROR_READING_CONFIG = "Error reading Google Drive config: %s";

    // Load the json file.
    json::Object clientJson = json::new_object(json_object_from_file, remote::PATH_GOOGLE_DRIVE_CONFIG.data());
    if (!clientJson) { return; }

    json_object *installed = json::get_object(clientJson, JSON_KEY_INSTALLED);
    if (error::is_null(installed)) { return; }

    json_object *clientId     = json_object_object_get(installed, JSON_KEY_CLIENT_ID);
    json_object *clientSecret = json_object_object_get(installed, JSON_KEY_CLIENT_SECRET);
    json_object *refreshToken = json_object_object_get(installed, JSON_KEY_REFRESH_TOKEN);
    if (error::is_null({clientId, clientSecret})) { return; }
    // Grab them.
    m_clientId     = json_object_get_string(clientId);
    m_clientSecret = json_object_get_string(clientSecret);

    // Returning here will make is_initialized return false.
    if (error::is_null(refreshToken)) { return; }
    m_refreshToken = json_object_get_string(refreshToken);

    if (!GoogleDrive::refresh_token())
    {
        // If refreshing the token failed, this will cause is_initialized to return false and force a re-signin.
        m_refreshToken.clear();
    }
    else if (GoogleDrive::get_root_id() && GoogleDrive::request_listing()) { m_isInitialized = true; }
}

//                      ---- Public functions ----

bool remote::GoogleDrive::create_directory(std::string_view name)
{
    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    curl::HeaderList headers = curl::new_header_list();
    curl::append_header(headers, m_authHeader);
    curl::append_header(headers, HEADER_CONTENT_TYPE_JSON);

    json::Object postJson      = json::new_object(json_object_new_object);
    json_object *directoryName = json_object_new_string(name.data());
    json_object *mimeType      = json_object_new_string(MIME_TYPE_DIRECTORY);
    json::add_object(postJson, JSON_KEY_NAME, directoryName);
    json::add_object(postJson, JSON_KEY_MIMETYPE, mimeType);
    if (!m_parent.empty())
    {
        // I don't understand why this is an array.
        json_object *parentArray = json_object_new_array();
        json_object *parentId    = json_object_new_string(m_parent.c_str());
        json_object_array_add(parentArray, parentId);
        json::add_object(postJson, JSON_KEY_PARENTS, parentArray);
    }

    std::string response;
    curl::prepare_post(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, headers.get());
    curl::set_option(m_curl, CURLOPT_URL, URL_DRIVE_FILE_API);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, json_object_get_string(postJson.get()));

    if (!curl::perform(m_curl)) { return false; }

    json::Object parser = json::new_object(json_tokener_parse, response.c_str());
    if (!parser)
    {
        logger::log("Error creating directory: Error parsing server response.");
        return false;
    }

    // This is all I really need from the response.
    json_object *id = json::get_object(parser, JSON_KEY_ID);
    if (error::is_null(id))
    {
        // This doesn't really mean the request wasn't successful. It just means the new directory couldn't be appended
        // to the list...
        logger::log("Error creating directory: Error finding directory ID in response!");
        return false;
    }

    m_list.emplace_back(name, json_object_get_string(id), m_parent, 0, true);

    return true;
}

bool remote::GoogleDrive::upload_file(const fslib::Path &source, std::string_view name, sys::ProgressTask *task)
{
    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    fslib::File sourceFile(source, FsOpenMode_Read);
    if (!sourceFile)
    {
        logger::log("Error uploading file: %s", fslib::error::get_string());
        return false;
    }

    curl::HeaderList headers = curl::new_header_list();
    curl::append_header(headers, m_authHeader);
    curl::append_header(headers, HEADER_CONTENT_TYPE_JSON);

    // I don't know if I like this much. Looks like I'm using a high level language instead of a real one.
    remote::URL url{URL_DRIVE_UPLOAD_API};
    url.append_parameter("uploadType", "resumable");

    // Json to post.
    json::Object postJson  = json::new_object(json_object_new_object);
    json_object *driveName = json_object_new_string(name.data());
    json::add_object(postJson, JSON_KEY_NAME, driveName);
    if (!m_parent.empty())
    {
        json_object *parentArray = json_object_new_array();
        json_object *parentId    = json_object_new_string(m_parent.c_str());
        json_object_array_add(parentArray, parentId);
        json::add_object(postJson, JSON_KEY_PARENTS, parentArray);
    }

    curl::HeaderArray headerArray;
    curl::prepare_post(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, headers.get());
    curl::set_option(m_curl, CURLOPT_HEADERFUNCTION, curl::write_header_array);
    curl::set_option(m_curl, CURLOPT_HEADERDATA, &headerArray);
    curl::set_option(m_curl, CURLOPT_URL, url.get());
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, json_object_get_string(postJson.get()));

    if (!curl::perform(m_curl)) { return false; }

    // Extract the location from the headers.
    std::string location;
    if (!curl::get_header_value(headerArray, HEADER_UPLOAD_LOCATION.data(), location))
    {
        logger::log("Error uploading file: Couldn't extract upload location from headers.");
        return false;
    }

    const int64_t sourceSize = sourceFile.get_size();
    if (task) { task->reset(static_cast<double>(sourceSize)); }

    std::string response;
    curl::UploadStruct uploadData = {.source = &sourceFile, .task = task};
    // This is the actual upload. This doesn't need the authentication header to work for some reason?
    curl::prepare_upload(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, location.c_str());
    curl::set_option(m_curl, CURLOPT_READFUNCTION, curl::read_data_from_file);
    curl::set_option(m_curl, CURLOPT_READDATA, &uploadData);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);

    if (!curl::perform(m_curl)) { return false; }

    json::Object responseParser = json::new_object(json_tokener_parse, response.c_str());
    if (!responseParser)
    {
        logger::log("Error uploading file: response could not be parsed.");
        return false;
    }

    json_object *id       = json::get_object(responseParser, JSON_KEY_ID);
    json_object *filename = json::get_object(responseParser, JSON_KEY_NAME);
    if (error::is_null({id, filename}))
    {
        logger::log("Error uploading file: server response is missing data required.");
        return false;
    }

    const char *idString   = json_object_get_string(id);
    const char *nameString = json_object_get_string(filename);
    m_list.emplace_back(nameString, idString, m_parent, sourceSize, false);

    return true;
}

bool remote::GoogleDrive::patch_file(remote::Item *file, const fslib::Path &source, sys::ProgressTask *task)
{
    static constexpr const char *STRING_PATCH_ERROR = "Error patching file: %s";

    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    fslib::File sourceFile(source, FsOpenMode_Read);
    if (!sourceFile)
    {
        logger::log(STRING_PATCH_ERROR, fslib::error::get_string());
        return false;
    }

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, m_authHeader);

    remote::URL url{URL_DRIVE_UPLOAD_API};
    url.append_path(file->get_id()).append_parameter("uploadType", "resumable");

    std::string response;
    curl::HeaderArray headerArray;
    curl::reset_handle(m_curl);
    curl::set_option(m_curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_HEADERFUNCTION, curl::write_header_array);
    curl::set_option(m_curl, CURLOPT_HEADERDATA, &headerArray);
    curl::set_option(m_curl, CURLOPT_URL, url.get());
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);

    if (!curl::perform(m_curl)) { return false; }

    // This is the target location to upload to.
    std::string location;
    if (!curl::get_header_value(headerArray, HEADER_UPLOAD_LOCATION, location))
    {
        logger::log(STRING_PATCH_ERROR, "Location could not be read from headers!");
        return false;
    }

    if (task) { task->reset(static_cast<double>(sourceFile.get_size())); }

    // For some reason, this doesn't need the auth header.
    curl::UploadStruct uploadData = {.source = &sourceFile, .task = task};

    curl::prepare_upload(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, location.c_str());
    curl::set_option(m_curl, CURLOPT_READFUNCTION, curl::read_data_from_file);
    curl::set_option(m_curl, CURLOPT_READDATA, &uploadData);

    if (!curl::perform(m_curl)) { return false; }

    // Update the file size with the source file size.
    file->set_size(sourceFile.get_size());

    return true;
}

bool remote::GoogleDrive::download_file(const remote::Item *file, const fslib::Path &destination, sys::ProgressTask *task)
{
    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    const int64_t itemSize = file->get_size();
    fslib::File destFile{destination, FsOpenMode_Create | FsOpenMode_Write};
    if (!destFile)
    {
        logger::log("Error downloading file: local file could not be opened for writing!");
        return false;
    }

    if (task) { task->reset(static_cast<double>(itemSize)); }

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, m_authHeader);

    remote::URL url{URL_DRIVE_FILE_API};
    url.append_path(file->get_id()).append_parameter("alt", "media");

    auto download = curl::create_download_struct(destFile, task, itemSize);

    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_URL, url.get());
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::download_file_threaded);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, download.get());

    sys::threadpool::push_job(curl::download_write_thread_function, download);
    if (!curl::perform(m_curl)) { return false; }

    download->writeComplete.acquire();

    return true;
}

bool remote::GoogleDrive::delete_item(const remote::Item *item)
{
    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    // Iterator is needed to remove it from the list.
    const std::string_view itemId = item->get_id();
    auto findItem                 = Storage::find_item_by_id(itemId);
    if (findItem == m_list.end())
    {
        logger::log("Error deleting item: Item not found in list!");
        return false;
    }

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, m_authHeader);

    remote::URL url{URL_DRIVE_FILE_API};
    url.append_path(itemId);

    curl::reset_handle(m_curl);
    curl::set_option(m_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl::set_option(m_curl, CURLOPT_URL, url.get());
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());

    if (!curl::perform(m_curl)) { return false; }

    // This might be a better way to check?
    long code = curl::get_response_code(m_curl);
    if (code != 204)
    {
        logger::log("Error deleting item from Google Drive: %i.", code);
        return false;
    }

    // Erase from the master list.
    m_list.erase(findItem);

    return true;
}

bool remote::GoogleDrive::rename_item(remote::Item *item, std::string_view newName)
{
    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, m_authHeader);
    curl::append_header(header, HEADER_CONTENT_TYPE_JSON);

    remote::URL url{URL_DRIVE_FILE_API};
    url.append_path(item->get_id());

    json::Object post = json::new_object(json_object_new_object);
    json_object *name = json_object_new_string(newName.data());
    json::add_object(post, "name", name);

    std::string response{};
    const char *postString = json_object_get_string(post.get());
    curl::reset_handle(m_curl);
    curl::set_option(m_curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl::set_option(m_curl, CURLOPT_URL, url.get());
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, HEADER_CONTENT_TYPE_JSON);
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, postString);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);

    if (!curl::perform(m_curl)) { return false; }

    // We're only doing this to check for errors.
    json::Object responseParser = json::new_object(json_tokener_parse, response.c_str());
    if (GoogleDrive::error_occurred(responseParser)) { return false; }

    item->set_name(newName);
    return true;
}

bool remote::GoogleDrive::sign_in_required() const { return !m_isInitialized || m_refreshToken.empty(); }

bool remote::GoogleDrive::get_sign_in_data(std::string &message, std::string &code, std::time_t &expiration, int &wait)
{
    static constexpr const char *STRING_SIGN_IN_ERROR = "Error requesting sign-in data: %s";
    // This is the the URL for device codes and limited input.
    static constexpr const char *API_URL_DEVICE_CODE = "https://oauth2.googleapis.com/device/code";
    // This is a gimped version of what I used to use.
    static constexpr const char *STRING_DRIVE_FILE_SCOPE = "https://www.googleapis.com/auth/drive.file";

    const char *messageTemplate = strings::get_by_name(strings::names::GOOGLE_DRIVE, 0);

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, HEADER_CONTENT_TYPE_FORM);

    remote::Form post{};
    post.append_parameter(JSON_KEY_CLIENT_ID, m_clientId).append_parameter("scope", STRING_DRIVE_FILE_SCOPE);

    std::string response;
    curl::prepare_post(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_URL, API_URL_DEVICE_CODE);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, post.get());
    curl::set_option(m_curl, CURLOPT_POSTFIELDSIZE, post.length());

    if (!curl::perform(m_curl)) { return false; }

    json::Object parser = json::new_object(json_tokener_parse, response.c_str());
    if (!parser || GoogleDrive::error_occurred(parser)) { return false; }

    json_object *deviceCode      = json::get_object(parser, JSON_KEY_DEVICE_CODE);
    json_object *userCode        = json::get_object(parser, "user_code");
    json_object *verificationUrl = json::get_object(parser, "verification_url");
    json_object *expiresIn       = json::get_object(parser, "expires_in");
    json_object *interval        = json::get_object(parser, "interval");
    // These are required and fatal.
    if (error::is_null({deviceCode, userCode, verificationUrl, expiresIn, interval}))
    {
        logger::log(STRING_SIGN_IN_ERROR, "Malformed response.");
        return false;
    }

    message    = stringutil::get_formatted_string(messageTemplate,
                                               json_object_get_string(verificationUrl),
                                               json_object_get_string(userCode));
    code       = json_object_get_string(deviceCode);
    expiration = std::time(NULL) + json_object_get_uint64(expiresIn);
    wait       = json_object_get_int64(interval);

    return true;
}

bool remote::GoogleDrive::poll_sign_in(std::string_view code)
{
    static constexpr const char *STRING_ERROR_POLLING = "Error polling Google OAuth2: %s";
    // I'm just gonna save this pre-escaped instead of wasting time escaping it.
    static constexpr const char *STRING_DEVICE_GRANT = "urn:ietf:params:oauth:grant-type:device_code";

    // I'm not sure how else to make this really work with JKSV's task threading system?
    remote::Form post{};
    post.append_parameter(JSON_KEY_CLIENT_ID, m_clientId)
        .append_parameter(JSON_KEY_CLIENT_SECRET, m_clientSecret)
        .append_parameter("device_code", code)
        .append_parameter(JSON_KEY_GRANT_TYPE, STRING_DEVICE_GRANT);

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, HEADER_CONTENT_TYPE_FORM);

    std::string response;
    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_URL, URL_OAUTH2_TOKEN_URL);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, post.get());
    curl::set_option(m_curl, CURLOPT_POSTFIELDSIZE, post.length());

    if (!curl::perform(m_curl) || response.empty()) { return false; }

    json::Object parser = json::new_object(json_tokener_parse, response.c_str());
    // This error isn't logged, because that's how you know if the user logged in or not.
    if (!parser || GoogleDrive::error_occurred(parser, false)) { return false; }

    json_object *accessToken  = json::get_object(parser, JSON_KEY_ACCESS_TOKEN);
    json_object *expiresIn    = json::get_object(parser, JSON_KEY_EXPIRES_IN);
    json_object *refreshToken = json::get_object(parser, JSON_KEY_REFRESH_TOKEN);
    // All of these are required.
    if (error::is_null({accessToken, expiresIn, refreshToken}))
    {
        logger::log(STRING_ERROR_POLLING, "Malformed response or missing data!");
        return false;
    }

    m_token        = json_object_get_string(accessToken);
    m_refreshToken = json_object_get_string(refreshToken);
    m_tokenExpires = std::time(NULL) + json_object_get_uint64(expiresIn);
    m_authHeader   = std::string(HEADER_AUTH_BEARER) + m_token;

    json::Object config = json::new_object(json_object_from_file, remote::PATH_GOOGLE_DRIVE_CONFIG.data());
    if (config)
    {
        // We're going to attach the refresh token to the installed object so nothing bad can happen to it and I don't
        // have to deal with Git issues about it.
        json_object *installed    = json::get_object(config, JSON_KEY_INSTALLED);
        json_object *refreshToken = json_object_new_string(m_refreshToken.c_str());
        json_object_object_add(installed, JSON_KEY_REFRESH_TOKEN, refreshToken);

        fslib::File configFile{remote::PATH_GOOGLE_DRIVE_CONFIG, FsOpenMode_Create | FsOpenMode_Write};
        if (configFile.is_open()) { configFile << json_object_get_string(config.get()); }
    }

    if (!GoogleDrive::get_root_id()) { return false; }

    m_isInitialized = true;

    return true;
}

//                      ---- Private functions ----

bool remote::GoogleDrive::get_root_id()
{
    // This is the only place this is used. V3 of the API doesn't allow you to retrieve this for some reason?
    static constexpr const char *API_URL_ABOUT_ROOT_ID = "https://www.googleapis.com/drive/v2/about?fields=rootFolderId";

    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, m_authHeader);

    std::string response;
    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_URL, API_URL_ABOUT_ROOT_ID);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);

    if (!curl::perform(m_curl)) { return false; }

    json::Object parser = json::new_object(json_tokener_parse, response.c_str());
    if (!parser) { return false; }

    json_object *rootId = json::get_object(parser, "rootFolderId");
    if (error::is_null(rootId))
    {
        logger::log("Error getting drive root directory ID!");
        return false;
    }

    m_root   = json_object_get_string(rootId);
    m_parent = m_root;

    return true;
}

bool remote::GoogleDrive::token_is_valid() const noexcept
{
    // I'm giving this a grace period just to be safe.
    return std::time(NULL) < m_tokenExpires - 10;
}

bool remote::GoogleDrive::refresh_token()
{
    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, HEADER_CONTENT_TYPE_FORM);

    remote::Form post{};
    post.append_parameter(JSON_KEY_CLIENT_ID, m_clientId)
        .append_parameter(JSON_KEY_CLIENT_SECRET, m_clientSecret)
        .append_parameter(JSON_KEY_REFRESH_TOKEN, m_refreshToken)
        .append_parameter(JSON_KEY_GRANT_TYPE, JSON_KEY_REFRESH_TOKEN);

    std::string response;
    curl::prepare_post(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_URL, URL_OAUTH2_TOKEN_URL);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, post.get());
    curl::set_option(m_curl, CURLOPT_POSTFIELDSIZE, post.length());

    if (!curl::perform(m_curl)) { return false; }

    json::Object parser = json::new_object(json_tokener_parse, response.c_str());
    if (!parser || GoogleDrive::error_occurred(parser)) { return false; }

    // These are the only things I care about.
    json_object *accessToken = json::get_object(parser, JSON_KEY_ACCESS_TOKEN);
    json_object *expiresIn   = json::get_object(parser, JSON_KEY_EXPIRES_IN);
    if (error::is_null({accessToken, expiresIn})) { return false; }

    m_token        = json_object_get_string(accessToken);
    m_tokenExpires = std::time(NULL) + json_object_get_uint64(expiresIn);
    m_authHeader   = std::string(HEADER_AUTH_BEARER) + m_token;

    return true;
}

bool remote::GoogleDrive::request_listing()
{
    if (!GoogleDrive::token_is_valid() && !GoogleDrive::refresh_token()) { return false; }

    curl::HeaderList header = curl::new_header_list();
    curl::append_header(header, m_authHeader.c_str());

    remote::URL url{URL_DRIVE_FILE_API};
    url.append_parameter("fields", "nextPageToken,files(name,id,size,parents,mimeType)")
        .append_parameter("orderBy", "name_natural")
        .append_parameter("pageSize", "256")
        .append_parameter("q", "trashed%3Dfalse"); // This works, but could get messy if more parameters are added.

    std::string response;
    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, header.get());
    curl::set_option(m_curl, CURLOPT_URL, url.get());
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);

    // This is used as the loop condition.
    json_object *nextPageToken = nullptr;
    do {
        response.clear();

        if (!curl::perform(m_curl)) { return false; }

        json::Object parser = json::new_object(json_tokener_parse, response.c_str());
        if (!parser || GoogleDrive::error_occurred(parser) || !GoogleDrive::process_listing(parser))
        {
            logger::log("Error while parsing Google Drive response!");
            return false;
        }

        nextPageToken = json::get_object(parser, "nextPageToken");
        if (nextPageToken)
        {
            remote::URL nextPage{url};
            nextPage.append_parameter("pageToken", json_object_get_string(nextPageToken));
            curl::set_option(m_curl, CURLOPT_URL, nextPage);
        }
    } while (nextPageToken);

    return true;
}

bool remote::GoogleDrive::process_listing(json::Object &json)
{
    static constexpr const char *STRING_ERROR_PROCESSING = "Error processing Google Drive listing: %s";

    json_object *files = json::get_object(json, "files");
    if (!files) { return false; }

    size_t arrayLength = json_object_array_length(files);
    for (size_t i = 0; i < arrayLength; i++)
    {
        json_object *currentFile = json_object_array_get_idx(files, i);
        if (!currentFile) { return false; }

        json_object *mimeType = json_object_object_get(currentFile, JSON_KEY_MIMETYPE);
        json_object *parents  = json_object_object_get(currentFile, JSON_KEY_PARENTS);
        json_object *id       = json_object_object_get(currentFile, JSON_KEY_ID);
        json_object *name     = json_object_object_get(currentFile, JSON_KEY_NAME);
        json_object *size     = json_object_object_get(currentFile, "size");
        if (error::is_null({mimeType, parents, id, name}))
        {
            logger::log(STRING_ERROR_PROCESSING, "Malformed or missing data!");
            continue;
        }

        // I still think it's stupid this is an array when there can only be one...
        json_object *parent = json_object_array_get_idx(parents, 0);
        if (error::is_null(parent))
        {
            logger::log(STRING_ERROR_PROCESSING, "Missing parent ID!");
            continue;
        }

        m_list.emplace_back(json_object_get_string(name),
                            json_object_get_string(id),
                            json_object_get_string(parent),
                            size ? json_object_get_uint64(size) : 0,
                            std::strcmp(MIME_TYPE_DIRECTORY, json_object_get_string(mimeType)) == 0);
    }

    return true;
}

bool remote::GoogleDrive::error_occurred(json::Object &json, bool log) noexcept
{
    json_object *error = json::get_object(json, "error");
    if (!error) { return false; }

    // Google has so many different error response structures. I'm covering two here. Technically,
    // I could grab the code too from the second response, that makes this even more of a headache.
    json_object *description = json::get_object(json, "error_description");
    json_object *message     = json_object_object_get(error, "message");
    if (log && (description || message))
    {
        logger::log("Google Drive error: %s.",
                    description ? json_object_get_string(description) : json_object_get_string(message));
    }

    return true;
}
