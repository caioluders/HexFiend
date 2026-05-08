#include "EngineDocument.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <fstream>
#include <unistd.h>

#import <Foundation/Foundation.h>
#import <HexFiend/HexFiendCore.h>

namespace hexfiend::linux_ui {

EngineDocument::EngineDocument()
    : byteArray_(nullptr)
    , fileReference_(nullptr)
    , modified_(false) {
}

EngineDocument::~EngineDocument() {
    close();
}

void EngineDocument::close() {
    if (fileReference_) {
        [(HFFileReference*)fileReference_ close];
    }
    byteArray_ = nullptr;
    fileReference_ = nullptr;
    path_.clear();
    modified_ = false;
}

void EngineDocument::setErrorFromNSError(void* error, const std::string& fallback) {
    NSError* nsError = (NSError*)error;
    if (nsError) {
        NSString* description = [nsError localizedDescription];
        error_ = description ? [description UTF8String] : fallback;
    } else {
        error_ = fallback;
    }
}

bool EngineDocument::createEmpty() {
    @autoreleasepool {
        close();
        HFBTreeByteArray* byteArray = [[HFBTreeByteArray alloc] init];
        byteArray_ = byteArray;
        fileReference_ = nullptr;
        path_.clear();
        error_.clear();
        modified_ = false;
        return true;
    }
}

bool EngineDocument::createWithBytes(const std::vector<std::uint8_t>& bytes) {
    @autoreleasepool {
        close();
        NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        HFFullMemoryByteSlice* slice = [[HFFullMemoryByteSlice alloc] initWithData:data];
        HFBTreeByteArray* byteArray = [[HFBTreeByteArray alloc] init];
        [byteArray insertByteSlice:slice inRange:HFRangeMake(0, 0)];
        byteArray_ = byteArray;
        fileReference_ = nullptr;
        path_.clear();
        error_.clear();
        modified_ = false;
        return true;
    }
}

void EngineDocument::closeDocument() {
    close();
    error_.clear();
}

bool EngineDocument::open(const std::string& path) {
    @autoreleasepool {
        close();
        error_.clear();

        NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsPath) {
            error_ = "Invalid path encoding";
            return false;
        }

        NSError* error = nil;
        HFFileReference* fileReference = [[HFFileReference alloc] initWithPath:nsPath error:&error];
        if (!fileReference) {
            setErrorFromNSError(error, "Failed to open file");
            return false;
        }

        HFFileByteSlice* fileSlice = [[HFFileByteSlice alloc] initWithFile:fileReference];
        HFBTreeByteArray* byteArray = [[HFBTreeByteArray alloc] initWithByteSlice:fileSlice];

        fileReference_ = fileReference;
        byteArray_ = byteArray;
        path_ = path;
        modified_ = false;
        return true;
    }
}

bool EngineDocument::save() {
    if (path_.empty()) {
        error_ = "No output path";
        return false;
    }
    return saveAs(path_);
}

bool EngineDocument::saveAs(const std::string& path) {
    @autoreleasepool {
        if (!byteArray_) {
            error_ = "No file is open";
            return false;
        }

        const std::string targetPath = path;
        NSString* nsPath = [NSString stringWithUTF8String:targetPath.c_str()];
        if (!nsPath) {
            error_ = "Invalid path encoding";
            return false;
        }

        std::string temporaryTemplate = targetPath + ".hexfiend-save-XXXXXX";
        std::vector<char> temporaryPath(temporaryTemplate.begin(), temporaryTemplate.end());
        temporaryPath.push_back('\0');
        const int temporaryFd = mkstemp(temporaryPath.data());
        if (temporaryFd < 0) {
            error_ = "Failed to create temporary save file";
            return false;
        }
        ::close(temporaryFd);
        const std::string temporaryPathString = temporaryPath.data();
        std::remove(temporaryPathString.c_str());

        std::ofstream output(temporaryPathString, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::remove(temporaryPathString.c_str());
            error_ = "Failed to open temporary save file";
            return false;
        }

        constexpr std::uint64_t chunkSize = 1024 * 1024;
        const std::uint64_t documentLength = length();
        std::vector<std::uint8_t> buffer;
        for (std::uint64_t offset = 0; offset < documentLength; offset += chunkSize) {
            const std::uint64_t amount = std::min<std::uint64_t>(chunkSize, documentLength - offset);
            if (!read(offset, static_cast<std::size_t>(amount), buffer)) {
                output.close();
                std::remove(temporaryPathString.c_str());
                error_ = "Failed to read document while saving";
                return false;
            }
            output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            if (!output) {
                output.close();
                std::remove(temporaryPathString.c_str());
                error_ = "Failed to write temporary save file";
                return false;
            }
        }
        output.close();
        if (!output) {
            std::remove(temporaryPathString.c_str());
            error_ = "Failed to finish temporary save file";
            return false;
        }
        close();
        if (std::rename(temporaryPathString.c_str(), targetPath.c_str()) != 0) {
            std::remove(temporaryPathString.c_str());
            error_ = "Failed to replace output file";
            return false;
        }
        return open(targetPath);
    }
}

bool EngineDocument::isOpen() const {
    return byteArray_ != nullptr;
}

bool EngineDocument::isModified() const {
    return modified_;
}

std::uint64_t EngineDocument::length() const {
    if (!byteArray_) return 0;
    return [(HFByteArray*)byteArray_ length];
}

bool EngineDocument::read(std::uint64_t offset, std::size_t amount, std::vector<std::uint8_t>& out) const {
    if (!byteArray_) return false;
    const std::uint64_t documentLength = length();
    if (offset > documentLength) return false;
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(amount, documentLength - offset));
    out.resize(available);
    if (available > 0) {
        [(HFByteArray*)byteArray_ copyBytes:out.data() range:HFRangeMake(offset, available)];
    }
    return true;
}

bool EngineDocument::replace(std::uint64_t offset,
                             std::uint64_t amount,
                             const std::vector<std::uint8_t>& bytes) {
    @autoreleasepool {
        if (!byteArray_) {
            error_ = "No file is open";
            return false;
        }
        const std::uint64_t documentLength = length();
        if (offset > documentLength || amount > documentLength - offset) {
            error_ = "Edit range is outside the file";
            return false;
        }

        NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        HFFullMemoryByteSlice* slice = [[HFFullMemoryByteSlice alloc] initWithData:data];
        HFBTreeByteArray* replacement = [[HFBTreeByteArray alloc] init];
        [replacement insertByteSlice:slice inRange:HFRangeMake(0, 0)];
        [(HFByteArray*)byteArray_ insertByteArray:replacement inRange:HFRangeMake(offset, amount)];
        modified_ = true;
        error_.clear();
        return true;
    }
}

bool EngineDocument::erase(std::uint64_t offset, std::uint64_t amount) {
    @autoreleasepool {
        if (!byteArray_) {
            error_ = "No file is open";
            return false;
        }
        const std::uint64_t documentLength = length();
        if (offset > documentLength || amount > documentLength - offset) {
            error_ = "Delete range is outside the file";
            return false;
        }
        [(HFByteArray*)byteArray_ deleteBytesInRange:HFRangeMake(offset, amount)];
        modified_ = true;
        error_.clear();
        return true;
    }
}

bool EngineDocument::find(const std::vector<std::uint8_t>& needle,
                          std::uint64_t startOffset,
                          bool forwards,
                          std::uint64_t& resultOffset) {
    @autoreleasepool {
        if (!byteArray_) {
            error_ = "No file is open";
            return false;
        }
        if (needle.empty()) {
            error_ = "Find pattern is empty";
            return false;
        }

        const std::uint64_t documentLength = length();
        if (startOffset > documentLength) startOffset = documentLength;

        NSData* needleData = [NSData dataWithBytes:needle.data() length:needle.size()];
        HFFullMemoryByteSlice* needleSlice = [[HFFullMemoryByteSlice alloc] initWithData:needleData];
        HFBTreeByteArray* needleArray = [[HFBTreeByteArray alloc] init];
        [needleArray insertByteSlice:needleSlice inRange:HFRangeMake(0, 0)];

        HFRange searchRange = forwards
            ? HFRangeMake(startOffset, documentLength - startOffset)
            : HFRangeMake(0, startOffset);
        unsigned long long found = [(HFByteArray*)byteArray_ indexOfBytesEqualToBytes:needleArray
                                                                              inRange:searchRange
                                                                  searchingForwards:forwards
                                                                    trackingProgress:nil];
        if (found == ULLONG_MAX) {
            error_ = "Not found";
            return false;
        }
        resultOffset = found;
        error_.clear();
        return true;
    }
}

const std::string& EngineDocument::path() const {
    return path_;
}

const std::string& EngineDocument::error() const {
    return error_;
}

} // namespace hexfiend::linux_ui
