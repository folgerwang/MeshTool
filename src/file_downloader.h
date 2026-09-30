#pragma once
#include <string>
#include <functional>

class FileDownloader
{
public:
    FileDownloader();
    ~FileDownloader();

    // Download a file from url to dir_path/file_id
    bool downloadFile(const std::string& url, const std::string& file_id, const std::string& dir_path);

    // Async download with callback
    void downloadFileAsync(const std::string& url, const std::string& file_id, const std::string& dir_path,
                           std::function<void(const std::string& path, const std::string& id)> on_complete,
                           std::function<void(const std::string& error)> on_error = nullptr);
};
