#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hexfiend::linux_ui {

class EngineDocument {
public:
    EngineDocument();
    ~EngineDocument();

    EngineDocument(const EngineDocument&) = delete;
    EngineDocument& operator=(const EngineDocument&) = delete;

    bool createEmpty();
    bool createWithBytes(const std::vector<std::uint8_t>& bytes);
    void closeDocument();
    bool open(const std::string& path);
    bool save();
    bool saveAs(const std::string& path);
    bool isOpen() const;
    bool isModified() const;

    std::uint64_t length() const;
    bool read(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& out) const;
    bool replace(std::uint64_t offset,
                 std::uint64_t length,
                 const std::vector<std::uint8_t>& bytes);
    bool erase(std::uint64_t offset, std::uint64_t length);
    bool find(const std::vector<std::uint8_t>& needle,
              std::uint64_t startOffset,
              bool forwards,
              std::uint64_t& resultOffset);

    const std::string& path() const;
    const std::string& error() const;

private:
    void close();
    void setErrorFromNSError(void* error, const std::string& fallback);

    void* byteArray_;
    void* fileReference_;
    std::string path_;
    std::string error_;
    bool modified_;
};

} // namespace hexfiend::linux_ui
