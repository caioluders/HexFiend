#include "EngineDocument.hpp"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <tcl.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 620;
constexpr std::size_t kPathBufferSize = 4096;
constexpr std::size_t kTextBufferSize = 1024;
constexpr std::uint64_t kMinBytesPerRow = 4;
constexpr std::uint64_t kMaxBytesPerRow = 128;
constexpr std::uint64_t kDefaultBytesPerRow = 16;
constexpr std::uint64_t kDefaultVisibleRows = 64;
constexpr std::size_t kMaxRecentFiles = 10;
constexpr float kDataInspectorTableHeight = 150.0f;
constexpr float kDataInspectorHeight = 176.0f;
constexpr float kDocumentToolbarSingleRowHeight = 34.0f;
constexpr float kDocumentToolbarTwoRowHeight = 62.0f;
constexpr float kDocumentToolbarCompactWidth = 780.0f;

enum class EditMode {
    Insert,
    Overwrite,
    ReadOnly,
};

enum class EditorPane {
    Hex,
    Ascii,
};

enum class PasteMode {
    Auto,
    Hex,
    Text,
};

enum class PendingDocumentAction {
    None,
    Quit,
    NewDocument,
    CloseDocument,
    OpenPath,
    OpenReadOnlyPath,
    OpenProcessSnapshot,
    RefreshProcessSnapshot,
    RevertDocument,
};

struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct ViewState {
    bool showFindBanner = false;
    bool findAsHex = true;
    bool showLineNumbers = true;
    bool showColumnHeader = false;
    bool showHex = true;
    bool showAscii = true;
    bool showDataInspector = true;
    bool showStatusBar = true;
    bool showScroller = true;
    bool showBinaryTemplates = false;
    bool inspectorBigEndian = false;
    EditMode editMode = EditMode::Insert;
    EditorPane activePane = EditorPane::Hex;
    std::uint64_t viewOffset = 0;
    std::uint64_t bytesPerRow = kDefaultBytesPerRow;
    std::uint64_t visibleRows = kDefaultVisibleRows;
    std::uint64_t selectionOffset = 0;
    std::uint64_t selectionLength = 0;
    std::uint64_t selectionAnchor = 0;
    std::uint64_t multiSelectionAnchor = 0;
    bool multiSelectionInProgress = false;
    std::vector<ByteRange> additionalSelections;
    std::vector<ByteRange> multiSelectionBase;
    int pendingHexNibble = -1;
    std::uint64_t pendingHexOffset = 0;
};

struct AppPreferences {
    EditMode defaultEditMode = EditMode::Insert;
    bool darkTheme = true;
    bool showLineNumbers = true;
    bool showHex = true;
    bool showAscii = true;
    bool showDataInspector = true;
    bool showStatusBar = true;
    bool showScroller = true;
    std::uint64_t bytesPerRow = kDefaultBytesPerRow;
    std::vector<std::string> recentFiles;
};

struct DeviceEntry {
    std::string path;
    std::string label;
};

struct FileBrowserEntry {
    std::string name;
    std::string path;
    bool isDirectory = false;
};

struct FileBrowserState {
    std::string directory;
    std::vector<FileBrowserEntry> entries;
    std::string error;
    bool initialized = false;
};

struct ProcessEntry {
    int pid = 0;
    std::string name;
};

struct MemoryRegion {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::string permissions;
    std::string name;
};

struct ProcessSnapshotState {
    bool active = false;
    int pid = 0;
    MemoryRegion region;
    std::string path;
};

struct SelectionSnapshot {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t anchor = 0;
    std::uint64_t viewOffset = 0;
    std::vector<ByteRange> additionalSelections;
};

struct EditOperation {
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> oldBytes;
    std::vector<std::uint8_t> newBytes;
};

struct EditTransaction {
    std::string label;
    SelectionSnapshot before;
    SelectionSnapshot after;
    std::vector<EditOperation> operations;
};

struct EditHistory {
    std::vector<EditTransaction> undo;
    std::vector<EditTransaction> redo;

    void clear() {
        undo.clear();
        redo.clear();
    }
};

struct DiffRange {
    std::uint64_t leftOffset = 0;
    std::uint64_t rightOffset = 0;
    std::uint64_t leftLength = 0;
    std::uint64_t rightLength = 0;
    std::string leftPreview;
    std::string rightPreview;
};

struct DiffState {
    std::string leftPath;
    std::string rightPath;
    std::uint64_t leftLength = 0;
    std::uint64_t rightLength = 0;
    int selectedRangeIndex = 0;
    bool truncated = false;
    std::vector<DiffRange> ranges;
};

struct TemplateRequirement {
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct TemplateEntry {
    std::string name;
    std::string path;
    std::string source;
    std::vector<TemplateRequirement> requirements;
};

std::vector<ByteRange> normalizedRanges(std::vector<ByteRange> ranges);
void clearPendingHexInput(ViewState& view);

struct TemplateRow {
    std::string path;
    std::string label;
    std::string value;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool isSection = false;
    bool collapsed = false;
};

struct TemplateRunResult {
    std::vector<TemplateRow> rows;
    std::string error;
};

struct CommandLineOptions {
    std::vector<std::string> filesToOpen;
    std::string diffLeftFile;
    std::string diffRightFile;
    std::vector<std::uint8_t> dataToOpen;
    bool hasDataToOpen = false;
    std::string error;
};

void copyToBuffer(std::array<char, kPathBufferSize>& buffer, const std::string& value) {
    std::memset(buffer.data(), 0, buffer.size());
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
}

std::string formatHex(std::uint64_t value, int width = 0) {
    char text[32];
    if (width > 0) {
        std::snprintf(text, sizeof(text), "%0*llX", width, static_cast<unsigned long long>(value));
    } else {
        std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(value));
    }
    return text;
}

int base64Value(char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

bool decodeBase64(std::string_view text, std::vector<std::uint8_t>& output) {
    output.clear();
    int buffer = 0;
    int bits = 0;
    bool sawPadding = false;
    for (char character : text) {
        if (std::isspace(static_cast<unsigned char>(character))) continue;
        if (character == '=') {
            sawPadding = true;
            continue;
        }
        if (sawPadding) return false;
        const int value = base64Value(character);
        if (value < 0) return false;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

CommandLineOptions parseCommandLineOptions(int argc, char** argv) {
    CommandLineOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc || argv[i + 1] == nullptr) return {};
            ++i;
            return argv[i];
        };
        if (arg == "-HFOpenFile") {
            const std::string path = next();
            if (!path.empty()) options.filesToOpen.push_back(path);
        } else if (arg == "-HFDiffLeftFile") {
            options.diffLeftFile = next();
        } else if (arg == "-HFDiffRightFile") {
            options.diffRightFile = next();
        } else if (arg == "-HFOpenData") {
            const std::string encoded = next();
            options.hasDataToOpen = true;
            if (!decodeBase64(encoded, options.dataToOpen)) {
                options.error = "Invalid -HFOpenData base64 payload.";
            }
        } else if (arg == "--compare") {
            options.diffLeftFile = next();
            options.diffRightFile = next();
        } else if (!arg.empty() && arg[0] != '-') {
            options.filesToOpen.push_back(arg);
        }
    }
    return options;
}

std::string formatBytes(std::uint64_t value) {
    if (value < 1024) return std::to_string(value) + " bytes";
    if (value < 1024 * 1024) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.1f kilobytes", static_cast<double>(value) / 1024.0);
        return text;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.1f megabytes", static_cast<double>(value) / (1024.0 * 1024.0));
    return text;
}

std::string formatBytePreview(const std::vector<std::uint8_t>& bytes, std::size_t maxCount = 12) {
    if (bytes.empty()) return "-";
    std::string text;
    const std::size_t count = std::min(bytes.size(), maxCount);
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) text.push_back(' ');
        text += formatHex(bytes[i], 2);
    }
    if (bytes.size() > count) text += " ...";
    return text;
}

int offsetDigitCount(std::uint64_t documentLength) {
    return std::max<int>(5, static_cast<int>(formatHex(documentLength).size()));
}

const char* editModeTitle(EditMode mode) {
    switch (mode) {
    case EditMode::Insert:
        return "";
    case EditMode::Overwrite:
        return " **OVERWRITE MODE**";
    case EditMode::ReadOnly:
        return " **READ-ONLY MODE**";
    }
    return "";
}

int editModeIndex(EditMode mode) {
    switch (mode) {
    case EditMode::Insert:
        return 0;
    case EditMode::Overwrite:
        return 1;
    case EditMode::ReadOnly:
        return 2;
    }
    return 1;
}

EditMode editModeFromIndex(int index) {
    switch (index) {
    case 0:
        return EditMode::Insert;
    case 2:
        return EditMode::ReadOnly;
    case 1:
    default:
        return EditMode::Overwrite;
    }
}

float documentToolbarHeight(float width) {
    return width < kDocumentToolbarCompactWidth ? kDocumentToolbarTwoRowHeight : kDocumentToolbarSingleRowHeight;
}

bool isLikelyBlockDeviceName(std::string_view name) {
    return name.rfind("sd", 0) == 0 ||
           name.rfind("hd", 0) == 0 ||
           name.rfind("vd", 0) == 0 ||
           name.rfind("xvd", 0) == 0 ||
           name.rfind("nvme", 0) == 0 ||
           name.rfind("mmcblk", 0) == 0 ||
           name.rfind("loop", 0) == 0 ||
           name.rfind("md", 0) == 0 ||
           name.rfind("dm-", 0) == 0;
}

std::string realPath(const std::string& path) {
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? std::string(resolved) : std::string();
}

bool directoryExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string parentDirectory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string currentDirectory() {
    char path[PATH_MAX];
    return getcwd(path, sizeof(path)) ? std::string(path) : std::string(".");
}

std::string joinPath(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory == ".") return name;
    if (directory == "/") return "/" + name;
    return directory + "/" + name;
}

std::string executableDirectory() {
    char path[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return {};
    path[length] = '\0';
    return parentDirectory(path);
}

std::string browsingDirectoryForPath(const char* path) {
    if (path == nullptr || path[0] == '\0') return currentDirectory();
    const std::string text(path);
    if (directoryExists(text)) return text;
    const std::string parent = parentDirectory(text);
    return directoryExists(parent) ? parent : currentDirectory();
}

bool refreshFileBrowser(FileBrowserState& browser, const std::string& directory) {
    browser.entries.clear();
    browser.error.clear();
    browser.directory = directory.empty() ? currentDirectory() : directory;
    browser.initialized = true;

    DIR* dir = opendir(browser.directory.c_str());
    if (dir == nullptr) {
        browser.error = std::string("Unable to open directory: ") + std::strerror(errno);
        return false;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = joinPath(browser.directory, name);
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;
        if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) continue;
        browser.entries.push_back(FileBrowserEntry{name, path, S_ISDIR(info.st_mode)});
    }
    closedir(dir);

    std::sort(browser.entries.begin(), browser.entries.end(), [](const FileBrowserEntry& a, const FileBrowserEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });
    return true;
}

std::unordered_map<std::string, std::string> deviceLabels() {
    std::unordered_map<std::string, std::string> labels;
    DIR* directory = opendir("/dev/disk/by-label");
    if (directory == nullptr) return labels;

    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        const std::string linkPath = std::string("/dev/disk/by-label/") + entry->d_name;
        const std::string resolved = realPath(linkPath);
        if (!resolved.empty()) labels[resolved] = entry->d_name;
    }
    closedir(directory);
    return labels;
}

std::vector<DeviceEntry> scanDevices() {
    std::vector<DeviceEntry> devices;
    const auto labels = deviceLabels();
    DIR* directory = opendir("/dev");
    if (directory == nullptr) return devices;

    while (dirent* entry = readdir(directory)) {
        if (!isLikelyBlockDeviceName(entry->d_name)) continue;
        const std::string path = std::string("/dev/") + entry->d_name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0 || !S_ISBLK(info.st_mode)) continue;

        const std::string resolved = realPath(path);
        auto label = labels.find(resolved.empty() ? path : resolved);
        devices.push_back(DeviceEntry{path, label == labels.end() ? std::string() : label->second});
    }
    closedir(directory);

    std::sort(devices.begin(), devices.end(), [](const DeviceEntry& a, const DeviceEntry& b) {
        return a.path < b.path;
    });
    return devices;
}

bool parseHexUint64(std::string_view text, std::uint64_t& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto result = std::from_chars(begin, end, value, 16);
    return result.ec == std::errc() && result.ptr == end;
}

std::string readProcText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::string processDisplayName(int pid) {
    std::string command = readProcText("/proc/" + std::to_string(pid) + "/cmdline");
    for (char& ch : command) {
        if (ch == '\0') ch = ' ';
    }
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) command.pop_back();
    if (!command.empty()) return command;

    command = readProcText("/proc/" + std::to_string(pid) + "/comm");
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) command.pop_back();
    return command.empty() ? "(unknown)" : command;
}

std::string lowercaseAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trimAsciiWhitespace(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool processMatchesFilter(const ProcessEntry& process, std::string_view filter) {
    const std::string query = lowercaseAscii(trimAsciiWhitespace(std::string(filter)));
    if (query.empty()) return true;
    const std::string pid = std::to_string(process.pid);
    if (pid.find(query) != std::string::npos) return true;
    return lowercaseAscii(process.name).find(query) != std::string::npos;
}

bool memoryRegionMatchesFilter(const MemoryRegion& region, std::string_view filter) {
    const std::string query = lowercaseAscii(trimAsciiWhitespace(std::string(filter)));
    if (query.empty()) return true;
    const std::string start = "0x" + lowercaseAscii(formatHex(region.start));
    const std::string end = "0x" + lowercaseAscii(formatHex(region.end));
    if (start.find(query) != std::string::npos || end.find(query) != std::string::npos) return true;
    if (lowercaseAscii(region.permissions).find(query) != std::string::npos) return true;
    return lowercaseAscii(region.name).find(query) != std::string::npos;
}

std::vector<ProcessEntry> scanProcesses() {
    std::vector<ProcessEntry> processes;
    DIR* directory = opendir("/proc");
    if (directory == nullptr) return processes;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch));
        })) {
            continue;
        }
        const int pid = std::atoi(name.c_str());
        if (pid <= 0) continue;
        processes.push_back(ProcessEntry{pid, processDisplayName(pid)});
    }
    closedir(directory);
    std::sort(processes.begin(), processes.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        return a.pid < b.pid;
    });
    return processes;
}

std::vector<MemoryRegion> scanProcessRegions(int pid) {
    std::vector<MemoryRegion> regions;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range;
        std::string permissions;
        std::string offset;
        std::string device;
        std::string inode;
        if (!(fields >> range >> permissions >> offset >> device >> inode)) continue;
        if (permissions.empty() || permissions[0] != 'r') continue;
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos) continue;
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        if (!parseHexUint64(std::string_view(range).substr(0, dash), start) ||
            !parseHexUint64(std::string_view(range).substr(dash + 1), end) ||
            end <= start) {
            continue;
        }
        std::string regionName;
        std::getline(fields, regionName);
        while (!regionName.empty() && std::isspace(static_cast<unsigned char>(regionName.front()))) regionName.erase(regionName.begin());
        regions.push_back(MemoryRegion{start, end, permissions, regionName.empty() ? "(anonymous)" : regionName});
    }
    return regions;
}

std::string readTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool parseHexPattern(std::string_view text, std::vector<std::uint8_t>& bytes) {
    bytes.clear();
    std::string compact;
    compact.reserve(text.size());
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        compact.push_back(c);
    }
    if (compact.empty() || compact.size() % 2 != 0) return false;
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        unsigned int value = 0;
        const auto begin = compact.data() + i;
        const auto end = begin + 2;
        auto result = std::from_chars(begin, end, value, 16);
        if (result.ec != std::errc()) return false;
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return true;
}

bool parseOffset(std::string_view text, std::uint64_t& offset) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) return false;

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    } else if (text.find_first_of("ABCDEFabcdef") != std::string_view::npos) {
        base = 16;
    }
    if (text.empty()) return false;

    const char* begin = text.data();
    const char* end = begin + text.size();
    auto result = std::from_chars(begin, end, offset, base);
    return result.ec == std::errc() && result.ptr == end;
}

std::string preferencesPath() {
    const char* configHome = std::getenv("XDG_CONFIG_HOME");
    if (configHome && configHome[0] != '\0') {
        return std::string(configHome) + "/hexfiend-linux/preferences.conf";
    }
    const char* home = std::getenv("HOME");
    return std::string(home && home[0] != '\0' ? home : ".") + "/.config/hexfiend-linux/preferences.conf";
}

bool parseBoolPreference(const std::map<std::string, std::string>& values,
                         const std::string& key,
                         bool fallback) {
    auto it = values.find(key);
    if (it == values.end()) return fallback;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

std::uint64_t parseUintPreference(const std::map<std::string, std::string>& values,
                                  const std::string& key,
                                  std::uint64_t fallback,
                                  std::uint64_t minimum,
                                  std::uint64_t maximum) {
    auto it = values.find(key);
    if (it == values.end()) return fallback;
    std::uint64_t parsed = fallback;
    if (!parseOffset(it->second, parsed)) return fallback;
    return std::clamp(parsed, minimum, maximum);
}

AppPreferences loadPreferences() {
    AppPreferences preferences;
    std::ifstream file(preferencesPath());
    if (!file) return preferences;

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        values[line.substr(0, equals)] = line.substr(equals + 1);
    }

    preferences.darkTheme = parseBoolPreference(values, "darkTheme", preferences.darkTheme);
    preferences.showLineNumbers = parseBoolPreference(values, "showLineNumbers", preferences.showLineNumbers);
    preferences.showHex = parseBoolPreference(values, "showHex", preferences.showHex);
    preferences.showAscii = parseBoolPreference(values, "showAscii", preferences.showAscii);
    preferences.showDataInspector = parseBoolPreference(values, "showDataInspector", preferences.showDataInspector);
    preferences.showStatusBar = parseBoolPreference(values, "showStatusBar", preferences.showStatusBar);
    preferences.showScroller = parseBoolPreference(values, "showScroller", preferences.showScroller);
    preferences.bytesPerRow = parseUintPreference(values, "bytesPerRow", preferences.bytesPerRow, kMinBytesPerRow, kMaxBytesPerRow);
    preferences.recentFiles.clear();
    for (std::size_t i = 0; i < kMaxRecentFiles; ++i) {
        auto recent = values.find("recentFile" + std::to_string(i));
        if (recent != values.end() && !recent->second.empty()) preferences.recentFiles.push_back(recent->second);
    }

    auto mode = values.find("defaultEditMode");
    if (mode != values.end()) {
        if (mode->second == "overwrite") preferences.defaultEditMode = EditMode::Overwrite;
        else if (mode->second == "readonly") preferences.defaultEditMode = EditMode::ReadOnly;
        else preferences.defaultEditMode = EditMode::Insert;
    }
    return preferences;
}

std::string editModePreferenceValue(EditMode mode) {
    switch (mode) {
        case EditMode::Insert: return "insert";
        case EditMode::Overwrite: return "overwrite";
        case EditMode::ReadOnly: return "readonly";
    }
    return "insert";
}

void ensureByteRepresenterVisible(ViewState& view) {
    if (!view.showHex && !view.showAscii) view.showHex = true;
}

void applyPreferences(ViewState& view, const AppPreferences& preferences) {
    view.showLineNumbers = preferences.showLineNumbers;
    view.showHex = preferences.showHex;
    view.showAscii = preferences.showAscii;
    view.showDataInspector = preferences.showDataInspector;
    view.showStatusBar = preferences.showStatusBar;
    view.showScroller = preferences.showScroller;
    view.bytesPerRow = preferences.bytesPerRow;
    ensureByteRepresenterVisible(view);
}

void capturePreferences(const ViewState& view, EditMode defaultEditMode, AppPreferences& preferences) {
    preferences.defaultEditMode = defaultEditMode;
    preferences.showLineNumbers = view.showLineNumbers;
    preferences.showHex = view.showHex;
    preferences.showAscii = view.showAscii;
    preferences.showDataInspector = view.showDataInspector;
    preferences.showStatusBar = view.showStatusBar;
    preferences.showScroller = view.showScroller;
    preferences.bytesPerRow = view.bytesPerRow;
}

bool ensureParentDirectory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return true;
    const std::string directory = path.substr(0, slash);
    std::string partial;
    for (char ch : directory) {
        partial.push_back(ch);
        if (ch == '/' && partial.size() > 1) mkdir(partial.c_str(), 0700);
    }
    return mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST;
}

bool savePreferences(const AppPreferences& preferences, std::string& status) {
    const std::string path = preferencesPath();
    if (!ensureParentDirectory(path)) {
        status = "Unable to create preferences directory.";
        return false;
    }

    std::ofstream file(path);
    if (!file) {
        status = "Unable to save preferences.";
        return false;
    }
    file << "defaultEditMode=" << editModePreferenceValue(preferences.defaultEditMode) << "\n"
         << "darkTheme=" << (preferences.darkTheme ? 1 : 0) << "\n"
         << "showLineNumbers=" << (preferences.showLineNumbers ? 1 : 0) << "\n"
         << "showHex=" << (preferences.showHex ? 1 : 0) << "\n"
         << "showAscii=" << (preferences.showAscii ? 1 : 0) << "\n"
         << "showDataInspector=" << (preferences.showDataInspector ? 1 : 0) << "\n"
         << "showStatusBar=" << (preferences.showStatusBar ? 1 : 0) << "\n"
         << "showScroller=" << (preferences.showScroller ? 1 : 0) << "\n"
         << "bytesPerRow=" << preferences.bytesPerRow << "\n";
    for (std::size_t i = 0; i < preferences.recentFiles.size() && i < kMaxRecentFiles; ++i) {
        file << "recentFile" << i << "=" << preferences.recentFiles[i] << "\n";
    }
    status = "Saved preferences.";
    return true;
}

bool rememberRecentFile(AppPreferences& preferences, const std::string& path) {
    if (path.empty() || path.rfind("/tmp/hexfiend-process-region-", 0) == 0) return false;
    preferences.recentFiles.erase(std::remove(preferences.recentFiles.begin(), preferences.recentFiles.end(), path),
                                  preferences.recentFiles.end());
    preferences.recentFiles.insert(preferences.recentFiles.begin(), path);
    if (preferences.recentFiles.size() > kMaxRecentFiles) preferences.recentFiles.resize(kMaxRecentFiles);
    return true;
}

bool buildFindPattern(const std::array<char, kTextBufferSize>& findBuffer,
                      bool asHex,
                      std::vector<std::uint8_t>& pattern,
                      std::string& status) {
    if (asHex) {
        if (!parseHexPattern(findBuffer.data(), pattern)) {
            status = "Find field must contain complete hexadecimal bytes.";
            return false;
        }
        return true;
    }
    pattern.assign(findBuffer.data(), findBuffer.data() + std::strlen(findBuffer.data()));
    if (pattern.empty()) {
        status = "Find field is empty.";
        return false;
    }
    return true;
}

bool buildReplacePattern(const std::array<char, kTextBufferSize>& replaceBuffer,
                         bool asHex,
                         std::vector<std::uint8_t>& replacement,
                         std::string& status) {
    if (asHex) {
        if (replaceBuffer[0] == '\0') {
            replacement.clear();
            return true;
        }
        if (!parseHexPattern(replaceBuffer.data(), replacement)) {
            status = "Replace field must contain complete hexadecimal bytes.";
            return false;
        }
        return true;
    }
    replacement.assign(replaceBuffer.data(), replaceBuffer.data() + std::strlen(replaceBuffer.data()));
    return true;
}

std::vector<TemplateRequirement> parseTemplateRequirements(const std::string& source) {
    std::vector<TemplateRequirement> requirements;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        std::string_view text(line);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        if (text.rfind("requires ", 0) != 0) continue;

        std::istringstream fields{std::string(text)};
        std::string command;
        std::string offsetText;
        fields >> command >> offsetText;
        std::uint64_t offset = 0;
        if (!parseOffset(offsetText, offset)) continue;

        const std::size_t firstQuote = line.find('"');
        const std::size_t lastQuote = line.rfind('"');
        if (firstQuote == std::string::npos || lastQuote <= firstQuote) continue;

        std::vector<std::uint8_t> bytes;
        if (!parseHexPattern(std::string_view(line).substr(firstQuote + 1, lastQuote - firstQuote - 1), bytes)) continue;
        requirements.push_back(TemplateRequirement{offset, bytes});
    }
    return requirements;
}

std::string templateDisplayName(const std::string& path) {
    constexpr std::string_view prefix = "templates/";
    std::string name = path;
    if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
    const std::string marker = "/templates/";
    const std::size_t installedPrefix = name.find(marker);
    if (installedPrefix != std::string::npos) name.erase(0, installedPrefix + marker.size());
    if (name.size() > 4 && name.substr(name.size() - 4) == ".tcl") name.erase(name.size() - 4);
    return name;
}

void scanTemplatesRecursive(const std::string& directory, std::vector<TemplateEntry>& templates) {
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) return;

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::string path = directory + "/" + entry->d_name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            scanTemplatesRecursive(path, templates);
        } else if (S_ISREG(info.st_mode) && path.size() > 4 && path.substr(path.size() - 4) == ".tcl") {
            const std::string source = readTextFile(path);
            templates.push_back(TemplateEntry{
                templateDisplayName(path),
                path,
                source,
                parseTemplateRequirements(source),
            });
        }
    }
    closedir(dir);
}

std::vector<TemplateEntry> scanTemplates() {
    std::vector<TemplateEntry> templates;
    std::vector<std::string> roots;
    if (const char* templatePath = std::getenv("HEXFIEND_TEMPLATE_PATH")) {
        std::string remaining = templatePath;
        std::size_t start = 0;
        while (start <= remaining.size()) {
            const std::size_t colon = remaining.find(':', start);
            const std::string root = remaining.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (!root.empty()) roots.push_back(root);
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    roots.push_back("templates");
    const std::string exeDir = executableDirectory();
    if (!exeDir.empty()) roots.push_back(exeDir + "/../share/hexfiend/templates");
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(std::string(home) + "/.local/share/hexfiend/templates");
    }
    roots.push_back("/usr/local/share/hexfiend/templates");
    roots.push_back("/usr/share/hexfiend/templates");

    std::set<std::string> scanned;
    for (const std::string& root : roots) {
        const std::string resolved = realPath(root);
        const std::string key = resolved.empty() ? root : resolved;
        if (!scanned.insert(key).second) continue;
        scanTemplatesRecursive(root, templates);
    }
    std::sort(templates.begin(), templates.end(), [](const TemplateEntry& a, const TemplateEntry& b) {
        return a.name < b.name;
    });
    return templates;
}

bool templateRequirementMatches(hexfiend::linux_ui::EngineDocument& document,
                                const TemplateRequirement& requirement,
                                std::uint64_t anchor) {
    if (!document.isOpen()) return false;
    if (anchor > document.length() || requirement.offset > document.length() - anchor) return false;
    std::vector<std::uint8_t> actual;
    return document.read(anchor + requirement.offset, requirement.bytes.size(), actual) &&
        actual == requirement.bytes;
}

bool templateRequirementsMatch(hexfiend::linux_ui::EngineDocument& document,
                               const TemplateEntry& entry,
                               std::uint64_t anchor) {
    if (entry.requirements.empty()) return false;
    for (const TemplateRequirement& requirement : entry.requirements) {
        if (!templateRequirementMatches(document, requirement, anchor)) return false;
    }
    return true;
}

int findMatchingTemplate(hexfiend::linux_ui::EngineDocument& document,
                         const std::vector<TemplateEntry>& templates,
                         std::uint64_t anchor) {
    for (std::size_t i = 0; i < templates.size(); ++i) {
        if (templateRequirementsMatch(document, templates[i], anchor)) return static_cast<int>(i);
    }
    return -1;
}

struct TemplateExecutionContext {
    hexfiend::linux_ui::EngineDocument* document = nullptr;
    Tcl_Interp* interp = nullptr;
    std::uint64_t anchor = 0;
    std::uint64_t position = 0;
    bool bigEndian = false;
    std::string root = "templates";
    std::vector<std::string> sections;
    std::vector<std::size_t> sectionRows;
    TemplateRunResult result;
};

std::string currentSectionPath(const TemplateExecutionContext& context) {
    std::string path;
    for (const std::string& section : context.sections) {
        if (!path.empty()) path += " / ";
        path += section;
    }
    return path;
}

std::string templateRowSectionPath(const TemplateRow& row) {
    if (row.path.empty()) return row.label;
    if (row.label.empty()) return row.path;
    return row.path + " / " + row.label;
}

bool templateRowIsUnderPath(const TemplateRow& row, const std::string& path) {
    return row.path == path || row.path.rfind(path + " / ", 0) == 0;
}

void setTclError(Tcl_Interp* interp, const std::string& message) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message.c_str(), static_cast<int>(message.size())));
}

std::string tclString(Tcl_Obj* object) {
    int length = 0;
    const char* value = Tcl_GetStringFromObj(object, &length);
    return std::string(value, static_cast<std::size_t>(length));
}

bool parseTemplateLength(TemplateExecutionContext& context,
                         Tcl_Interp* interp,
                         Tcl_Obj* object,
                         std::uint64_t& length) {
    const std::string text = tclString(object);
    if (text == "eof") {
        const std::uint64_t absolutePosition = context.anchor + context.position;
        length = context.document->length() > absolutePosition ? context.document->length() - absolutePosition : 0;
        return true;
    }

    Tcl_WideInt parsed = 0;
    if (Tcl_GetWideIntFromObj(interp, object, &parsed) != TCL_OK) return false;
    if (parsed < 0) {
        setTclError(interp, "length must be non-negative");
        return false;
    }
    length = static_cast<std::uint64_t>(parsed);
    return true;
}

bool readTemplateBytes(TemplateExecutionContext& context,
                       std::uint64_t relativeOffset,
                       std::uint64_t length,
                       std::vector<std::uint8_t>& bytes) {
    bytes.clear();
    if (length == 0) return true;
    const std::uint64_t absoluteOffset = context.anchor + relativeOffset;
    if (absoluteOffset > context.document->length() || length > context.document->length() - absoluteOffset) return false;
    return context.document->read(absoluteOffset, static_cast<std::size_t>(length), bytes);
}

std::uint64_t bytesToUnsigned(const std::vector<std::uint8_t>& bytes, bool bigEndian) {
    std::uint64_t value = 0;
    if (bigEndian) {
        for (std::uint8_t byte : bytes) value = (value << 8) | byte;
    } else {
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) value = (value << 8) | *it;
    }
    return value;
}

std::int64_t signExtend(std::uint64_t value, unsigned bits) {
    if (bits == 64) return static_cast<std::int64_t>(value);
    const std::uint64_t signBit = 1ULL << (bits - 1);
    const std::uint64_t mask = (1ULL << bits) - 1;
    value &= mask;
    return (value & signBit) ? static_cast<std::int64_t>(value | ~mask) : static_cast<std::int64_t>(value);
}

struct Leb128Value {
    std::uint64_t unsignedValue = 0;
    std::int64_t signedValue = 0;
    std::size_t length = 0;
};

bool decodeLeb128(const std::vector<std::uint8_t>& bytes, bool signedValue, Leb128Value& decoded) {
    decoded = {};
    constexpr std::size_t maxLeb128Bytes = 8;
    const std::size_t count = std::min(maxLeb128Bytes, bytes.size());
    unsigned shift = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t byte = bytes[i];
        if (signedValue) {
            decoded.signedValue |= static_cast<std::int64_t>(byte & 0x7F) << shift;
        } else {
            decoded.unsignedValue |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        }
        shift += 7;
        if ((byte & 0x80) == 0) {
            decoded.length = i + 1;
            if (signedValue && shift < 64 && (byte & 0x40) != 0) {
                decoded.signedValue |= -static_cast<std::int64_t>(1ULL << shift);
            }
            return true;
        }
    }
    return false;
}

std::string formatTemplateHexValue(std::uint64_t value, int byteCount) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%0*llX", byteCount * 2, static_cast<unsigned long long>(value));
    return buffer;
}

std::vector<std::uint8_t> bytesInEndianOrder(const std::vector<std::uint8_t>& bytes, bool bigEndian) {
    std::vector<std::uint8_t> ordered = bytes;
    if (bigEndian) std::reverse(ordered.begin(), ordered.end());
    return ordered;
}

std::size_t addTemplateRow(TemplateExecutionContext& context,
                           const std::string& label,
                           const std::string& value,
                           std::uint64_t offset,
                           std::uint64_t length,
                           bool isSection = false,
                           bool collapsed = false) {
    context.result.rows.push_back(TemplateRow{currentSectionPath(context), label, value, context.anchor + offset, length, isSection, collapsed});
    return context.result.rows.size() - 1;
}

int templateEndianCommand(ClientData data, Tcl_Interp*, int, Tcl_Obj* const[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    context->bigEndian = true;
    return TCL_OK;
}

int templateLittleEndianCommand(ClientData data, Tcl_Interp*, int, Tcl_Obj* const[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    context->bigEndian = false;
    return TCL_OK;
}

int templatePositionCommand(ClientData data, Tcl_Interp* interp, int, Tcl_Obj* const[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(context->position)));
    return TCL_OK;
}

int templateLengthCommand(ClientData data, Tcl_Interp* interp, int, Tcl_Obj* const[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    const std::uint64_t length = context->document->length() > context->anchor ? context->document->length() - context->anchor : 0;
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(length)));
    return TCL_OK;
}

int templateEndCommand(ClientData data, Tcl_Interp* interp, int, Tcl_Obj* const[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(context->anchor + context->position >= context->document->length()));
    return TCL_OK;
}

int templateMoveCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "offset");
        return TCL_ERROR;
    }
    Tcl_WideInt amount = 0;
    if (Tcl_GetWideIntFromObj(interp, objv[1], &amount) != TCL_OK) return TCL_ERROR;
    if (amount < 0 && static_cast<std::uint64_t>(-amount) > context->position) {
        setTclError(interp, "move would seek before the template anchor");
        return TCL_ERROR;
    }
    context->position = static_cast<std::uint64_t>(static_cast<Tcl_WideInt>(context->position) + amount);
    return TCL_OK;
}

int templateGotoCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "offset");
        return TCL_ERROR;
    }
    Tcl_WideInt offset = 0;
    if (Tcl_GetWideIntFromObj(interp, objv[1], &offset) != TCL_OK) return TCL_ERROR;
    if (offset < 0) {
        setTclError(interp, "offset must be non-negative");
        return TCL_ERROR;
    }
    context->position = static_cast<std::uint64_t>(offset);
    return TCL_OK;
}

int templateRequiresCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "offset \"hex values\"");
        return TCL_ERROR;
    }
    Tcl_WideInt offset = 0;
    if (Tcl_GetWideIntFromObj(interp, objv[1], &offset) != TCL_OK) return TCL_ERROR;
    if (offset < 0) {
        setTclError(interp, "offset must be non-negative");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> expected;
    if (!parseHexPattern(tclString(objv[2]), expected)) {
        setTclError(interp, "requires expects hexadecimal byte text");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> actual;
    if (!readTemplateBytes(*context, static_cast<std::uint64_t>(offset), expected.size(), actual) || actual != expected) {
        setTclError(interp, "template requirement did not match");
        return TCL_ERROR;
    }
    return TCL_OK;
}

int templateIncludeCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "path");
        return TCL_ERROR;
    }
    const std::string includePath = context->root + "/" + tclString(objv[1]);
    return Tcl_EvalFile(interp, includePath.c_str());
}

int templateNoopCommand(ClientData, Tcl_Interp*, int, Tcl_Obj* const[]) {
    return TCL_OK;
}

int templateSectionCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    int labelIndex = 1;
    const bool collapsed = objc > 1 && tclString(objv[1]) == "-collapsed";
    if (collapsed) labelIndex = 2;
    if (objc < labelIndex + 1 || objc > labelIndex + 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "[-collapsed] label [body]");
        return TCL_ERROR;
    }

    const std::string label = tclString(objv[labelIndex]);
    const std::size_t rowIndex = addTemplateRow(*context, label, "", context->position, 0, true, collapsed);
    if (objc == labelIndex + 2) {
        context->sections.push_back(label);
        context->sectionRows.push_back(rowIndex);
        const int result = Tcl_EvalObjEx(interp, objv[labelIndex + 1], 0);
        context->sectionRows.pop_back();
        context->sections.pop_back();
        return result;
    }
    context->sections.push_back(label);
    context->sectionRows.push_back(rowIndex);
    return TCL_OK;
}

int templateEndSectionCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    if (context->sections.empty()) {
        setTclError(interp, "endsection without matching section");
        return TCL_ERROR;
    }
    context->sections.pop_back();
    context->sectionRows.pop_back();
    return TCL_OK;
}

int templateSectionCollapseCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    if (context->sectionRows.empty()) {
        setTclError(interp, "sectioncollapse without active section");
        return TCL_ERROR;
    }
    context->result.rows[context->sectionRows.back()].collapsed = true;
    return TCL_OK;
}

int templateSectionValueCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "value");
        return TCL_ERROR;
    }
    if (!context->result.rows.empty()) context->result.rows.back().value = tclString(objv[1]);
    return TCL_OK;
}

int templateSectionNameCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
        return TCL_ERROR;
    }
    if (!context->sections.empty()) context->sections.back() = tclString(objv[1]);
    if (!context->result.rows.empty()) context->result.rows.back().label = tclString(objv[1]);
    return TCL_OK;
}

int templateEntryCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 3 || objc > 5) {
        Tcl_WrongNumArgs(interp, 1, objv, "label value [length] [offset]");
        return TCL_ERROR;
    }
    std::uint64_t length = 0;
    std::uint64_t offset = context->position;
    if (objc >= 4) {
        Tcl_WideInt parsedLength = 0;
        if (Tcl_GetWideIntFromObj(interp, objv[3], &parsedLength) != TCL_OK) return TCL_ERROR;
        if (parsedLength < 0) {
            setTclError(interp, "entry length must be non-negative");
            return TCL_ERROR;
        }
        length = static_cast<std::uint64_t>(parsedLength);
    }
    if (objc == 5) {
        Tcl_WideInt parsedOffset = 0;
        if (Tcl_GetWideIntFromObj(interp, objv[4], &parsedOffset) != TCL_OK) return TCL_ERROR;
        if (parsedOffset < 0) {
            setTclError(interp, "entry offset must be non-negative");
            return TCL_ERROR;
        }
        offset = static_cast<std::uint64_t>(parsedOffset);
    }
    addTemplateRow(*context, tclString(objv[1]), tclString(objv[2]), offset, length);
    return TCL_OK;
}

int templateReadBytesCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "length [label]");
        return TCL_ERROR;
    }
    std::uint64_t length = 0;
    if (!parseTemplateLength(*context, interp, objv[1], length)) return TCL_ERROR;
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, length, bytes)) {
        setTclError(interp, "failed to read bytes");
        return TCL_ERROR;
    }
    const std::string label = objc == 3 ? tclString(objv[2]) : "Bytes";
    addTemplateRow(*context, label, formatBytePreview(bytes, 24), context->position, length);
    context->position += length;
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(bytes.data(), static_cast<int>(bytes.size())));
    return TCL_OK;
}

int templateAsciiCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "length [label]");
        return TCL_ERROR;
    }
    std::uint64_t length = 0;
    if (!parseTemplateLength(*context, interp, objv[1], length)) return TCL_ERROR;
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, length, bytes)) {
        setTclError(interp, "failed to read ascii bytes");
        return TCL_ERROR;
    }
    std::string value;
    for (std::uint8_t byte : bytes) value.push_back(std::isprint(static_cast<unsigned char>(byte)) ? static_cast<char>(byte) : '.');
    const std::string label = objc == 3 ? tclString(objv[2]) : "ASCII";
    addTemplateRow(*context, label, value, context->position, length);
    context->position += length;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(value.c_str(), static_cast<int>(value.size())));
    return TCL_OK;
}

std::string printableTemplateString(const std::vector<std::uint8_t>& bytes) {
    std::string value;
    for (std::uint8_t byte : bytes) {
        if (byte == 0) break;
        value.push_back(std::isprint(static_cast<unsigned char>(byte)) ? static_cast<char>(byte) : '.');
    }
    return value;
}

int templateStringCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "length encoding [label]");
        return TCL_ERROR;
    }
    std::uint64_t length = 0;
    if (!parseTemplateLength(*context, interp, objv[1], length)) return TCL_ERROR;
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, length, bytes)) {
        setTclError(interp, "failed to read string bytes");
        return TCL_ERROR;
    }
    const std::string value = printableTemplateString(bytes);
    if (objc == 4) addTemplateRow(*context, tclString(objv[3]), value, context->position, length);
    context->position += length;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(value.c_str(), static_cast<int>(value.size())));
    return TCL_OK;
}

int templateCStringCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "encoding [label]");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> bytes;
    std::uint64_t offset = context->position;
    while (context->anchor + offset < context->document->length()) {
        std::vector<std::uint8_t> one;
        if (!readTemplateBytes(*context, offset, 1, one)) {
            setTclError(interp, "failed to read cstring bytes");
            return TCL_ERROR;
        }
        bytes.push_back(one[0]);
        offset++;
        if (one[0] == 0) break;
    }
    const std::string value = printableTemplateString(bytes);
    if (objc == 3) addTemplateRow(*context, tclString(objv[2]), value, context->position, bytes.size());
    context->position += bytes.size();
    Tcl_SetObjResult(interp, Tcl_NewStringObj(value.c_str(), static_cast<int>(value.size())));
    return TCL_OK;
}

int templateUtf16Command(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "length [label]");
        return TCL_ERROR;
    }
    std::uint64_t length = 0;
    if (!parseTemplateLength(*context, interp, objv[1], length)) return TCL_ERROR;
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, length, bytes)) {
        setTclError(interp, "failed to read UTF-16 bytes");
        return TCL_ERROR;
    }
    std::string value;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const std::uint16_t codeUnit = context->bigEndian
            ? static_cast<std::uint16_t>((bytes[i] << 8) | bytes[i + 1])
            : static_cast<std::uint16_t>((bytes[i + 1] << 8) | bytes[i]);
        if (codeUnit == 0) break;
        value.push_back(codeUnit >= 0x20 && codeUnit < 0x7F ? static_cast<char>(codeUnit) : '.');
    }
    if (objc == 3) addTemplateRow(*context, tclString(objv[2]), value, context->position, length);
    context->position += length;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(value.c_str(), static_cast<int>(value.size())));
    return TCL_OK;
}

template <int ByteCount, bool Signed>
int templateIntegerCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    bool displayHex = false;
    std::string label;
    for (int i = 1; i < objc; ++i) {
        const std::string arg = tclString(objv[i]);
        if (!Signed && arg == "-hex") {
            displayHex = true;
        } else if (label.empty()) {
            label = arg;
        } else {
            Tcl_WrongNumArgs(interp, 1, objv, Signed ? "[label]" : "[-hex] [label]");
            return TCL_ERROR;
        }
    }
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, ByteCount, bytes)) {
        setTclError(interp, "failed to read integer bytes");
        return TCL_ERROR;
    }
    const std::uint64_t unsignedValue = bytesToUnsigned(bytes, context->bigEndian);
    std::string value;
    if constexpr (Signed) {
        const std::int64_t signedValue = signExtend(unsignedValue, ByteCount * 8);
        value = std::to_string(signedValue);
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(signedValue)));
    } else {
        value = displayHex ? formatTemplateHexValue(unsignedValue, ByteCount) : std::to_string(unsignedValue);
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(unsignedValue)));
    }
    if (!label.empty()) addTemplateRow(*context, label, value, context->position, ByteCount);
    context->position += ByteCount;
    return TCL_OK;
}

template <typename FloatType, int ByteCount>
int templateFloatCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "[label]");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, ByteCount, bytes)) {
        setTclError(interp, "failed to read floating point bytes");
        return TCL_ERROR;
    }
    const std::vector<std::uint8_t> ordered = bytesInEndianOrder(bytes, context->bigEndian);
    FloatType number = 0;
    std::memcpy(&number, ordered.data(), sizeof(number));
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.12g", static_cast<double>(number));
    if (objc == 2) addTemplateRow(*context, tclString(objv[1]), buffer, context->position, ByteCount);
    context->position += ByteCount;
    Tcl_SetObjResult(interp, Tcl_NewDoubleObj(static_cast<double>(number)));
    return TCL_OK;
}

int templateUuidCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "[label]");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, 16, bytes)) {
        setTclError(interp, "failed to read UUID bytes");
        return TCL_ERROR;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer),
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    if (objc == 2) addTemplateRow(*context, tclString(objv[1]), buffer, context->position, 16);
    context->position += 16;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(buffer, -1));
    return TCL_OK;
}

template <int ByteCount>
int templateBitsCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "bits [label]");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, ByteCount, bytes)) {
        setTclError(interp, "failed to read bitfield bytes");
        return TCL_ERROR;
    }
    const std::uint64_t source = bytesToUnsigned(bytes, context->bigEndian);
    std::uint64_t value = 0;
    std::stringstream bitStream(tclString(objv[1]));
    std::string token;
    while (std::getline(bitStream, token, ',')) {
        if (token.empty()) continue;
        int bit = -1;
        const char* begin = token.data();
        const char* end = token.data() + token.size();
        const auto result = std::from_chars(begin, end, bit);
        if (result.ec != std::errc() || result.ptr != end || bit < 0 || bit >= ByteCount * 8) {
            setTclError(interp, "invalid bit index");
            return TCL_ERROR;
        }
        value = (value << 1) | ((source >> bit) & 1ULL);
    }
    const std::string label = objc == 3 ? tclString(objv[2]) : "";
    if (!label.empty()) addTemplateRow(*context, label, std::to_string(value), context->position, ByteCount);
    context->position += ByteCount;
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(value)));
    return TCL_OK;
}

template <bool Signed>
int templateLeb128Command(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "[label]");
        return TCL_ERROR;
    }

    const std::uint64_t available = context->document->length() > context->anchor + context->position
        ? context->document->length() - (context->anchor + context->position)
        : 0;
    const std::uint64_t bytesToRead = std::min<std::uint64_t>(8, available);
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, bytesToRead, bytes)) {
        setTclError(interp, "failed to read LEB128 bytes");
        return TCL_ERROR;
    }

    Leb128Value decoded;
    if (!decodeLeb128(bytes, Signed, decoded)) {
        setTclError(interp, "failed to read complete LEB128 value");
        return TCL_ERROR;
    }

    const std::string value = Signed ? std::to_string(decoded.signedValue) : std::to_string(decoded.unsignedValue);
    if constexpr (Signed) {
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(decoded.signedValue)));
    } else {
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(decoded.unsignedValue)));
    }
    if (objc == 2) addTemplateRow(*context, tclString(objv[1]), value, context->position, decoded.length);
    context->position += decoded.length;
    return TCL_OK;
}

int templateZlibUncompressCommand(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "data");
        return TCL_ERROR;
    }
    int inputLength = 0;
    const unsigned char* input = Tcl_GetByteArrayFromObj(objv[1], &inputLength);
    if (input == nullptr || inputLength < 0) return TCL_ERROR;

    uLongf outputLength = static_cast<uLongf>(std::max(1, inputLength * 5));
    std::vector<std::uint8_t> output(outputLength);
    for (int attempt = 0; attempt < 10; ++attempt) {
        uLongf actualLength = outputLength;
        const int result = uncompress(output.data(), &actualLength, input, static_cast<uLong>(inputLength));
        if (result == Z_OK) {
            Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(output.data(), static_cast<int>(actualLength)));
            return TCL_OK;
        }
        if (result != Z_BUF_ERROR) {
            setTclError(interp, "zlib decompression failed");
            return TCL_ERROR;
        }
        outputLength *= 2;
        output.resize(outputLength);
    }
    setTclError(interp, "zlib decompression output was too large");
    return TCL_ERROR;
}

template <int ByteCount>
int templateTimestampCommand(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    auto* context = static_cast<TemplateExecutionContext*>(data);
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "[label]");
        return TCL_ERROR;
    }
    std::vector<std::uint8_t> bytes;
    if (!readTemplateBytes(*context, context->position, ByteCount, bytes)) {
        setTclError(interp, "failed to read timestamp bytes");
        return TCL_ERROR;
    }
    const std::uint64_t value = bytesToUnsigned(bytes, context->bigEndian);
    if (objc == 2) addTemplateRow(*context, tclString(objv[1]), std::to_string(value), context->position, ByteCount);
    context->position += ByteCount;
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(value)));
    return TCL_OK;
}

TemplateRunResult runTemplate(hexfiend::linux_ui::EngineDocument& document,
                              const TemplateEntry& entry,
                              std::uint64_t anchor) {
    TemplateExecutionContext context;
    context.document = &document;
    context.anchor = std::min(anchor, document.length());
    context.interp = Tcl_CreateInterp();
    if (context.interp == nullptr) {
        context.result.error = "Unable to create Tcl interpreter.";
        return context.result;
    }
    if (Tcl_Init(context.interp) != TCL_OK) {
        context.result.error = Tcl_GetStringResult(context.interp);
        Tcl_DeleteInterp(context.interp);
        return context.result;
    }

    auto command = [&](const char* name, Tcl_ObjCmdProc* proc) {
        Tcl_CreateObjCommand(context.interp, name, proc, &context, nullptr);
    };
    command("big_endian", templateEndianCommand);
    command("little_endian", templateLittleEndianCommand);
    command("pos", templatePositionCommand);
    command("len", templateLengthCommand);
    command("end", templateEndCommand);
    command("move", templateMoveCommand);
    command("goto", templateGotoCommand);
    command("requires", templateRequiresCommand);
    command("include", templateIncludeCommand);
    command("hf_min_version_required", templateNoopCommand);
    command("section", templateSectionCommand);
    command("sectionvalue", templateSectionValueCommand);
    command("sectionname", templateSectionNameCommand);
    command("sectioncollapse", templateSectionCollapseCommand);
    command("endsection", templateEndSectionCommand);
    command("entry", templateEntryCommand);
    command("bytes", templateReadBytesCommand);
    command("hex", templateReadBytesCommand);
    command("ascii", templateAsciiCommand);
    command("str", templateStringCommand);
    command("cstr", templateCStringCommand);
    command("utf16", templateUtf16Command);
    command("uint8", templateIntegerCommand<1, false>);
    command("byte", templateIntegerCommand<1, false>);
    command("int8", templateIntegerCommand<1, true>);
    command("uint16", templateIntegerCommand<2, false>);
    command("int16", templateIntegerCommand<2, true>);
    command("uint24", templateIntegerCommand<3, false>);
    command("uint32", templateIntegerCommand<4, false>);
    command("int32", templateIntegerCommand<4, true>);
    command("uint64", templateIntegerCommand<8, false>);
    command("int64", templateIntegerCommand<8, true>);
    command("float", templateFloatCommand<float, 4>);
    command("double", templateFloatCommand<double, 8>);
    command("uuid", templateUuidCommand);
    command("uint8_bits", templateBitsCommand<1>);
    command("uint16_bits", templateBitsCommand<2>);
    command("uint32_bits", templateBitsCommand<4>);
    command("uint64_bits", templateBitsCommand<8>);
    command("uleb128", templateLeb128Command<false>);
    command("sleb128", templateLeb128Command<true>);
    command("zlib_uncompress", templateZlibUncompressCommand);
    command("macdate", templateTimestampCommand<4>);
    command("fatdate", templateTimestampCommand<2>);
    command("fattime", templateTimestampCommand<2>);
    command("unixtime32", templateTimestampCommand<4>);
    command("unixtime64", templateTimestampCommand<8>);

    const int result = Tcl_EvalFile(context.interp, entry.path.c_str());
    if (result != TCL_OK) context.result.error = Tcl_GetStringResult(context.interp);
    Tcl_DeleteInterp(context.interp);
    return context.result;
}

int hexNibble(ImWchar character) {
    if (character >= '0' && character <= '9') return static_cast<int>(character - '0');
    if (character >= 'a' && character <= 'f') return static_cast<int>(character - 'a' + 10);
    if (character >= 'A' && character <= 'F') return static_cast<int>(character - 'A' + 10);
    return -1;
}

void clampView(hexfiend::linux_ui::EngineDocument& document, ViewState& view, bool keepSelectionVisible = true) {
    const std::uint64_t length = document.length();
    const std::uint64_t visibleRows = std::max<std::uint64_t>(1, view.visibleRows);
    const std::uint64_t bytesPerPage = view.bytesPerRow * visibleRows;
    if (length == 0) {
        view.viewOffset = 0;
        view.selectionOffset = 0;
        view.selectionLength = 0;
        view.selectionAnchor = 0;
        view.additionalSelections.clear();
        view.multiSelectionBase.clear();
        view.multiSelectionInProgress = false;
        return;
    }
    const std::uint64_t maxOffset = length > bytesPerPage ? length - bytesPerPage : 0;
    view.viewOffset = std::min(view.viewOffset, maxOffset);
    view.selectionOffset = std::min(view.selectionOffset, length);
    view.selectionAnchor = std::min(view.selectionAnchor, length);
    view.selectionLength = std::min(view.selectionLength, length - view.selectionOffset);
    view.additionalSelections = normalizedRanges(view.additionalSelections);
    for (ByteRange& range : view.additionalSelections) {
        range.offset = std::min(range.offset, length);
        range.length = std::min(range.length, length - range.offset);
    }
    view.additionalSelections = normalizedRanges(view.additionalSelections);
    if (keepSelectionVisible &&
        (view.selectionOffset < view.viewOffset || view.selectionOffset >= view.viewOffset + bytesPerPage)) {
        view.viewOffset = std::min((view.selectionOffset / view.bytesPerRow) * view.bytesPerRow, maxOffset);
    }
}

bool openDocument(hexfiend::linux_ui::EngineDocument& document,
                  std::array<char, kPathBufferSize>& pathBuffer,
                  ViewState& view,
                  EditHistory& history,
                  std::string& status) {
    if (pathBuffer[0] == '\0') {
        status = "Enter a file path.";
        return false;
    }
    if (!document.open(pathBuffer.data())) {
        status = document.error();
        return false;
    }
    view.viewOffset = 0;
    view.selectionOffset = 0;
    view.selectionLength = 0;
    view.selectionAnchor = 0;
    view.additionalSelections.clear();
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
    clearPendingHexInput(view);
    history.clear();
    copyToBuffer(pathBuffer, document.path());
    status = "Opened " + document.path();
    return true;
}

bool newDocument(hexfiend::linux_ui::EngineDocument& document,
                 std::array<char, kPathBufferSize>& pathBuffer,
                 ViewState& view,
                 EditHistory& history,
                 std::string& status) {
    if (!document.createEmpty()) {
        status = document.error();
        return false;
    }
    pathBuffer.fill(0);
    view.viewOffset = 0;
    view.selectionOffset = 0;
    view.selectionLength = 0;
    view.selectionAnchor = 0;
    view.additionalSelections.clear();
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
    clearPendingHexInput(view);
    history.clear();
    status = "New document";
    return true;
}

void closeCurrentDocument(hexfiend::linux_ui::EngineDocument& document,
                          std::array<char, kPathBufferSize>& pathBuffer,
                          ViewState& view,
                          EditHistory& history,
                          std::string& status) {
    document.closeDocument();
    pathBuffer.fill(0);
    view.viewOffset = 0;
    view.selectionOffset = 0;
    view.selectionLength = 0;
    view.selectionAnchor = 0;
    view.additionalSelections.clear();
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
    clearPendingHexInput(view);
    history.clear();
    status = "Closed document.";
}

bool openDataDocument(hexfiend::linux_ui::EngineDocument& document,
                      const std::vector<std::uint8_t>& bytes,
                      std::array<char, kPathBufferSize>& pathBuffer,
                      ViewState& view,
                      EditHistory& history,
                      std::string& status) {
    if (!document.createWithBytes(bytes)) {
        status = document.error();
        return false;
    }
    pathBuffer.fill(0);
    view.viewOffset = 0;
    view.selectionOffset = 0;
    view.selectionLength = 0;
    view.selectionAnchor = 0;
    view.additionalSelections.clear();
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
    clearPendingHexInput(view);
    history.clear();
    status = "Opened " + std::to_string(bytes.size()) + " byte" + (bytes.size() == 1 ? "" : "s") + " from command line data.";
    return true;
}

bool revertDocument(hexfiend::linux_ui::EngineDocument& document,
                    std::array<char, kPathBufferSize>& pathBuffer,
                    ViewState& view,
                    EditHistory& history,
                    std::string& status) {
    if (!document.isOpen()) return false;
    if (document.path().empty()) {
        status = "Untitled documents cannot be reverted.";
        return false;
    }
    copyToBuffer(pathBuffer, document.path());
    if (!openDocument(document, pathBuffer, view, history, status)) return false;
    status = "Reverted to saved.";
    return true;
}

bool openProcessRegionSnapshot(hexfiend::linux_ui::EngineDocument& document,
                               std::array<char, kPathBufferSize>& pathBuffer,
                               ViewState& view,
                               EditHistory& history,
                               int pid,
                               const MemoryRegion& region,
                               std::string& status) {
    constexpr std::uint64_t kMaxProcessSnapshotBytes = 256ULL * 1024ULL * 1024ULL;
    const std::uint64_t length = region.end - region.start;
    if (length == 0 || length > kMaxProcessSnapshotBytes) {
        status = "Select a readable process region up to 256 MiB.";
        return false;
    }

    const std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    const int memFd = open(memPath.c_str(), O_RDONLY);
    if (memFd < 0) {
        status = "Unable to open process memory: " + std::string(std::strerror(errno));
        return false;
    }

    char pathTemplate[] = "/tmp/hexfiend-process-region-XXXXXX";
    const int outFd = mkstemp(pathTemplate);
    if (outFd < 0) {
        const std::string error = std::strerror(errno);
        close(memFd);
        status = "Unable to create process snapshot: " + error;
        return false;
    }

    constexpr std::size_t kChunkSize = 64 * 1024;
    std::vector<std::uint8_t> buffer(kChunkSize);
    std::uint64_t copied = 0;
    bool ok = true;
    std::string error;
    while (copied < length) {
        const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - copied));
        const ssize_t bytesRead = pread(memFd, buffer.data(), amount, static_cast<off_t>(region.start + copied));
        if (bytesRead <= 0) {
            ok = false;
            error = std::strerror(errno);
            break;
        }
        const ssize_t bytesWritten = write(outFd, buffer.data(), static_cast<std::size_t>(bytesRead));
        if (bytesWritten != bytesRead) {
            ok = false;
            error = std::strerror(errno);
            break;
        }
        copied += static_cast<std::uint64_t>(bytesRead);
    }
    close(memFd);
    close(outFd);

    if (!ok) {
        unlink(pathTemplate);
        status = "Unable to snapshot process region: " + error;
        return false;
    }

    copyToBuffer(pathBuffer, pathTemplate);
    if (!openDocument(document, pathBuffer, view, history, status)) {
        unlink(pathTemplate);
        return false;
    }
    view.editMode = EditMode::ReadOnly;
    status = "Opened process " + std::to_string(pid) + " snapshot 0x" + formatHex(region.start) + "-0x" + formatHex(region.end);
    return true;
}

bool jumpToOffset(hexfiend::linux_ui::EngineDocument& document,
                  ViewState& view,
                  const std::array<char, kTextBufferSize>& jumpOffsetBuffer,
                  std::string& status) {
    if (!document.isOpen()) {
        status = "No file is open.";
        return false;
    }
    if (document.length() == 0) {
        status = "Document is empty.";
        return false;
    }

    std::uint64_t offset = 0;
    if (!parseOffset(jumpOffsetBuffer.data(), offset)) {
        status = "Enter a decimal or hexadecimal offset.";
        return false;
    }
    if (offset > document.length()) {
        status = "Offset is past the end of the document.";
        return false;
    }

    view.selectionOffset = offset;
    view.selectionLength = offset == document.length() ? 0 : 1;
    view.selectionAnchor = offset;
    view.viewOffset = (offset / view.bytesPerRow) * view.bytesPerRow;
    view.additionalSelections.clear();
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
    clearPendingHexInput(view);
    status = "Jumped to offset 0x" + formatHex(offset);
    return true;
}

void showJumpToOffset(std::array<char, kTextBufferSize>& jumpOffsetBuffer,
                      const ViewState& view,
                      bool& showJumpDialog) {
    std::snprintf(jumpOffsetBuffer.data(), jumpOffsetBuffer.size(), "0x%s", formatHex(view.selectionOffset).c_str());
    showJumpDialog = true;
}

void showCompareWithFile(const ViewState& view,
                         std::array<char, kTextBufferSize>& compareStartBuffer,
                         std::array<char, kTextBufferSize>& compareLengthBuffer,
                         bool& compareUseRange,
                         bool& showCompareDialog) {
    const std::uint64_t length = view.selectionLength > 0 ? view.selectionLength : 1024;
    std::snprintf(compareStartBuffer.data(), compareStartBuffer.size(), "0x%s", formatHex(view.selectionOffset).c_str());
    std::snprintf(compareLengthBuffer.data(), compareLengthBuffer.size(), "%llu", static_cast<unsigned long long>(length));
    compareUseRange = view.selectionLength > 0;
    showCompareDialog = true;
}

bool drawFileBrowser(const char* id,
                     FileBrowserState& browser,
                     std::array<char, kPathBufferSize>& targetBuffer,
                     bool selectDirectories) {
    if (!browser.initialized) refreshFileBrowser(browser, browsingDirectoryForPath(targetBuffer.data()));

    bool activatedPath = false;
    ImGui::PushID(id);
    if (ImGui::Button("Up", ImVec2(70.0f, 0.0f))) {
        refreshFileBrowser(browser, parentDirectory(browser.directory));
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(90.0f, 0.0f))) {
        refreshFileBrowser(browser, browser.directory);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", browser.directory.c_str());
    if (!browser.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "%s", browser.error.c_str());

    if (ImGui::BeginTable("files", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(620.0f, 160.0f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();
        for (const FileBrowserEntry& entry : browser.entries) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected = std::strcmp(targetBuffer.data(), entry.path.c_str()) == 0;
            const std::string label = entry.isDirectory ? "[" + entry.name + "]" : entry.name;
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                const bool doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (entry.isDirectory && doubleClicked) {
                    refreshFileBrowser(browser, entry.path);
                } else {
                    copyToBuffer(targetBuffer, entry.path);
                    activatedPath = doubleClicked && (!entry.isDirectory || selectDirectories);
                }
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.isDirectory ? "Folder" : "File");
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return activatedPath;
}

bool handleFileBrowserAction(FileBrowserState& browser,
                             std::array<char, kPathBufferSize>& pathBuffer,
                             bool allowDirectories,
                             std::string& status) {
    if (directoryExists(pathBuffer.data())) {
        if (allowDirectories) return true;
        refreshFileBrowser(browser, pathBuffer.data());
        return false;
    }
    status.clear();
    return true;
}

std::string previewAt(hexfiend::linux_ui::EngineDocument& document,
                      std::uint64_t offset,
                      std::uint64_t length) {
    std::vector<std::uint8_t> bytes;
    if (length == 0 || !document.read(offset, static_cast<std::size_t>(std::min<std::uint64_t>(length, 16)), bytes)) return "-";
    return formatBytePreview(bytes);
}

std::string previewBytes(const std::vector<std::uint8_t>& bytes,
                         std::uint64_t offset,
                         std::uint64_t length) {
    if (length == 0 || offset >= bytes.size()) return "-";
    const std::uint64_t amount = std::min<std::uint64_t>(length, std::min<std::uint64_t>(16, bytes.size() - offset));
    using Difference = std::vector<std::uint8_t>::difference_type;
    return formatBytePreview(std::vector<std::uint8_t>(
        bytes.begin() + static_cast<Difference>(offset),
        bytes.begin() + static_cast<Difference>(offset + amount)));
}

void drawDiffPreviewPane(const char* label,
                         const std::string& path,
                         std::uint64_t offset,
                         std::uint64_t length,
                         const std::string& preview,
                         ImVec2 size) {
    ImGui::BeginChild(label, size, true);
    ImGui::TextUnformatted(path.c_str());
    ImGui::Text("0x%s, %s", formatHex(offset).c_str(), formatBytes(length).c_str());
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(preview.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

std::string diffRangeSummary(const DiffState& diff, const DiffRange& range, std::size_t index) {
    std::ostringstream output;
    output << "Difference " << (index + 1) << " of " << diff.ranges.size() << "\n"
           << "Left: " << diff.leftPath << "\n"
           << "  Offset: 0x" << formatHex(range.leftOffset) << "\n"
           << "  Length: " << range.leftLength << " bytes\n"
           << "  Preview: " << range.leftPreview << "\n"
           << "Right: " << diff.rightPath << "\n"
           << "  Offset: 0x" << formatHex(range.rightOffset) << "\n"
           << "  Length: " << range.rightLength << " bytes\n"
           << "  Preview: " << range.rightPreview << "\n";
    return output.str();
}

bool copySelectedDiffSummary(const DiffState& diff, std::string& status) {
    if (diff.ranges.empty()) {
        status = "No difference selected.";
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(diff.selectedRangeIndex, 0, static_cast<int>(diff.ranges.size() - 1)));
    const std::string summary = diffRangeSummary(diff, diff.ranges[index], index);
    if (SDL_SetClipboardText(summary.c_str()) != 0) {
        status = SDL_GetError();
        return false;
    }
    status = "Copied diff " + std::to_string(index + 1) + " summary.";
    return true;
}

bool readDiffRegion(hexfiend::linux_ui::EngineDocument& document,
                    std::uint64_t offset,
                    std::uint64_t length,
                    std::vector<std::uint8_t>& bytes) {
    bytes.clear();
    if (length == 0) return true;
    return document.read(offset, static_cast<std::size_t>(length), bytes);
}

bool computeCommonPrefix(hexfiend::linux_ui::EngineDocument& left,
                         hexfiend::linux_ui::EngineDocument& right,
                         std::uint64_t limit,
                         std::uint64_t& prefix) {
    constexpr std::uint64_t kChunkSize = 16 * 1024;
    prefix = 0;
    while (prefix < limit) {
        const std::uint64_t amount = std::min(kChunkSize, limit - prefix);
        std::vector<std::uint8_t> leftBytes;
        std::vector<std::uint8_t> rightBytes;
        if (!left.read(prefix, static_cast<std::size_t>(amount), leftBytes) ||
            !right.read(prefix, static_cast<std::size_t>(amount), rightBytes)) {
            return false;
        }
        for (std::uint64_t i = 0; i < amount; ++i) {
            if (leftBytes[static_cast<std::size_t>(i)] != rightBytes[static_cast<std::size_t>(i)]) return true;
            prefix++;
        }
    }
    return true;
}

bool computeCommonSuffix(hexfiend::linux_ui::EngineDocument& left,
                         hexfiend::linux_ui::EngineDocument& right,
                         std::uint64_t leftLength,
                         std::uint64_t rightLength,
                         std::uint64_t prefix,
                         std::uint64_t& suffix) {
    constexpr std::uint64_t kChunkSize = 16 * 1024;
    suffix = 0;
    const std::uint64_t limit = std::min(leftLength, rightLength) - std::min(prefix, std::min(leftLength, rightLength));
    while (suffix < limit) {
        const std::uint64_t amount = std::min(kChunkSize, limit - suffix);
        const std::uint64_t leftOffset = leftLength - suffix - amount;
        const std::uint64_t rightOffset = rightLength - suffix - amount;
        std::vector<std::uint8_t> leftBytes;
        std::vector<std::uint8_t> rightBytes;
        if (!left.read(leftOffset, static_cast<std::size_t>(amount), leftBytes) ||
            !right.read(rightOffset, static_cast<std::size_t>(amount), rightBytes)) {
            return false;
        }
        for (std::uint64_t i = 0; i < amount; ++i) {
            const std::uint64_t index = amount - i - 1;
            if (leftBytes[static_cast<std::size_t>(index)] != rightBytes[static_cast<std::size_t>(index)]) return true;
            suffix++;
        }
    }
    return true;
}

bool computeSmallEditDiffRanges(const std::vector<std::uint8_t>& leftBytes,
                                const std::vector<std::uint8_t>& rightBytes,
                                std::uint64_t leftBase,
                                std::uint64_t rightBase,
                                std::vector<DiffRange>& ranges,
                                std::size_t maxRanges) {
    const std::size_t leftSize = leftBytes.size();
    const std::size_t rightSize = rightBytes.size();
    constexpr std::size_t kMaxDiffMatrixCells = 16 * 1024 * 1024;
    if ((leftSize + 1) != 0 && (rightSize + 1) > kMaxDiffMatrixCells / (leftSize + 1)) return false;

    std::vector<std::uint32_t> lcs((leftSize + 1) * (rightSize + 1), 0);
    const auto cell = [&](std::size_t leftIndex, std::size_t rightIndex) -> std::uint32_t& {
        return lcs[leftIndex * (rightSize + 1) + rightIndex];
    };

    for (std::size_t i = leftSize; i-- > 0;) {
        for (std::size_t j = rightSize; j-- > 0;) {
            if (leftBytes[i] == rightBytes[j]) {
                cell(i, j) = cell(i + 1, j + 1) + 1;
            } else {
                cell(i, j) = std::max(cell(i + 1, j), cell(i, j + 1));
            }
        }
    }

    std::size_t i = 0;
    std::size_t j = 0;
    bool inRange = false;
    std::size_t rangeLeftStart = 0;
    std::size_t rangeRightStart = 0;
    const auto flushRange = [&]() {
        if (!inRange) return true;
        if (ranges.size() >= maxRanges) return false;
        const std::uint64_t leftLength = i - rangeLeftStart;
        const std::uint64_t rightLength = j - rangeRightStart;
        ranges.push_back(DiffRange{
            leftBase + rangeLeftStart,
            rightBase + rangeRightStart,
            leftLength,
            rightLength,
            previewBytes(leftBytes, rangeLeftStart, leftLength),
            previewBytes(rightBytes, rangeRightStart, rightLength),
        });
        inRange = false;
        return true;
    };

    while (i < leftSize || j < rightSize) {
        if (i < leftSize && j < rightSize && leftBytes[i] == rightBytes[j]) {
            if (!flushRange()) return false;
            ++i;
            ++j;
        } else {
            if (!inRange) {
                inRange = true;
                rangeLeftStart = i;
                rangeRightStart = j;
            }
            if (j >= rightSize || (i < leftSize && cell(i + 1, j) >= cell(i, j + 1))) {
                ++i;
            } else {
                ++j;
            }
        }
    }
    return flushRange();
}

void computeDirectDiffRanges(const std::vector<std::uint8_t>& leftBytes,
                             const std::vector<std::uint8_t>& rightBytes,
                             std::uint64_t leftBase,
                             std::uint64_t rightBase,
                             std::vector<DiffRange>& ranges,
                             bool& truncated,
                             std::size_t maxRanges) {
    const std::uint64_t sharedLength = std::min(leftBytes.size(), rightBytes.size());
    bool inRange = false;
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeLength = 0;

    const auto finishRange = [&]() {
        if (!inRange || truncated) return;
        ranges.push_back(DiffRange{
            leftBase + rangeStart,
            rightBase + rangeStart,
            rangeLength,
            rangeLength,
            previewBytes(leftBytes, rangeStart, rangeLength),
            previewBytes(rightBytes, rangeStart, rangeLength),
        });
        inRange = false;
        rangeLength = 0;
        truncated = ranges.size() >= maxRanges;
    };

    for (std::uint64_t offset = 0; offset < sharedLength && !truncated; ++offset) {
        if (leftBytes[static_cast<std::size_t>(offset)] != rightBytes[static_cast<std::size_t>(offset)]) {
            if (!inRange) {
                inRange = true;
                rangeStart = offset;
                rangeLength = 0;
            }
            rangeLength++;
        } else {
            finishRange();
        }
    }
    finishRange();

    if (!truncated && leftBytes.size() != rightBytes.size()) {
        ranges.push_back(DiffRange{
            leftBase + sharedLength,
            rightBase + sharedLength,
            leftBytes.size() - sharedLength,
            rightBytes.size() - sharedLength,
            previewBytes(leftBytes, sharedLength, leftBytes.size() - sharedLength),
            previewBytes(rightBytes, sharedLength, rightBytes.size() - sharedLength),
        });
        truncated = ranges.size() >= maxRanges;
    }
}

bool computeDiffFromBuffers(const std::vector<std::uint8_t>& leftBytes,
                            const std::vector<std::uint8_t>& rightBytes,
                            std::uint64_t leftBase,
                            std::uint64_t rightBase,
                            DiffState& result) {
    constexpr std::size_t kMaxDiffRanges = 4096;
    std::vector<DiffRange> editRanges;
    if (computeSmallEditDiffRanges(leftBytes, rightBytes, leftBase, rightBase, editRanges, kMaxDiffRanges)) {
        result.ranges.insert(result.ranges.end(), editRanges.begin(), editRanges.end());
        return true;
    }
    computeDirectDiffRanges(leftBytes, rightBytes, leftBase, rightBase, result.ranges, result.truncated, kMaxDiffRanges);
    return !result.truncated;
}

bool computeDiffRange(hexfiend::linux_ui::EngineDocument& left,
                      const std::array<char, kPathBufferSize>& rightPathBuffer,
                      std::uint64_t start,
                      std::uint64_t requestedLength,
                      DiffState& diff,
                      std::string& status) {
    if (!left.isOpen()) {
        status = "No file is open.";
        return false;
    }
    if (rightPathBuffer[0] == '\0') {
        status = "Enter a comparison file path.";
        return false;
    }
    if (requestedLength == 0) {
        status = "Range length must be greater than zero.";
        return false;
    }

    hexfiend::linux_ui::EngineDocument right;
    if (!right.open(rightPathBuffer.data())) {
        status = right.error();
        return false;
    }

    DiffState result;
    result.leftPath = left.path().empty() ? "Untitled" : left.path();
    result.rightPath = right.path();
    const std::uint64_t leftAvailable = start < left.length() ? std::min(requestedLength, left.length() - start) : 0;
    const std::uint64_t rightAvailable = start < right.length() ? std::min(requestedLength, right.length() - start) : 0;
    result.leftLength = leftAvailable;
    result.rightLength = rightAvailable;

    std::vector<std::uint8_t> leftBytes;
    std::vector<std::uint8_t> rightBytes;
    if (!readDiffRegion(left, start, leftAvailable, leftBytes) ||
        !readDiffRegion(right, start, rightAvailable, rightBytes)) {
        status = "Unable to read bytes for comparison.";
        return false;
    }

    computeDiffFromBuffers(leftBytes, rightBytes, start, start, result);
    const std::size_t count = result.ranges.size();
    status = count == 0 ? "Range is identical." : "Found " + std::to_string(count) + " differing range" + (count == 1 ? "" : "s");
    diff = std::move(result);
    return true;
}

bool computeDiff(hexfiend::linux_ui::EngineDocument& left,
                 const std::array<char, kPathBufferSize>& rightPathBuffer,
                 DiffState& diff,
                 std::string& status) {
    if (!left.isOpen()) {
        status = "No file is open.";
        return false;
    }
    if (rightPathBuffer[0] == '\0') {
        status = "Enter a comparison file path.";
        return false;
    }

    hexfiend::linux_ui::EngineDocument right;
    if (!right.open(rightPathBuffer.data())) {
        status = right.error();
        return false;
    }

    DiffState result;
    result.leftPath = left.path().empty() ? "Untitled" : left.path();
    result.rightPath = right.path();
    result.leftLength = left.length();
    result.rightLength = right.length();

    constexpr std::uint64_t kChunkSize = 16 * 1024;
    constexpr std::size_t kMaxDiffRanges = 4096;
    const std::uint64_t sharedLength = std::min(result.leftLength, result.rightLength);

    if (result.leftLength != result.rightLength) {
        std::uint64_t prefix = 0;
        std::uint64_t suffix = 0;
        if (!computeCommonPrefix(left, right, sharedLength, prefix) ||
            !computeCommonSuffix(left, right, result.leftLength, result.rightLength, prefix, suffix)) {
            status = "Unable to read bytes for comparison.";
            return false;
        }

        const std::uint64_t leftChangedLength = result.leftLength - prefix - suffix;
        const std::uint64_t rightChangedLength = result.rightLength - prefix - suffix;
        if (leftChangedLength > 0 || rightChangedLength > 0) {
            std::vector<std::uint8_t> leftChangedBytes;
            std::vector<std::uint8_t> rightChangedBytes;
            if (!readDiffRegion(left, prefix, leftChangedLength, leftChangedBytes) ||
                !readDiffRegion(right, prefix, rightChangedLength, rightChangedBytes)) {
                status = "Unable to read bytes for comparison.";
                return false;
            }
            std::vector<DiffRange> editRanges;
            if (computeSmallEditDiffRanges(leftChangedBytes, rightChangedBytes, prefix, prefix, editRanges, kMaxDiffRanges)) {
                result.ranges.insert(result.ranges.end(), editRanges.begin(), editRanges.end());
            } else {
                result.ranges.push_back(DiffRange{
                    prefix,
                    prefix,
                    leftChangedLength,
                    rightChangedLength,
                    previewBytes(leftChangedBytes, 0, leftChangedLength),
                    previewBytes(rightChangedBytes, 0, rightChangedLength),
                });
                result.truncated = result.ranges.size() >= kMaxDiffRanges;
            }
        }
        const std::size_t count = result.ranges.size();
        status = count == 0 ? "Files are identical." : "Found " + std::to_string(count) + " differing range" + (count == 1 ? "" : "s");
        diff = std::move(result);
        return true;
    }

    bool inRange = false;
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeLength = 0;

    auto finishRange = [&]() {
        if (!inRange) return;
        result.ranges.push_back(DiffRange{
            rangeStart,
            rangeStart,
            rangeLength,
            rangeLength,
            previewAt(left, rangeStart, rangeLength),
            previewAt(right, rangeStart, rangeLength),
        });
        inRange = false;
        rangeLength = 0;
        if (result.ranges.size() >= kMaxDiffRanges) result.truncated = true;
    };

    for (std::uint64_t offset = 0; offset < sharedLength && !result.truncated; offset += kChunkSize) {
        const std::uint64_t amount = std::min(kChunkSize, sharedLength - offset);
        std::vector<std::uint8_t> leftBytes;
        std::vector<std::uint8_t> rightBytes;
        if (!left.read(offset, static_cast<std::size_t>(amount), leftBytes) ||
            !right.read(offset, static_cast<std::size_t>(amount), rightBytes)) {
            status = "Unable to read bytes for comparison.";
            return false;
        }
        for (std::uint64_t i = 0; i < amount; ++i) {
            if (leftBytes[static_cast<std::size_t>(i)] != rightBytes[static_cast<std::size_t>(i)]) {
                if (!inRange) {
                    inRange = true;
                    rangeStart = offset + i;
                    rangeLength = 0;
                }
                rangeLength++;
            } else {
                finishRange();
                if (result.truncated) break;
            }
        }
    }
    finishRange();

    if (!result.truncated && result.leftLength != result.rightLength) {
        const std::uint64_t offset = sharedLength;
        const std::uint64_t leftExtra = result.leftLength - sharedLength;
        const std::uint64_t rightExtra = result.rightLength - sharedLength;
        result.ranges.push_back(DiffRange{
            offset,
            offset,
            leftExtra,
            rightExtra,
            previewAt(left, offset, leftExtra),
            previewAt(right, offset, rightExtra),
        });
    }

    const std::size_t count = result.ranges.size();
    status = count == 0 ? "Files are identical." : "Found " + std::to_string(count) + " differing range" + (count == 1 ? "" : "s");
    diff = std::move(result);
    return true;
}

bool openFilesForComparison(hexfiend::linux_ui::EngineDocument& document,
                            std::array<char, kPathBufferSize>& pathBuffer,
                            const std::array<char, kPathBufferSize>& leftPathBuffer,
                            const std::array<char, kPathBufferSize>& rightPathBuffer,
                            ViewState& view,
                            EditHistory& history,
                            DiffState& diff,
                            EditMode defaultEditMode,
                            std::string& status) {
    if (leftPathBuffer[0] == '\0') {
        status = "Enter a left comparison file path.";
        return false;
    }
    if (rightPathBuffer[0] == '\0') {
        status = "Enter a right comparison file path.";
        return false;
    }

    hexfiend::linux_ui::EngineDocument left;
    if (!left.open(leftPathBuffer.data())) {
        status = left.error();
        return false;
    }

    DiffState result;
    std::string compareStatus;
    if (!computeDiff(left, rightPathBuffer, result, compareStatus)) {
        status = compareStatus;
        return false;
    }

    copyToBuffer(pathBuffer, leftPathBuffer.data());
    if (!openDocument(document, pathBuffer, view, history, status)) return false;
    view.editMode = defaultEditMode;
    diff = std::move(result);
    status = compareStatus;
    return true;
}

void saveDocument(hexfiend::linux_ui::EngineDocument& document,
                  bool& showSaveAsDialog,
                  std::string& status) {
    if (!document.isOpen()) return;
    if (document.path().empty()) {
        showSaveAsDialog = true;
        status = "Choose a save path.";
        return;
    }
    status = document.save() ? "Saved " + document.path() : document.error();
}

ByteRange primarySelectionRange(const ViewState& view) {
    return ByteRange{view.selectionOffset, view.selectionLength};
}

std::vector<ByteRange> normalizedRanges(std::vector<ByteRange> ranges) {
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [](const ByteRange& range) {
        return range.length == 0;
    }), ranges.end());
    std::sort(ranges.begin(), ranges.end(), [](const ByteRange& left, const ByteRange& right) {
        return left.offset < right.offset;
    });

    std::vector<ByteRange> result;
    for (const ByteRange& range : ranges) {
        if (result.empty()) {
            result.push_back(range);
            continue;
        }
        ByteRange& previous = result.back();
        const std::uint64_t previousEnd = previous.offset + previous.length;
        if (range.offset <= previousEnd) {
            previous.length = std::max(previousEnd, range.offset + range.length) - previous.offset;
        } else {
            result.push_back(range);
        }
    }
    return result;
}

std::vector<ByteRange> selectedRanges(const ViewState& view) {
    std::vector<ByteRange> ranges = view.additionalSelections;
    ranges.push_back(primarySelectionRange(view));
    return normalizedRanges(std::move(ranges));
}

std::string statusSelectionSummary(hexfiend::linux_ui::EngineDocument& document, const ViewState& view) {
    const std::vector<ByteRange> ranges = selectedRanges(view);
    if (view.selectionLength == 0 && ranges.empty()) {
        return "Insertion point at offset 0x" + formatHex(view.selectionOffset) + " out of " + formatBytes(document.length());
    }
    if (ranges.size() > 1) {
        std::uint64_t selectedBytes = 0;
        for (const ByteRange& range : ranges) selectedBytes += range.length;
        return std::to_string(selectedBytes) + " bytes selected in " + std::to_string(ranges.size()) +
            " ranges out of " + formatBytes(document.length());
    }

    const std::uint64_t selectedLength = ranges.empty() ? view.selectionLength : ranges.front().length;
    const std::uint64_t selectedOffset = ranges.empty() ? view.selectionOffset : ranges.front().offset;
    return std::to_string(selectedLength) + " byte" + (selectedLength == 1 ? "" : "s") +
        " selected at offset 0x" + formatHex(selectedOffset) + " out of " + formatBytes(document.length());
}

void clearAdditionalSelections(ViewState& view) {
    view.additionalSelections.clear();
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
}

void clearPendingHexInput(ViewState& view) {
    view.pendingHexNibble = -1;
    view.pendingHexOffset = 0;
}

void setAdditionalSelectionsFromRanges(ViewState& view, const std::vector<ByteRange>& ranges) {
    view.additionalSelections.clear();
    const ByteRange primary = primarySelectionRange(view);
    for (const ByteRange& range : normalizedRanges(ranges)) {
        if (range.length == 0) continue;
        if (range.offset == primary.offset && range.length == primary.length) continue;
        view.additionalSelections.push_back(range);
    }
}

void selectByte(hexfiend::linux_ui::EngineDocument& document,
                ViewState& view,
                std::uint64_t offset,
                bool extend) {
    if (!document.isOpen() || offset >= document.length()) return;
    clearPendingHexInput(view);
    if (!extend) clearAdditionalSelections(view);
    if (!extend) view.selectionAnchor = offset;
    const std::uint64_t anchor = extend ? std::min(view.selectionAnchor, document.length() - 1) : offset;
    const std::uint64_t start = std::min(anchor, offset);
    const std::uint64_t end = std::max(anchor, offset);
    view.selectionOffset = start;
    view.selectionLength = end - start + 1;
}

void selectCharacter(hexfiend::linux_ui::EngineDocument& document,
                     ViewState& view,
                     std::uint64_t offset,
                     bool extend) {
    if (!document.isOpen()) return;
    offset = std::min(offset, document.length());
    clearPendingHexInput(view);
    if (!extend) clearAdditionalSelections(view);
    if (!extend) view.selectionAnchor = offset;
    const std::uint64_t anchor = extend ? std::min(view.selectionAnchor, document.length()) : offset;
    view.selectionOffset = std::min(anchor, offset);
    view.selectionLength = std::max(anchor, offset) - view.selectionOffset;
}

bool selectDiffRange(hexfiend::linux_ui::EngineDocument& document,
                     ViewState& view,
                     DiffState& diff,
                     int index,
                     std::string& status) {
    if (!document.isOpen() || diff.ranges.empty()) return false;
    index = std::clamp(index, 0, static_cast<int>(diff.ranges.size() - 1));
    const DiffRange& range = diff.ranges[static_cast<std::size_t>(index)];
    const std::uint64_t offset = std::min(range.leftOffset, document.length());
    if (range.leftLength > 0) {
        selectCharacter(document, view, offset, false);
        selectCharacter(document, view, std::min(document.length(), offset + range.leftLength), true);
    } else {
        selectCharacter(document, view, offset, false);
    }
    view.viewOffset = (offset / view.bytesPerRow) * view.bytesPerRow;
    diff.selectedRangeIndex = index;
    status = "Selected diff " + std::to_string(index + 1) + " of " + std::to_string(diff.ranges.size()) +
        " at offset 0x" + formatHex(offset);
    return true;
}

bool stepDiffRange(hexfiend::linux_ui::EngineDocument& document,
                   ViewState& view,
                   DiffState& diff,
                   int direction,
                   std::string& status) {
    if (diff.ranges.empty()) return false;
    const int count = static_cast<int>(diff.ranges.size());
    const int next = (std::clamp(diff.selectedRangeIndex, 0, count - 1) + direction + count) % count;
    return selectDiffRange(document, view, diff, next, status);
}

bool findAndSelect(hexfiend::linux_ui::EngineDocument& document,
                   ViewState& view,
                   const std::vector<std::uint8_t>& pattern,
                   bool forwards,
                   std::string& status) {
    if (!document.isOpen()) return false;
    std::uint64_t found = 0;
    const std::uint64_t start = forwards
        ? std::min(document.length(), view.selectionOffset + std::max<std::uint64_t>(view.selectionLength, 1))
        : (view.selectionOffset > 0 ? view.selectionOffset : document.length());
    bool wrapped = false;
    if (!document.find(pattern, start, forwards, found)) {
        const std::uint64_t wrapStart = forwards ? 0 : document.length();
        if (!document.find(pattern, wrapStart, forwards, found)) {
            status = document.error();
            return false;
        }
        wrapped = true;
    }
    selectCharacter(document, view, found, false);
    selectCharacter(document, view, std::min(document.length(), found + pattern.size()), true);
    view.viewOffset = (found / view.bytesPerRow) * view.bytesPerRow;
    status = std::string(wrapped ? "Wrapped to offset 0x" : "Found at offset 0x") + formatHex(found);
    return true;
}

void extendAdditionalSelection(hexfiend::linux_ui::EngineDocument& document,
                               ViewState& view,
                               std::uint64_t offset,
                               bool begin) {
    if (!document.isOpen()) return;
    offset = std::min(offset, document.length());
    clearPendingHexInput(view);
    if (begin || !view.multiSelectionInProgress) {
        view.multiSelectionInProgress = true;
        view.multiSelectionAnchor = offset;
        view.multiSelectionBase = selectedRanges(view);
    }

    const std::uint64_t start = std::min(view.multiSelectionAnchor, offset);
    const std::uint64_t length = std::max(view.multiSelectionAnchor, offset) - start;
    std::vector<ByteRange> ranges = view.multiSelectionBase;
    if (length > 0) ranges.push_back(ByteRange{start, length});
    ranges = normalizedRanges(std::move(ranges));
    if (!ranges.empty()) {
        view.selectionOffset = ranges.back().offset;
        view.selectionLength = ranges.back().length;
        view.selectionAnchor = view.multiSelectionAnchor;
    } else {
        view.selectionOffset = offset;
        view.selectionLength = 0;
        view.selectionAnchor = offset;
    }
    setAdditionalSelectionsFromRanges(view, ranges);
}

SelectionSnapshot captureSelection(const ViewState& view) {
    return SelectionSnapshot{view.selectionOffset, view.selectionLength, view.selectionAnchor, view.viewOffset, view.additionalSelections};
}

void restoreSelection(hexfiend::linux_ui::EngineDocument& document, ViewState& view, const SelectionSnapshot& selection) {
    view.selectionOffset = selection.offset;
    view.selectionLength = selection.length;
    view.selectionAnchor = selection.anchor;
    view.viewOffset = selection.viewOffset;
    view.additionalSelections = selection.additionalSelections;
    view.multiSelectionBase.clear();
    view.multiSelectionInProgress = false;
    clearPendingHexInput(view);
    clampView(document, view);
}

void finishEditSelection(hexfiend::linux_ui::EngineDocument& document,
                         ViewState& view,
                         std::uint64_t offset,
                         std::size_t insertedLength) {
    clearAdditionalSelections(view);
    if (document.length() == 0) {
        view.selectionOffset = 0;
        view.selectionLength = 0;
        view.selectionAnchor = 0;
    } else if (insertedLength == 0) {
        view.selectionOffset = std::min<std::uint64_t>(offset, document.length());
        view.selectionLength = 0;
        view.selectionAnchor = view.selectionOffset;
    } else {
        view.selectionOffset = std::min<std::uint64_t>(offset + insertedLength, document.length());
        view.selectionLength = 0;
        view.selectionAnchor = view.selectionOffset;
    }
    clearPendingHexInput(view);
    clampView(document, view);
}

bool readOldBytes(hexfiend::linux_ui::EngineDocument& document,
                  std::uint64_t offset,
                  std::uint64_t length,
                  std::vector<std::uint8_t>& bytes,
                  std::string& status) {
    bytes.clear();
    if (length == 0) return true;
    if (!document.read(offset, static_cast<std::size_t>(length), bytes)) {
        status = document.error();
        return false;
    }
    return true;
}

bool applyReplacement(hexfiend::linux_ui::EngineDocument& document,
                      ViewState& view,
                      EditHistory* history,
                      std::uint64_t offset,
                      std::uint64_t replaceLength,
                      const std::vector<std::uint8_t>& bytes,
                      const std::string& label,
                      std::string& status) {
    std::vector<std::uint8_t> oldBytes;
    if (!readOldBytes(document, offset, replaceLength, oldBytes, status)) return false;
    const SelectionSnapshot before = captureSelection(view);

    if (!document.replace(offset, replaceLength, bytes)) {
        status = document.error();
        return false;
    }

    finishEditSelection(document, view, offset, bytes.size());

    if (history != nullptr) {
        EditTransaction transaction;
        transaction.label = label;
        transaction.before = before;
        transaction.after = captureSelection(view);
        transaction.operations.push_back(EditOperation{offset, oldBytes, bytes});
        history->undo.push_back(std::move(transaction));
        history->redo.clear();
    }
    return true;
}

bool deleteSelectedRanges(hexfiend::linux_ui::EngineDocument& document,
                          ViewState& view,
                          EditHistory& history,
                          std::string& status) {
    clearPendingHexInput(view);
    std::vector<ByteRange> ranges = selectedRanges(view);
    if (ranges.empty()) return false;

    EditTransaction transaction;
    transaction.label = "Delete";
    transaction.before = captureSelection(view);

    std::uint64_t deleted = 0;
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        std::vector<std::uint8_t> oldBytes;
        if (!readOldBytes(document, it->offset, it->length, oldBytes, status)) return false;
        if (!document.replace(it->offset, it->length, {})) {
            status = document.error();
            return false;
        }
        transaction.operations.push_back(EditOperation{it->offset, oldBytes, {}});
        deleted += it->length;
    }

    const std::uint64_t caret = std::min(ranges.front().offset, document.length());
    view.selectionOffset = caret;
    view.selectionLength = 0;
    view.selectionAnchor = caret;
    clearAdditionalSelections(view);
    clampView(document, view);

    transaction.after = captureSelection(view);
    history.undo.push_back(std::move(transaction));
    history.redo.clear();
    status = "Deleted " + std::to_string(deleted) + " byte" + (deleted == 1 ? "" : "s");
    return true;
}

void replaceSelection(hexfiend::linux_ui::EngineDocument& document,
                      ViewState& view,
                      EditHistory& history,
                      const std::vector<std::uint8_t>& bytes,
                      std::string& status) {
    if (!document.isOpen()) {
        status = "No file is open.";
        return;
    }
    if (view.editMode == EditMode::ReadOnly) {
        status = "Read-only mode.";
        return;
    }
    if (!view.additionalSelections.empty()) {
        if (!deleteSelectedRanges(document, view, history, status)) return;
    }
    const std::uint64_t documentLength = document.length();
    const std::uint64_t replaceLength = view.selectionLength > 0
        ? view.selectionLength
        : (view.editMode == EditMode::Overwrite && view.selectionOffset < documentLength
            ? std::min<std::uint64_t>(1, documentLength - view.selectionOffset)
            : 0);
    if (applyReplacement(document, view, &history, view.selectionOffset, replaceLength, bytes, "Edit", status)) {
        status = "Edited at offset 0x" + formatHex(view.selectionOffset);
    }
}

bool typeByte(hexfiend::linux_ui::EngineDocument& document,
              ViewState& view,
              EditHistory& history,
              std::uint8_t byte,
              std::string& status) {
    if (!document.isOpen()) {
        status = "No file is open.";
        return false;
    }
    if (view.editMode == EditMode::ReadOnly) {
        status = "Read-only mode.";
        return false;
    }
    if (!view.additionalSelections.empty()) {
        if (!deleteSelectedRanges(document, view, history, status)) return false;
    }

    const std::uint64_t documentLength = document.length();
    const std::uint64_t replaceLength = view.selectionLength > 0
        ? view.selectionLength
        : (view.editMode == EditMode::Overwrite && view.selectionOffset < documentLength ? 1 : 0);
    if (applyReplacement(document, view, &history, view.selectionOffset, replaceLength, {byte}, "Typing", status)) {
        status = "Inserted at offset 0x" + formatHex(view.selectionOffset);
        return true;
    }
    return false;
}

bool typeHexNibble(hexfiend::linux_ui::EngineDocument& document,
                   ViewState& view,
                   EditHistory& history,
                   int nibble,
                   std::string& status) {
    if (nibble < 0 || nibble > 0xF) return false;

    if (view.pendingHexNibble >= 0 && view.pendingHexOffset < document.length()) {
        const std::uint8_t byte = static_cast<std::uint8_t>((view.pendingHexNibble << 4) | nibble);
        const std::uint64_t offset = view.pendingHexOffset;
        if (!document.replace(offset, 1, {byte})) {
            status = document.error();
            clearPendingHexInput(view);
            return false;
        }
        if (!history.undo.empty() && !history.undo.back().operations.empty()) {
            EditTransaction& transaction = history.undo.back();
            EditOperation& operation = transaction.operations.back();
            if (operation.offset == offset && operation.newBytes.size() == 1) {
                operation.newBytes[0] = byte;
                view.selectionOffset = std::min<std::uint64_t>(offset + 1, document.length());
                view.selectionLength = 0;
                view.selectionAnchor = view.selectionOffset;
                clearAdditionalSelections(view);
                transaction.after = captureSelection(view);
            }
        }
        clearPendingHexInput(view);
        clampView(document, view);
        status = "Inserted byte 0x" + formatHex(byte, 2);
        return true;
    }

    const std::uint64_t offset = view.selectionOffset;
    const std::uint8_t byte = static_cast<std::uint8_t>(nibble << 4);
    if (!typeByte(document, view, history, byte, status)) return false;
    view.pendingHexNibble = nibble;
    view.pendingHexOffset = std::min(offset, document.length() == 0 ? 0 : document.length() - 1);
    status = "Entered first hex digit.";
    return true;
}

bool replaceCurrentMatch(hexfiend::linux_ui::EngineDocument& document,
                         ViewState& view,
                         EditHistory& history,
                         const std::vector<std::uint8_t>& pattern,
                         const std::vector<std::uint8_t>& replacement,
                         std::string& status) {
    if (!document.isOpen()) {
        status = "No file is open.";
        return false;
    }
    if (view.editMode == EditMode::ReadOnly) {
        status = "Read-only mode.";
        return false;
    }

    std::uint64_t replaceOffset = view.selectionOffset;
    if (view.selectionLength != pattern.size()) {
        if (!document.find(pattern, view.selectionOffset, true, replaceOffset)) {
            status = document.error();
            return false;
        }
    } else {
        std::vector<std::uint8_t> selectedBytes;
        if (!document.read(view.selectionOffset, pattern.size(), selectedBytes) || selectedBytes != pattern) {
            if (!document.find(pattern, view.selectionOffset, true, replaceOffset)) {
                status = document.error();
                return false;
            }
        }
    }

    if (!applyReplacement(document, view, &history, replaceOffset, pattern.size(), replacement, "Replace", status)) return false;
    view.selectionOffset = replaceOffset;
    view.selectionLength = replacement.empty() ? 0 : replacement.size();
    view.selectionAnchor = replaceOffset;
    clearPendingHexInput(view);
    if (!history.undo.empty()) history.undo.back().after = captureSelection(view);
    status = "Replaced at offset 0x" + formatHex(replaceOffset);
    return true;
}

std::uint64_t replaceAllMatches(hexfiend::linux_ui::EngineDocument& document,
                                ViewState& view,
                                EditHistory& history,
                                const std::vector<std::uint8_t>& pattern,
                                const std::vector<std::uint8_t>& replacement,
                                std::string& status) {
    if (!document.isOpen()) {
        status = "No file is open.";
        return 0;
    }
    if (view.editMode == EditMode::ReadOnly) {
        status = "Read-only mode.";
        return 0;
    }

    std::uint64_t count = 0;
    std::uint64_t searchOffset = 0;
    EditTransaction transaction;
    transaction.label = "Replace All";
    transaction.before = captureSelection(view);
    while (searchOffset <= document.length()) {
        std::uint64_t found = 0;
        if (!document.find(pattern, searchOffset, true, found)) break;

        std::vector<std::uint8_t> oldBytes;
        if (!readOldBytes(document, found, pattern.size(), oldBytes, status)) return count;
        if (!document.replace(found, pattern.size(), replacement)) {
            status = document.error();
            return count;
        }
        transaction.operations.push_back(EditOperation{found, oldBytes, replacement});
        count++;
        searchOffset = found + std::max<std::uint64_t>(replacement.size(), 1);
    }

    if (count == 0) {
        status = "Not found";
    } else {
        view.selectionOffset = std::min<std::uint64_t>(searchOffset, document.length() ? document.length() - 1 : 0);
        view.selectionLength = replacement.empty() ? 0 : replacement.size();
        view.selectionAnchor = view.selectionOffset;
        clearPendingHexInput(view);
        transaction.after = captureSelection(view);
        history.undo.push_back(std::move(transaction));
        history.redo.clear();
        status = "Replaced " + std::to_string(count) + " occurrence" + (count == 1 ? "" : "s");
    }
    return count;
}

void deleteSelection(hexfiend::linux_ui::EngineDocument& document, ViewState& view, EditHistory& history, bool backwards, std::string& status) {
    if (!document.isOpen() || document.length() == 0) return;
    clearPendingHexInput(view);
    if (view.editMode == EditMode::ReadOnly) {
        status = "Read-only mode.";
        return;
    }
    if (!view.additionalSelections.empty() && !selectedRanges(view).empty()) {
        deleteSelectedRanges(document, view, history, status);
        return;
    }
    std::uint64_t offset = view.selectionOffset;
    std::uint64_t amount = view.selectionLength;
    if (amount == 0) {
        if (backwards) {
            if (offset == 0) return;
            offset -= 1;
        }
        amount = 1;
    }
    if (applyReplacement(document, view, &history, offset, amount, {}, "Delete", status)) {
        status = "Deleted " + std::to_string(amount) + " byte" + (amount == 1 ? "" : "s");
    }
}

std::uint64_t selectionFocusOffset(const ViewState& view) {
    if (view.selectionLength == 0) return view.selectionOffset;
    const std::uint64_t end = view.selectionOffset + view.selectionLength;
    return view.selectionAnchor == view.selectionOffset ? end : view.selectionOffset;
}

void handleEditorKeys(hexfiend::linux_ui::EngineDocument& document, ViewState& view, EditHistory& history, std::string& status) {
    if (!document.isOpen() || ImGui::GetIO().KeyCtrl || ImGui::IsAnyItemActive()) return;

    const bool extendSelection = ImGui::GetIO().KeyShift;
    const std::uint64_t focusOffset = selectionFocusOffset(view);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (!extendSelection && view.selectionLength > 0) {
            selectCharacter(document, view, view.selectionOffset, false);
        } else if (focusOffset > 0) {
            selectCharacter(document, view, focusOffset - 1, extendSelection);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (!extendSelection && view.selectionLength > 0) {
            selectCharacter(document, view, view.selectionOffset + view.selectionLength, false);
        } else if (focusOffset < document.length()) {
            selectCharacter(document, view, focusOffset + 1, extendSelection);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        selectCharacter(document, view, (focusOffset / view.bytesPerRow) * view.bytesPerRow, extendSelection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        const std::uint64_t lineEnd = std::min(document.length(), ((focusOffset / view.bytesPerRow) + 1) * view.bytesPerRow);
        selectCharacter(document, view, lineEnd, extendSelection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && focusOffset >= view.bytesPerRow) {
        selectCharacter(document, view, focusOffset - view.bytesPerRow, extendSelection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && focusOffset + view.bytesPerRow <= document.length()) {
        selectCharacter(document, view, focusOffset + view.bytesPerRow, extendSelection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        const std::uint64_t visibleRows = std::max<std::uint64_t>(1, view.visibleRows);
        const std::uint64_t pageBytes = view.bytesPerRow * (visibleRows > 1 ? visibleRows - 1 : 1);
        selectCharacter(document, view, focusOffset > pageBytes ? focusOffset - pageBytes : 0, extendSelection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        const std::uint64_t visibleRows = std::max<std::uint64_t>(1, view.visibleRows);
        const std::uint64_t pageBytes = view.bytesPerRow * (visibleRows > 1 ? visibleRows - 1 : 1);
        selectCharacter(document, view, std::min(document.length(), focusOffset + pageBytes), extendSelection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        deleteSelection(document, view, history, false, status);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        deleteSelection(document, view, history, true, status);
    }

    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar character = io.InputQueueCharacters[i];
        if (view.activePane == EditorPane::Hex) {
            const int nibble = hexNibble(character);
            if (nibble < 0) continue;
            typeHexNibble(document, view, history, nibble, status);
        } else {
            clearPendingHexInput(view);
            if (character >= 0x20 && character <= 0x7E) {
                typeByte(document, view, history, static_cast<std::uint8_t>(character), status);
            }
        }
    }
}

bool selectedContains(const ViewState& view, std::uint64_t offset) {
    for (const ByteRange& range : selectedRanges(view)) {
        if (offset >= range.offset && offset < range.offset + range.length) return true;
    }
    return false;
}

bool readSelection(hexfiend::linux_ui::EngineDocument& document,
                   const ViewState& view,
                   std::vector<std::uint8_t>& bytes,
                   std::string& status) {
    if (!document.isOpen() || document.length() == 0) {
        status = "No bytes selected.";
        return false;
    }
    const std::vector<ByteRange> ranges = selectedRanges(view);
    if (ranges.empty()) {
        status = "No bytes selected.";
        return false;
    }

    bytes.clear();
    for (const ByteRange& range : ranges) {
        const std::uint64_t offset = std::min(range.offset, document.length() - 1);
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(range.length, document.length() - offset));
        std::vector<std::uint8_t> rangeBytes;
        if (!document.read(offset, length, rangeBytes)) {
            status = document.error();
            return false;
        }
        bytes.insert(bytes.end(), rangeBytes.begin(), rangeBytes.end());
    }
    return true;
}

bool copySelection(hexfiend::linux_ui::EngineDocument& document,
                   const ViewState& view,
                   bool asAscii,
                   std::string& status) {
    std::vector<std::uint8_t> bytes;
    if (!readSelection(document, view, bytes, status)) return false;

    std::string text;
    if (asAscii) {
        text.assign(bytes.begin(), bytes.end());
    } else {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) text.push_back(' ');
            text += formatHex(bytes[i], 2);
        }
    }
    if (SDL_SetClipboardText(text.c_str()) != 0) {
        status = SDL_GetError();
        return false;
    }
    status = "Copied " + std::to_string(bytes.size()) + " byte" + (bytes.size() == 1 ? "" : "s");
    return true;
}

bool cutSelection(hexfiend::linux_ui::EngineDocument& document,
                  ViewState& view,
                  EditHistory& history,
                  bool asAscii,
                  std::string& status) {
    if (view.editMode == EditMode::ReadOnly) {
        status = "Read-only mode.";
        return false;
    }
    if (!copySelection(document, view, asAscii, status)) return false;
    deleteSelection(document, view, history, false, status);
    return true;
}

void selectAll(hexfiend::linux_ui::EngineDocument& document, ViewState& view, std::string& status) {
    if (!document.isOpen() || document.length() == 0) {
        status = "No bytes selected.";
        return;
    }
    view.selectionOffset = 0;
    view.selectionLength = document.length();
    view.selectionAnchor = 0;
    view.viewOffset = 0;
    clearAdditionalSelections(view);
    clearPendingHexInput(view);
    status = "Selected all.";
}

bool buildPasteBytes(std::string_view text,
                     PasteMode mode,
                     std::vector<std::uint8_t>& bytes,
                     std::string& status) {
    bytes.clear();
    if (mode == PasteMode::Text) {
        bytes.assign(text.begin(), text.end());
    } else if (!parseHexPattern(text, bytes)) {
        if (mode == PasteMode::Hex) {
            status = "Clipboard does not contain complete hexadecimal bytes.";
            return false;
        }
        bytes.assign(text.begin(), text.end());
    }
    if (bytes.empty()) {
        status = "Clipboard is empty.";
        return false;
    }
    return true;
}

bool pasteClipboard(hexfiend::linux_ui::EngineDocument& document,
                    ViewState& view,
                    EditHistory& history,
                    PasteMode mode,
                    std::string& status) {
    char* clipboard = SDL_GetClipboardText();
    if (clipboard == nullptr) {
        status = SDL_GetError();
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const bool built = buildPasteBytes(std::string_view(clipboard, std::strlen(clipboard)), mode, bytes, status);
    SDL_free(clipboard);
    if (!built) return false;
    replaceSelection(document, view, history, bytes, status);
    return true;
}

PasteMode defaultPasteMode(const ViewState& view) {
    return view.activePane == EditorPane::Ascii ? PasteMode::Text : PasteMode::Auto;
}

bool applyHistoryTransaction(hexfiend::linux_ui::EngineDocument& document,
                             ViewState& view,
                             EditTransaction& transaction,
                             bool undoing,
                             std::string& status) {
    if (!document.isOpen()) {
        status = "No file is open.";
        return false;
    }

    if (undoing) {
        for (auto it = transaction.operations.rbegin(); it != transaction.operations.rend(); ++it) {
            if (!document.replace(it->offset, it->newBytes.size(), it->oldBytes)) {
                status = document.error();
                return false;
            }
        }
        restoreSelection(document, view, transaction.before);
        status = "Undid " + transaction.label;
    } else {
        for (const EditOperation& operation : transaction.operations) {
            if (!document.replace(operation.offset, operation.oldBytes.size(), operation.newBytes)) {
                status = document.error();
                return false;
            }
        }
        restoreSelection(document, view, transaction.after);
        status = "Redid " + transaction.label;
    }
    return true;
}

void undoEdit(hexfiend::linux_ui::EngineDocument& document,
              ViewState& view,
              EditHistory& history,
              std::string& status) {
    if (history.undo.empty()) {
        status = "Nothing to undo.";
        return;
    }

    EditTransaction transaction = std::move(history.undo.back());
    history.undo.pop_back();
    if (applyHistoryTransaction(document, view, transaction, true, status)) {
        history.redo.push_back(std::move(transaction));
    } else {
        history.undo.push_back(std::move(transaction));
    }
}

void redoEdit(hexfiend::linux_ui::EngineDocument& document,
              ViewState& view,
              EditHistory& history,
              std::string& status) {
    if (history.redo.empty()) {
        status = "Nothing to redo.";
        return;
    }

    EditTransaction transaction = std::move(history.redo.back());
    history.redo.pop_back();
    if (applyHistoryTransaction(document, view, transaction, false, status)) {
        history.undo.push_back(std::move(transaction));
    } else {
        history.redo.push_back(std::move(transaction));
    }
}

bool readUnsignedScalar(const std::vector<std::uint8_t>& bytes,
                        std::size_t byteCount,
                        bool bigEndian,
                        std::uint64_t& value) {
    if (byteCount == 0 || byteCount > sizeof(std::uint64_t) || bytes.size() < byteCount) return false;
    value = 0;
    if (bigEndian) {
        for (std::size_t i = 0; i < byteCount; ++i) value = (value << 8) | bytes[i];
    } else {
        for (std::size_t i = byteCount; i > 0; --i) value = (value << 8) | bytes[i - 1];
    }
    return true;
}

bool readSignedScalar(const std::vector<std::uint8_t>& bytes,
                      std::size_t byteCount,
                      bool bigEndian,
                      std::int64_t& value) {
    std::uint64_t unsignedValue = 0;
    if (!readUnsignedScalar(bytes, byteCount, bigEndian, unsignedValue)) return false;
    value = signExtend(unsignedValue, static_cast<unsigned>(byteCount * 8));
    return true;
}

template <typename T>
bool readFloatScalar(const std::vector<std::uint8_t>& bytes, bool bigEndian, T& value) {
    if (bytes.size() < sizeof(T)) return false;
    std::array<std::uint8_t, sizeof(T)> ordered{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        ordered[i] = bigEndian ? bytes[sizeof(T) - 1 - i] : bytes[i];
    }
    std::memcpy(&value, ordered.data(), sizeof(T));
    return true;
}

std::string formatInspectorNumber(std::uint64_t value, std::size_t byteCount) {
    return std::to_string(value) + " / 0x" + formatHex(value, static_cast<int>(byteCount * 2));
}

std::string formatFloatValue(double value, const char* format) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

std::string utfPreview(const std::vector<std::uint8_t>& bytes) {
    std::string result;
    for (std::uint8_t byte : bytes) {
        if (byte == 0) break;
        result.push_back(std::isprint(static_cast<unsigned char>(byte)) ? static_cast<char>(byte) : '.');
    }
    return result.empty() ? "." : result;
}

bool segmentedControl(const char* id, int* selected, const char* const* labels, int count) {
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 0.0f);
        const bool isSelected = *selected == i;
        if (isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        if (ImGui::Button(labels[i], ImVec2(42.0f, 0.0f))) {
            *selected = i;
            changed = true;
        }
        if (isSelected) ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return changed;
}

void drawCaretIfActive(const ViewState& view, EditorPane pane, bool isSelected) {
    if (!isSelected || view.activePane != pane || view.selectionLength > 1) return;
    if (std::fmod(ImGui::GetTime(), 1.0) >= 0.5) return;

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float x = pane == EditorPane::Hex && view.pendingHexNibble >= 0
        ? min.x + ImGui::CalcTextSize("0").x
        : min.x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, min.y + 1.0f), ImVec2(x, max.y - 1.0f),
                                        ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

void handleByteDragSelection(hexfiend::linux_ui::EngineDocument& document,
                             ViewState& view,
                             EditorPane pane,
                             std::uint64_t byteOffset) {
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        view.activePane = pane;
        selectByte(document, view, byteOffset, true);
    }
}

float hexByteX(float baseX, float byteWidth, float gapWidth, std::size_t index) {
    const std::size_t groupCount = index / 4;
    return baseX + static_cast<float>(index) * (byteWidth + gapWidth) + static_cast<float>(groupCount) * gapWidth;
}

float editorRowWidth(const ViewState& view,
                     std::uint64_t bytesPerRow,
                     float lineNumberWidth,
                     float hexByteWidth,
                     float hexGapWidth,
                     float asciiByteWidth,
                     float paneGap) {
    const float hexWidth = view.showHex
        ? hexByteX(0.0f, hexByteWidth, hexGapWidth, static_cast<std::size_t>(bytesPerRow))
        : 0.0f;
    const float asciiWidth = view.showAscii ? static_cast<float>(bytesPerRow) * asciiByteWidth : 0.0f;
    const float gapWidth = view.showHex && view.showAscii ? paneGap : 0.0f;
    return lineNumberWidth + hexWidth + gapWidth + asciiWidth;
}

std::uint64_t bytesPerRowForCanvas(const ViewState& view,
                                   float canvasWidth,
                                   float lineNumberWidth,
                                   float hexByteWidth,
                                   float hexGapWidth,
                                   float asciiByteWidth,
                                   float paneGap) {
    std::uint64_t best = kMinBytesPerRow;
    for (std::uint64_t candidate = kMinBytesPerRow; candidate <= kMaxBytesPerRow; ++candidate) {
        if (editorRowWidth(view, candidate, lineNumberWidth, hexByteWidth, hexGapWidth, asciiByteWidth, paneGap) > canvasWidth) break;
        best = candidate;
    }
    return std::max<std::uint64_t>(kMinBytesPerRow, best);
}

std::uint64_t hitTestEditorRow(EditorPane pane,
                               float mouseX,
                               float baseX,
                               float byteWidth,
                               float gapWidth,
                               std::uint64_t rowOffset,
                               std::size_t count) {
    if (count == 0) return rowOffset;
    if (pane == EditorPane::Ascii) {
        const float relativeX = std::max(0.0f, mouseX - baseX);
        const std::size_t index = std::min<std::size_t>(count, static_cast<std::size_t>((relativeX + byteWidth * 0.5f) / byteWidth));
        return rowOffset + index;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const float x = hexByteX(baseX, byteWidth, gapWidth, i);
        if (mouseX < x + byteWidth * 0.5f) return rowOffset + i;
        if (mouseX < x + byteWidth + gapWidth) return rowOffset + i + 1;
    }
    return rowOffset + count;
}

void drawEditorCaretAt(ImDrawList* drawList,
                       const ViewState& view,
                       EditorPane pane,
                       std::uint64_t rowOffset,
                       std::size_t count,
                       ImVec2 origin,
                       float byteWidth,
                       float gapWidth,
                       float lineHeight) {
    if (view.activePane != pane || view.selectionLength != 0) return;
    if (view.selectionOffset < rowOffset || view.selectionOffset > rowOffset + count) return;
    if (std::fmod(ImGui::GetTime(), 1.0) >= 0.5) return;

    const std::size_t localIndex = static_cast<std::size_t>(view.selectionOffset - rowOffset);
    const float x = pane == EditorPane::Hex
        ? hexByteX(origin.x, byteWidth, gapWidth, localIndex)
        : origin.x + static_cast<float>(localIndex) * byteWidth;
    drawList->AddLine(ImVec2(x, origin.y + 1.0f), ImVec2(x, origin.y + lineHeight - 1.0f),
                      ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

void drawEditorSelectionRun(ImDrawList* drawList,
                            const ViewState& view,
                            EditorPane pane,
                            std::uint64_t absoluteOffset,
                            std::size_t count,
                            ImVec2 origin,
                            float byteWidth,
                            float gapWidth,
                            float lineHeight) {
    if (count == 0) return;
    const std::uint64_t rowEnd = absoluteOffset + count;
    const ImU32 selectionColor = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    for (const ByteRange& range : selectedRanges(view)) {
        const std::uint64_t selectionStart = range.offset;
        const std::uint64_t selectionEnd = range.offset + range.length;
        if (selectionEnd <= absoluteOffset || selectionStart >= rowEnd) continue;

        const std::size_t startIndex = static_cast<std::size_t>(std::max(selectionStart, absoluteOffset) - absoluteOffset);
        const std::size_t endIndex = static_cast<std::size_t>(std::min(selectionEnd, rowEnd) - absoluteOffset);
        if (startIndex >= endIndex) continue;

        const float x1 = pane == EditorPane::Hex
            ? hexByteX(origin.x, byteWidth, gapWidth, startIndex)
            : origin.x + static_cast<float>(startIndex) * byteWidth;
        const float x2 = pane == EditorPane::Hex
            ? hexByteX(origin.x, byteWidth, gapWidth, endIndex)
            : origin.x + static_cast<float>(endIndex) * byteWidth;
        drawList->AddRectFilled(ImVec2(x1, origin.y), ImVec2(x2, origin.y + lineHeight), selectionColor);
    }
}

void drawEditorPaneText(ImDrawList* drawList,
                        const ViewState& view,
                        EditorPane pane,
                        const std::vector<std::uint8_t>& bytes,
                        std::uint64_t rowStart,
                        std::uint64_t absoluteOffset,
                        std::size_t count,
                        ImVec2 origin,
                        float byteWidth,
                        float gapWidth,
                        float lineHeight) {
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 selectedTextColor = IM_COL32(255, 255, 255, 255);
    char text[8];
    drawEditorSelectionRun(drawList, view, pane, absoluteOffset, count, origin, byteWidth, gapWidth, lineHeight);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t byteOffset = absoluteOffset + i;
        const std::uint8_t value = bytes[static_cast<std::size_t>(rowStart) + i];
        const bool selected = selectedContains(view, byteOffset);
        const float x = pane == EditorPane::Hex
            ? hexByteX(origin.x, byteWidth, gapWidth, i)
            : origin.x + static_cast<float>(i) * byteWidth;
        if (pane == EditorPane::Hex) {
            std::snprintf(text, sizeof(text), "%02X", value);
        } else {
            std::snprintf(text, sizeof(text), "%c", std::isprint(static_cast<unsigned char>(value)) ? value : '.');
        }
        drawList->AddText(ImVec2(x, origin.y), selected ? selectedTextColor : textColor, text);
    }
    drawEditorCaretAt(drawList, view, pane, absoluteOffset, count, origin, byteWidth, gapWidth, lineHeight);
}

void drawDataInspector(hexfiend::linux_ui::EngineDocument& document, ViewState& view) {
    std::vector<std::uint8_t> bytes;
    document.read(view.selectionOffset, 16, bytes);

    ImGui::TextUnformatted("Data Inspector");
    ImGui::SameLine(140.0f);
    const char* endianLabels[] = {"LE", "BE"};
    int endianMode = view.inspectorBigEndian ? 1 : 0;
    if (segmentedControl("inspector-endian", &endianMode, endianLabels, 2)) {
        view.inspectorBigEndian = endianMode == 1;
    }

    const char* endian = view.inspectorBigEndian ? "be" : "le";
    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("data-inspector", 3, flags, ImVec2(0.0f, kDataInspectorTableHeight))) return;
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Subtype", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

    auto row = [](const char* type, const char* subtype, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(type);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(subtype);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(value.c_str());
    };

    std::uint64_t unsignedValue = 0;
    std::int64_t signedValue = 0;
    float float32 = 0.0f;
    double float64 = 0.0;
    Leb128Value uleb;
    Leb128Value sleb;
    row("Unsigned Int", "8-bit", readUnsignedScalar(bytes, 1, view.inspectorBigEndian, unsignedValue) ? formatInspectorNumber(unsignedValue, 1) : ".");
    row("Unsigned Int", (std::string("16-bit ") + endian).c_str(), readUnsignedScalar(bytes, 2, view.inspectorBigEndian, unsignedValue) ? formatInspectorNumber(unsignedValue, 2) : ".");
    row("Unsigned Int", (std::string("32-bit ") + endian).c_str(), readUnsignedScalar(bytes, 4, view.inspectorBigEndian, unsignedValue) ? formatInspectorNumber(unsignedValue, 4) : ".");
    row("Unsigned Int", (std::string("64-bit ") + endian).c_str(), readUnsignedScalar(bytes, 8, view.inspectorBigEndian, unsignedValue) ? formatInspectorNumber(unsignedValue, 8) : ".");
    row("Signed Int", "8-bit", readSignedScalar(bytes, 1, view.inspectorBigEndian, signedValue) ? std::to_string(signedValue) : ".");
    row("Signed Int", (std::string("16-bit ") + endian).c_str(), readSignedScalar(bytes, 2, view.inspectorBigEndian, signedValue) ? std::to_string(signedValue) : ".");
    row("Signed Int", (std::string("32-bit ") + endian).c_str(), readSignedScalar(bytes, 4, view.inspectorBigEndian, signedValue) ? std::to_string(signedValue) : ".");
    row("Signed Int", (std::string("64-bit ") + endian).c_str(), readSignedScalar(bytes, 8, view.inspectorBigEndian, signedValue) ? std::to_string(signedValue) : ".");
    row("Float", (std::string("32-bit ") + endian).c_str(), readFloatScalar(bytes, view.inspectorBigEndian, float32) ? formatFloatValue(float32, "%.9g") : ".");
    row("Float", (std::string("64-bit ") + endian).c_str(), readFloatScalar(bytes, view.inspectorBigEndian, float64) ? formatFloatValue(float64, "%.17g") : ".");
    row("LEB128", "unsigned", decodeLeb128(bytes, false, uleb) ? std::to_string(uleb.unsignedValue) + " (" + std::to_string(uleb.length) + " bytes)" : ".");
    row("LEB128", "signed", decodeLeb128(bytes, true, sleb) ? std::to_string(sleb.signedValue) + " (" + std::to_string(sleb.length) + " bytes)" : ".");
    row("UTF-8", "", utfPreview(bytes));
    ImGui::EndTable();
}

void drawFindBanner(hexfiend::linux_ui::EngineDocument& document,
                    ViewState& view,
                    EditHistory& history,
                    std::array<char, kTextBufferSize>& findBuffer,
                    std::array<char, kTextBufferSize>& replaceBuffer,
                    std::string& status) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    ImGui::BeginChild("find-banner", ImVec2(0.0f, 76.0f), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    const char* modes[] = {"Hex", "Text"};
    int mode = view.findAsHex ? 0 : 1;
    if (segmentedControl("find-mode", &mode, modes, 2)) {
        view.findAsHex = mode == 0;
    }
    ImGui::SameLine(124.0f);
    ImGui::TextUnformatted("Find");
    ImGui::SameLine(178.0f);
    ImGui::SetNextItemWidth(-8.0f);
    ImGui::InputText("##find-field", findBuffer.data(), findBuffer.size());

    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    ImGui::SameLine(124.0f);
    ImGui::TextUnformatted("Replace");
    ImGui::SameLine(178.0f);
    ImGui::SetNextItemWidth(-8.0f);
    ImGui::InputText("##replace-field", replaceBuffer.data(), replaceBuffer.size());

    std::vector<std::uint8_t> pattern;
    std::vector<std::uint8_t> replacement;
    const bool canFind = document.isOpen();
    if (!canFind) ImGui::BeginDisabled();
    if (ImGui::Button("Replace All")) {
        if (buildFindPattern(findBuffer, view.findAsHex, pattern, status) &&
            buildReplacePattern(replaceBuffer, view.findAsHex, replacement, status)) {
            replaceAllMatches(document, view, history, pattern, replacement, status);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Replace")) {
        if (buildFindPattern(findBuffer, view.findAsHex, pattern, status) &&
            buildReplacePattern(replaceBuffer, view.findAsHex, replacement, status)) {
            replaceCurrentMatch(document, view, history, pattern, replacement, status);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Replace & Find")) {
        if (buildFindPattern(findBuffer, view.findAsHex, pattern, status) &&
            buildReplacePattern(replaceBuffer, view.findAsHex, replacement, status) &&
            replaceCurrentMatch(document, view, history, pattern, replacement, status)) {
            findAndSelect(document, view, pattern, true, status);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Previous")) {
        if (buildFindPattern(findBuffer, view.findAsHex, pattern, status)) {
            findAndSelect(document, view, pattern, false, status);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next")) {
        if (buildFindPattern(findBuffer, view.findAsHex, pattern, status)) {
            findAndSelect(document, view, pattern, true, status);
        }
    }
    if (!canFind) ImGui::EndDisabled();
    ImGui::EndChild();
}

void drawDocumentScroller(hexfiend::linux_ui::EngineDocument& document,
                          ViewState& view,
                          float height) {
    const std::uint64_t totalRows = std::max<std::uint64_t>(1, (document.length() + view.bytesPerRow - 1) / view.bytesPerRow);
    const std::uint64_t maxLine = totalRows > view.visibleRows ? totalRows - view.visibleRows : 0;
    std::uint64_t currentLine = maxLine == 0 ? 0 : std::min<std::uint64_t>(view.viewOffset / view.bytesPerRow, maxLine);

    const ImVec2 size(18.0f, height);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##document-scroll", size);

    if (maxLine > 0 && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float relativeY = std::clamp((ImGui::GetIO().MousePos.y - origin.y) / std::max(1.0f, height), 0.0f, 1.0f);
        currentLine = static_cast<std::uint64_t>(std::llround(relativeY * static_cast<double>(maxLine)));
        view.viewOffset = currentLine * view.bytesPerRow;
        clampView(document, view, false);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 trackColor = ImGui::GetColorU32(ImGuiCol_ScrollbarBg);
    const ImU32 grabColor = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ScrollbarGrabActive :
                                               ImGui::IsItemHovered() ? ImGuiCol_ScrollbarGrabHovered :
                                               ImGuiCol_ScrollbarGrab);
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), trackColor, 6.0f);

    const float visibleFraction = totalRows == 0 ? 1.0f : std::clamp(static_cast<float>(view.visibleRows) / static_cast<float>(totalRows), 0.0f, 1.0f);
    const float grabHeight = maxLine == 0 ? height : std::clamp(height * visibleFraction, 24.0f, height);
    const float travel = std::max(0.0f, height - grabHeight);
    const float position = maxLine == 0 ? 0.0f : travel * (static_cast<float>(currentLine) / static_cast<float>(maxLine));
    drawList->AddRectFilled(ImVec2(origin.x + 2.0f, origin.y + position),
                            ImVec2(origin.x + size.x - 2.0f, origin.y + position + grabHeight),
                            grabColor,
                            6.0f);
}

void drawDocumentToolbar(hexfiend::linux_ui::EngineDocument& document,
                         ViewState& view,
                         EditHistory& history,
                         DiffState& diff,
                         std::array<char, kTextBufferSize>& compareStartBuffer,
                         std::array<char, kTextBufferSize>& compareLengthBuffer,
                         std::array<char, kTextBufferSize>& jumpOffsetBuffer,
                         bool& compareUseRange,
                         bool& showCompareDialog,
                         bool& showJumpDialog,
                         bool& showSaveAsDialog,
                         std::string& status) {
    const float toolbarWidth = ImGui::GetContentRegionAvail().x;
    const bool compact = toolbarWidth < kDocumentToolbarCompactWidth;
    ImGui::BeginChild("document-toolbar", ImVec2(0.0f, documentToolbarHeight(toolbarWidth)), false, ImGuiWindowFlags_NoScrollbar);

    if (ImGui::Button("Save", ImVec2(64.0f, 0.0f))) saveDocument(document, showSaveAsDialog, status);
    ImGui::SameLine();
    if (history.undo.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Undo", ImVec2(64.0f, 0.0f))) undoEdit(document, view, history, status);
    if (history.undo.empty()) ImGui::EndDisabled();
    ImGui::SameLine();
    if (history.redo.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Redo", ImVec2(64.0f, 0.0f))) redoEdit(document, view, history, status);
    if (history.redo.empty()) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const char* modes[] = {"Insert", "Overwrite", "Read-only"};
    int modeIndex = editModeIndex(view.editMode);
    if (segmentedControl("document-edit-mode", &modeIndex, modes, 3)) view.editMode = editModeFromIndex(modeIndex);

    if (!compact) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    }
    if (ImGui::Button("Find", ImVec2(64.0f, 0.0f))) view.showFindBanner = true;
    ImGui::SameLine();
    if (ImGui::Button("Jump", ImVec2(64.0f, 0.0f))) showJumpToOffset(jumpOffsetBuffer, view, showJumpDialog);
    ImGui::SameLine();
    if (ImGui::Button("Compare", ImVec2(82.0f, 0.0f))) {
        showCompareWithFile(view, compareStartBuffer, compareLengthBuffer, compareUseRange, showCompareDialog);
    }
    ImGui::SameLine();
    if (diff.ranges.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Prev Diff", ImVec2(82.0f, 0.0f))) stepDiffRange(document, view, diff, -1, status);
    ImGui::SameLine();
    if (ImGui::Button("Next Diff", ImVec2(82.0f, 0.0f))) stepDiffRange(document, view, diff, 1, status);
    if (diff.ranges.empty()) ImGui::EndDisabled();

    ImGui::EndChild();
}

void drawHexRepresenters(hexfiend::linux_ui::EngineDocument& document,
                         ViewState& view,
                         std::string& status) {
    if (!view.showLineNumbers && !view.showHex && !view.showAscii) {
        ImGui::TextUnformatted("No representers are visible.");
        return;
    }

    const float scrollerWidth = view.showScroller ? 24.0f : 0.0f;
    const float editorHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("document-editor-area", ImVec2(view.showScroller ? -scrollerWidth : 0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollbar);

    const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float glyphWidth = ImGui::CalcTextSize("0").x;
    const float hexByteWidth = ImGui::CalcTextSize("00").x;
    const float hexGapWidth = glyphWidth;
    const int offsetDigits = offsetDigitCount(document.length());
    const float lineNumberWidth = view.showLineNumbers
        ? ImGui::CalcTextSize(std::string(static_cast<std::size_t>(offsetDigits), '0').c_str()).x + glyphWidth * 2.0f
        : 0.0f;
    const float paneGap = 28.0f;
    const std::uint64_t responsiveBytesPerRow =
        bytesPerRowForCanvas(view, canvasSize.x, lineNumberWidth, hexByteWidth, hexGapWidth, glyphWidth, paneGap);
    if (responsiveBytesPerRow != view.bytesPerRow) {
        view.bytesPerRow = responsiveBytesPerRow;
        view.viewOffset = (view.viewOffset / view.bytesPerRow) * view.bytesPerRow;
        clampView(document, view, false);
    }
    const float hexBaseX = canvasOrigin.x + lineNumberWidth;
    const float hexWidth = hexByteX(0.0f, hexByteWidth, hexGapWidth, static_cast<std::size_t>(view.bytesPerRow));
    const float asciiBaseX = hexBaseX + (view.showHex ? hexWidth + paneGap : 0.0f);
    const float headerHeight = view.showColumnHeader ? lineHeight : 0.0f;
    const float rowsTop = canvasOrigin.y + headerHeight;
    const float rowsHeight = std::max(0.0f, canvasSize.y - headerHeight);
    view.visibleRows = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(rowsHeight / lineHeight)));
    const std::uint64_t bytesToRead = view.bytesPerRow * view.visibleRows;
    std::vector<std::uint8_t> bytes;
    if (!document.read(view.viewOffset, static_cast<std::size_t>(bytesToRead), bytes)) {
        ImGui::EndChild();
        ImGui::TextUnformatted("Unable to read bytes from the current document.");
        status = document.error().empty() ? "Read failed." : document.error();
        return;
    }
    const std::uint64_t rowCount = std::max<std::uint64_t>(1, (bytes.size() + view.bytesPerRow - 1) / view.bytesPerRow);

    ImGui::InvisibleButton("##document-editor-surface", canvasSize);
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        const long long currentLine = static_cast<long long>(view.viewOffset / view.bytesPerRow);
        const long long deltaLines = -static_cast<long long>(std::llround(ImGui::GetIO().MouseWheel * 3.0f));
        const long long nextLine = std::max<long long>(0, currentLine + deltaLines);
        view.viewOffset = static_cast<std::uint64_t>(nextLine) * view.bytesPerRow;
        clampView(document, view, false);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        view.multiSelectionInProgress = false;
        view.multiSelectionBase.clear();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && rowCount > 0) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const std::uint64_t rowIndex = std::min<std::uint64_t>(
            rowCount - 1,
            static_cast<std::uint64_t>(std::max(0.0f, mouse.y - rowsTop) / lineHeight));
        const std::uint64_t rowStart = rowIndex * view.bytesPerRow;
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(view.bytesPerRow, bytes.size() - rowStart));
        const std::uint64_t absoluteOffset = view.viewOffset + rowStart;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (view.showAscii && (!view.showHex || mouse.x >= asciiBaseX - paneGap * 0.5f)) {
                view.activePane = EditorPane::Ascii;
            } else {
                view.activePane = EditorPane::Hex;
            }
        }
        const bool extend = ImGui::GetIO().KeyShift || ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f);
        const float paneBaseX = view.activePane == EditorPane::Ascii ? asciiBaseX : hexBaseX;
        const float paneByteWidth = view.activePane == EditorPane::Ascii ? glyphWidth : hexByteWidth;
        const float paneGapWidth = view.activePane == EditorPane::Ascii ? 0.0f : hexGapWidth;
        const std::uint64_t offset = hitTestEditorRow(view.activePane, mouse.x, paneBaseX, paneByteWidth, paneGapWidth, absoluteOffset, count);
        const bool multiSelect = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
        if (multiSelect) {
            extendAdditionalSelection(document, view, offset, ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        } else {
            selectCharacter(document, view, offset, extend);
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 rowTint = ImGui::GetColorU32(ImGuiCol_TableRowBgAlt);
    const ImU32 disabledText = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    if (view.showColumnHeader) {
        if (view.showHex) {
            for (std::uint64_t i = 0; i < view.bytesPerRow; ++i) {
                char label[4];
                std::snprintf(label, sizeof(label), "%02llX", static_cast<unsigned long long>(i));
                drawList->AddText(ImVec2(hexByteX(hexBaseX, hexByteWidth, hexGapWidth, static_cast<std::size_t>(i)), canvasOrigin.y),
                                  disabledText, label);
            }
        }
        if (view.showAscii) {
            drawList->AddText(ImVec2(asciiBaseX, canvasOrigin.y), disabledText, "Text");
        }
    }

    for (std::uint64_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        const std::uint64_t rowStart = rowIndex * view.bytesPerRow;
        if (rowStart >= bytes.size()) break;
        const std::uint64_t absoluteOffset = view.viewOffset + rowStart;
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(view.bytesPerRow, bytes.size() - rowStart));
        const float y = rowsTop + static_cast<float>(rowIndex) * lineHeight;
        if ((rowIndex & 1) != 0) {
            drawList->AddRectFilled(ImVec2(canvasOrigin.x, y), ImVec2(canvasOrigin.x + canvasSize.x, y + lineHeight), rowTint);
        }
        if (view.showLineNumbers) {
            char offsetText[24];
            std::snprintf(offsetText, sizeof(offsetText), "%0*llX", offsetDigits, static_cast<unsigned long long>(absoluteOffset));
            drawList->AddText(ImVec2(canvasOrigin.x, y), disabledText, offsetText);
        }
        if (view.showHex) {
            drawEditorPaneText(drawList, view, EditorPane::Hex, bytes, rowStart, absoluteOffset, count,
                               ImVec2(hexBaseX, y), hexByteWidth, hexGapWidth, lineHeight);
        }
        if (view.showAscii) {
            drawEditorPaneText(drawList, view, EditorPane::Ascii, bytes, rowStart, absoluteOffset, count,
                               ImVec2(asciiBaseX, y), glyphWidth, 0.0f, lineHeight);
        }
    }
    ImGui::EndChild();

    if (view.showScroller) {
        ImGui::SameLine();
        drawDocumentScroller(document, view, editorHeight);
    }
}

void drawDiffWindow(hexfiend::linux_ui::EngineDocument& document,
                    ViewState& view,
                    DiffState& diff,
                    bool& showDiffWindow,
                    std::string& status) {
    if (!showDiffWindow) return;

    ImGui::SetNextWindowSize(ImVec2(760.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Differences", &showDiffWindow)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(diff.leftPath.c_str());
    ImGui::TextUnformatted(diff.rightPath.c_str());
    ImGui::Text("%zu range%s, left %s, right %s%s",
                diff.ranges.size(),
                diff.ranges.size() == 1 ? "" : "s",
                formatBytes(diff.leftLength).c_str(),
                formatBytes(diff.rightLength).c_str(),
                diff.truncated ? ", truncated" : "");

    if (!diff.ranges.empty()) {
        diff.selectedRangeIndex = std::clamp(diff.selectedRangeIndex, 0, static_cast<int>(diff.ranges.size() - 1));
        if (ImGui::Button("Previous", ImVec2(86.0f, 0.0f))) stepDiffRange(document, view, diff, -1, status);
        ImGui::SameLine();
        if (ImGui::Button("Next", ImVec2(70.0f, 0.0f))) stepDiffRange(document, view, diff, 1, status);
        ImGui::SameLine();
        if (ImGui::Button("Jump", ImVec2(70.0f, 0.0f))) selectDiffRange(document, view, diff, diff.selectedRangeIndex, status);
        ImGui::SameLine();
        if (ImGui::Button("Copy Summary", ImVec2(112.0f, 0.0f))) copySelectedDiffSummary(diff, status);
        ImGui::SameLine();
        ImGui::Text("Range %d of %zu", diff.selectedRangeIndex + 1, diff.ranges.size());
        const DiffRange& selected = diff.ranges[static_cast<std::size_t>(diff.selectedRangeIndex)];
        const float paneWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        drawDiffPreviewPane("left-diff-preview", diff.leftPath, selected.leftOffset, selected.leftLength,
                            selected.leftPreview, ImVec2(paneWidth, 92.0f));
        ImGui::SameLine();
        drawDiffPreviewPane("right-diff-preview", diff.rightPath, selected.rightOffset, selected.rightLength,
                            selected.rightPreview, ImVec2(0.0f, 92.0f));
    }

    if (ImGui::BeginTable("diff-ranges", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, -1.0f))) {
        ImGui::TableSetupColumn("Left Off", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Right Off", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Left Len", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Right Len", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Left Bytes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Right Bytes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < diff.ranges.size(); ++i) {
            const DiffRange& range = diff.ranges[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string offset = "0x" + formatHex(range.leftOffset);
            if (ImGui::Selectable(offset.c_str(), diff.selectedRangeIndex == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns)) {
                selectDiffRange(document, view, diff, static_cast<int>(i), status);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%s", formatHex(range.rightOffset).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(range.leftLength));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(range.rightLength));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(range.leftPreview.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(range.rightPreview.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void drawBinaryTemplatePanel(hexfiend::linux_ui::EngineDocument& document,
                             ViewState& view,
                             std::vector<TemplateEntry>& templates,
                             int& selectedTemplate,
                             std::uint64_t& templateAnchor,
                             TemplateRunResult& templateResult,
                             std::string& status) {
    if (templates.empty()) templates = scanTemplates();

    ImGui::BeginChild("binary-template-panel", ImVec2(260.0f, 0.0f), true);
    if (ImGui::BeginCombo("Template", selectedTemplate >= 0 && selectedTemplate < static_cast<int>(templates.size())
        ? templates[static_cast<std::size_t>(selectedTemplate)].name.c_str()
        : "None")) {
        if (ImGui::Selectable("None", selectedTemplate < 0)) selectedTemplate = -1;
        for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
            if (ImGui::Selectable(templates[static_cast<std::size_t>(i)].name.c_str(), selectedTemplate == i)) {
                selectedTemplate = i;
                templateResult = {};
                status = "Selected template " + templates[static_cast<std::size_t>(i)].name;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("Refresh", ImVec2(78.0f, 0.0f))) {
        templates = scanTemplates();
        selectedTemplate = std::min(selectedTemplate, static_cast<int>(templates.size()) - 1);
        templateResult = {};
    }
    ImGui::SameLine();
    if (ImGui::Button("Anchor", ImVec2(78.0f, 0.0f))) {
        templateAnchor = view.selectionOffset;
        templateResult = {};
        status = "Anchored template at offset 0x" + formatHex(view.selectionOffset);
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto", ImVec2(78.0f, 0.0f))) {
        const int match = findMatchingTemplate(document, templates, templateAnchor);
        if (match >= 0) {
            selectedTemplate = match;
            templateResult = {};
            status = "Matched template " + templates[static_cast<std::size_t>(match)].name;
        } else {
            status = "No matching template at offset 0x" + formatHex(templateAnchor);
        }
    }

    if (selectedTemplate >= 0 && selectedTemplate < static_cast<int>(templates.size())) {
        const TemplateEntry& selected = templates[static_cast<std::size_t>(selectedTemplate)];
        ImGui::Separator();
        ImGui::TextUnformatted(selected.path.c_str());
        ImGui::Text("Anchor 0x%s", formatHex(templateAnchor).c_str());
        if (ImGui::Button("Run Template", ImVec2(116.0f, 0.0f))) {
            if (document.isOpen()) {
                templateResult = runTemplate(document, selected, templateAnchor);
                status = templateResult.error.empty()
                    ? "Ran template " + selected.name
                    : "Template error: " + templateResult.error;
            }
        }
        if (selected.requirements.empty()) {
            ImGui::TextDisabled("No signature requirements.");
        } else if (ImGui::BeginTable("template-requirements", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("OK", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableHeadersRow();
            for (const TemplateRequirement& requirement : selected.requirements) {
                const bool matches = templateRequirementMatches(document, requirement, templateAnchor);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("0x%s", formatHex(requirement.offset).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(formatBytePreview(requirement.bytes).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(matches ? "Yes" : "No");
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (!templateResult.error.empty()) ImGui::TextWrapped("%s", templateResult.error.c_str());
        if (!templateResult.rows.empty()) {
            if (ImGui::BeginTable("template-output", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 180.0f))) {
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 48.0f);
                ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                std::vector<std::string> hiddenSectionPaths;
                for (const TemplateRow& row : templateResult.rows) {
                    while (!hiddenSectionPaths.empty() && !templateRowIsUnderPath(row, hiddenSectionPaths.back())) {
                        hiddenSectionPaths.pop_back();
                    }
                    if (!hiddenSectionPaths.empty()) continue;

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const std::string offset = "0x" + formatHex(row.offset);
                    if (ImGui::Selectable(offset.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) &&
                        row.offset < document.length()) {
                        selectByte(document, view, row.offset, false);
                        view.viewOffset = (row.offset / view.bytesPerRow) * view.bytesPerRow;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.length));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.path.c_str());
                    ImGui::TableSetColumnIndex(3);
                    if (row.isSection) {
                        ImGui::SetNextItemOpen(!row.collapsed, ImGuiCond_Once);
                        const bool open = ImGui::TreeNodeEx(row.label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
                        if (open) {
                            ImGui::TreePop();
                        } else {
                            hiddenSectionPaths.push_back(templateRowSectionPath(row));
                        }
                    } else {
                        ImGui::TreeNodeEx(row.label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                    }
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(row.value.c_str());
                }
                ImGui::EndTable();
            }
        }
        if (ImGui::CollapsingHeader("Source")) {
            ImGui::BeginChild("template-source", ImVec2(0.0f, 120.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(selected.source.c_str());
            ImGui::EndChild();
        }
    } else {
        ImGui::Separator();
        ImGui::TextDisabled("%zu templates", templates.size());
    }
    ImGui::EndChild();
}

void configureStyle(bool darkTheme) {
    if (darkTheme) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 8.0f;
    style.ItemSpacing = ImVec2(5.0f, 3.0f);
    style.FramePadding = ImVec2(6.0f, 3.0f);

    ImVec4* colors = style.Colors;
    if (darkTheme) {
        colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.92f, 1.0f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.57f, 0.60f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.13f, 1.0f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.0f);
        colors[ImGuiCol_Border] = ImVec4(0.25f, 0.28f, 0.30f, 1.0f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.08f, 0.09f, 1.0f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.09f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.13f, 0.15f, 1.0f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.11f, 0.12f, 0.13f, 1.0f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.14f, 0.16f, 0.18f, 1.0f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.34f, 0.37f, 1.0f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.25f, 0.27f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.18f, 0.39f, 0.54f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.49f, 0.67f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.14f, 0.32f, 0.45f, 1.0f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.18f, 0.39f, 0.54f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.22f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.28f, 0.31f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.34f, 0.38f, 1.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.19f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.23f, 0.26f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.28f, 0.31f, 1.0f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.74f, 0.77f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.29f, 0.62f, 0.67f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.78f, 0.80f, 1.0f);
        colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.30f, 0.32f, 1.0f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.25f, 0.48f, 0.56f, 0.45f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.32f, 0.63f, 0.72f, 0.75f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.38f, 0.76f, 0.82f, 0.95f);
    } else {
        colors[ImGuiCol_WindowBg] = ImVec4(0.91f, 0.91f, 0.89f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.94f, 0.94f, 0.92f, 1.0f);
        colors[ImGuiCol_TableRowBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.87f, 0.90f, 0.98f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.30f, 0.42f, 0.72f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.38f, 0.50f, 0.78f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.36f, 0.66f, 1.0f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.30f, 0.42f, 0.72f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.82f, 0.82f, 0.80f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.76f, 0.76f, 0.74f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.68f, 0.68f, 0.66f, 1.0f);
    }
}

bool writeBinaryFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

int runSelfTests() {
    const std::string leftPath = "/tmp/hexfiend-linux-selftest-left.bin";
    const std::string rightPath = "/tmp/hexfiend-linux-selftest-right.bin";
    const std::string insertedPath = "/tmp/hexfiend-linux-selftest-inserted.bin";
    const std::string multiInsertedPath = "/tmp/hexfiend-linux-selftest-multi-inserted.bin";
    const std::string rangePath = "/tmp/hexfiend-linux-selftest-range.bin";
    const std::string templatePath = "/tmp/hexfiend-linux-selftest-template.tcl";
    const std::string browserDirectory = "/tmp/hexfiend-linux-selftest-browser";
    const std::string browserFilePath = browserDirectory + "/sample.bin";
    const std::string browserSubdirectory = browserDirectory + "/nested";
    {
        const char* args[] = {
            "HexFiendLinux",
            "-HFOpenFile", "/tmp/open-a",
            "-HFDiffLeftFile", "/tmp/left",
            "-HFDiffRightFile", "/tmp/right",
            "-HFOpenData", "AQID/w==",
        };
        CommandLineOptions options = parseCommandLineOptions(9, const_cast<char**>(args));
        if (options.filesToOpen.size() != 1 ||
            options.filesToOpen[0] != "/tmp/open-a" ||
            options.diffLeftFile != "/tmp/left" ||
            options.diffRightFile != "/tmp/right" ||
            !options.hasDataToOpen ||
            options.dataToOpen != std::vector<std::uint8_t>({0x01, 0x02, 0x03, 0xFF})) {
            std::fprintf(stderr, "self-test: macOS command-line parsing failed\n");
            return 1;
        }
    }
    {
        const char* args[] = {"HexFiendLinux", "-HFOpenData", "not base64!"};
        CommandLineOptions options = parseCommandLineOptions(3, const_cast<char**>(args));
        if (options.error.empty()) {
            std::fprintf(stderr, "self-test: command-line data validation failed\n");
            return 1;
        }
    }
    {
        const char* args[] = {"HexFiendLinux", "--compare", "/tmp/left", "/tmp/right"};
        CommandLineOptions options = parseCommandLineOptions(4, const_cast<char**>(args));
        if (options.diffLeftFile != "/tmp/left" || options.diffRightFile != "/tmp/right") {
            std::fprintf(stderr, "self-test: compare command-line parsing failed\n");
            return 1;
        }
    }
    {
        const ProcessEntry process{1234, "HexFiendLinux --self-test"};
        const MemoryRegion region{0x1000, 0x2000, "r-xp", "/usr/bin/hexfiend"};
        if (!processMatchesFilter(process, "") ||
            !processMatchesFilter(process, "123") ||
            !processMatchesFilter(process, "hexfiend") ||
            !processMatchesFilter(process, " SELF ") ||
            processMatchesFilter(process, "missing") ||
            !memoryRegionMatchesFilter(region, "1000") ||
            !memoryRegionMatchesFilter(region, "r-x") ||
            !memoryRegionMatchesFilter(region, "HEXFIEND") ||
            memoryRegionMatchesFilter(region, "missing")) {
            std::fprintf(stderr, "self-test: process filter matching failed\n");
            return 1;
        }
    }
    {
        ViewState responsiveView;
        responsiveView.showLineNumbers = true;
        responsiveView.showHex = true;
        responsiveView.showAscii = true;
        const std::uint64_t narrowRows = bytesPerRowForCanvas(responsiveView, 320.0f, 48.0f, 16.0f, 8.0f, 8.0f, 28.0f);
        const std::uint64_t wideRows = bytesPerRowForCanvas(responsiveView, 1280.0f, 48.0f, 16.0f, 8.0f, 8.0f, 28.0f);
        if (narrowRows < kMinBytesPerRow ||
            wideRows <= narrowRows ||
            wideRows > kMaxBytesPerRow ||
            editorRowWidth(responsiveView, wideRows, 48.0f, 16.0f, 8.0f, 8.0f, 28.0f) > 1280.0f) {
            std::fprintf(stderr, "self-test: responsive bytes-per-row calculation failed\n");
            return 1;
        }
    }
    const std::string configPath = "/tmp/hexfiend-linux-selftest-config";
    setenv("XDG_CONFIG_HOME", configPath.c_str(), 1);
    std::remove((configPath + "/hexfiend-linux/preferences.conf").c_str());
    AppPreferences savedPreferences;
    savedPreferences.defaultEditMode = EditMode::Overwrite;
    savedPreferences.darkTheme = false;
    savedPreferences.showLineNumbers = false;
    savedPreferences.showHex = true;
    savedPreferences.showAscii = false;
    savedPreferences.showDataInspector = false;
    savedPreferences.showStatusBar = true;
    savedPreferences.showScroller = false;
    savedPreferences.bytesPerRow = 32;
    savedPreferences.recentFiles = {"/tmp/recent-a.bin", "/tmp/recent-b.bin"};
    std::string preferenceStatus;
    if (!savePreferences(savedPreferences, preferenceStatus)) {
        std::fprintf(stderr, "self-test: save preferences failed: %s\n", preferenceStatus.c_str());
        return 1;
    }
    AppPreferences loadedPreferences = loadPreferences();
    if (loadedPreferences.defaultEditMode != EditMode::Overwrite ||
        loadedPreferences.darkTheme ||
        loadedPreferences.showLineNumbers ||
        !loadedPreferences.showHex ||
        loadedPreferences.showAscii ||
        loadedPreferences.showDataInspector ||
        !loadedPreferences.showStatusBar ||
        loadedPreferences.showScroller ||
        loadedPreferences.bytesPerRow != 32 ||
        loadedPreferences.recentFiles != std::vector<std::string>({"/tmp/recent-a.bin", "/tmp/recent-b.bin"})) {
        std::fprintf(stderr, "self-test: load preferences failed\n");
        return 1;
    }
    if (!rememberRecentFile(loadedPreferences, "/tmp/recent-b.bin") ||
        loadedPreferences.recentFiles != std::vector<std::string>({"/tmp/recent-b.bin", "/tmp/recent-a.bin"}) ||
        rememberRecentFile(loadedPreferences, "")) {
        std::fprintf(stderr, "self-test: recent files update failed\n");
        return 1;
    }
    {
        AppPreferences hiddenRepresenters;
        hiddenRepresenters.showHex = false;
        hiddenRepresenters.showAscii = false;
        ViewState hiddenView;
        applyPreferences(hiddenView, hiddenRepresenters);
        if (!hiddenView.showHex || hiddenView.showAscii) {
            std::fprintf(stderr, "self-test: byte representer fallback failed\n");
            return 1;
        }
    }
    {
        const std::string exeDir = executableDirectory();
        const std::string installedTemplateRoot = exeDir.empty() ? std::string() : exeDir + "/../share/hexfiend/templates";
        if (!installedTemplateRoot.empty() && directoryExists(installedTemplateRoot)) {
            const std::vector<TemplateEntry> installedTemplates = scanTemplates();
            const bool foundBundledTemplate = std::any_of(installedTemplates.begin(), installedTemplates.end(), [](const TemplateEntry& entry) {
                return entry.name == "Executables/ELF" || entry.name == "Images/PNG";
            });
            if (!foundBundledTemplate) {
                std::fprintf(stderr, "self-test: executable-relative template scan failed\n");
                return 1;
            }
        }
    }
    {
        const std::vector<std::uint8_t> scalarBytes = {0x01, 0x02, 0x03, 0x04, 0x80, 0x00, 0x00, 0x00};
        std::uint64_t unsignedValue = 0;
        std::int64_t signedValue = 0;
        if (!readUnsignedScalar(scalarBytes, 4, false, unsignedValue) || unsignedValue != 0x04030201ULL ||
            !readUnsignedScalar(scalarBytes, 4, true, unsignedValue) || unsignedValue != 0x01020304ULL ||
            !readSignedScalar(std::vector<std::uint8_t>{0x80}, 1, false, signedValue) || signedValue != -128) {
            std::fprintf(stderr, "self-test: inspector scalar decoding failed\n");
            return 1;
        }
    }
    {
        std::vector<std::uint8_t> pasteBytes;
        std::string pasteStatus;
        if (!buildPasteBytes("CA FE", PasteMode::Hex, pasteBytes, pasteStatus) ||
            pasteBytes != std::vector<std::uint8_t>({0xCA, 0xFE}) ||
            !buildPasteBytes("CA FE", PasteMode::Text, pasteBytes, pasteStatus) ||
            pasteBytes != std::vector<std::uint8_t>({'C', 'A', ' ', 'F', 'E'}) ||
            buildPasteBytes("not hex", PasteMode::Hex, pasteBytes, pasteStatus) ||
            !buildPasteBytes("not hex", PasteMode::Auto, pasteBytes, pasteStatus) ||
            pasteBytes != std::vector<std::uint8_t>({'n', 'o', 't', ' ', 'h', 'e', 'x'})) {
            std::fprintf(stderr, "self-test: paste mode parsing failed\n");
            return 1;
        }
    }
    if (!writeBinaryFile(leftPath, {0x01, 0x02, 0x03, 0x04, 0x05, 0xE5, 0x8E, 0x26, 0x7E}) ||
        !writeBinaryFile(rightPath, {0x01, 0x02, 0xFF, 0x04, 0x05, 0xE5, 0x8E, 0x26, 0x7E}) ||
        !writeBinaryFile(insertedPath, {0x01, 0x02, 0x03, 0x99, 0x04, 0x05, 0xE5, 0x8E, 0x26, 0x7E}) ||
        !writeBinaryFile(multiInsertedPath, {0x01, 0xAA, 0x02, 0x03, 0xBB, 0x04, 0x05, 0xE5, 0x8E, 0x26, 0x7E}) ||
        !writeBinaryFile(rangePath, {0xAA, 0x02, 0x03, 0x04, 0x05, 0xE5, 0x8E, 0x99, 0x7E})) {
        std::fprintf(stderr, "self-test: unable to write fixture files\n");
        return 1;
    }
    rmdir(browserSubdirectory.c_str());
    std::remove(browserFilePath.c_str());
    rmdir(browserDirectory.c_str());
    if (mkdir(browserDirectory.c_str(), 0700) != 0 ||
        mkdir(browserSubdirectory.c_str(), 0700) != 0 ||
        !writeBinaryFile(browserFilePath, {0xCA, 0xFE})) {
        std::fprintf(stderr, "self-test: unable to write browser fixtures\n");
        return 1;
    }
    {
        FileBrowserState browser;
        if (!refreshFileBrowser(browser, browserDirectory) ||
            browser.entries.size() != 2 ||
            !browser.entries[0].isDirectory ||
            browser.entries[0].name != "nested" ||
            browser.entries[1].isDirectory ||
            browser.entries[1].name != "sample.bin" ||
            browsingDirectoryForPath(browserFilePath.c_str()) != browserDirectory) {
            std::fprintf(stderr, "self-test: file browser scan failed\n");
            return 1;
        }
    }
    {
        std::ofstream file(templatePath);
        file << "big_endian\n"
             << "requires 0 \"01 02\"\n"
             << "uint8 \"First\"\n"
             << "uint16 \"Word\"\n"
             << "section \"Tail\" { hex 2 \"Bytes\"; sectionvalue \"ok\" }\n"
             << "goto 0\n"
             << "uint32 -hex \"Magic\"\n"
             << "entry \"Computed\" 42 1 1\n"
             << "goto 0\n"
             << "uint8_bits 0,1,2 \"Bits\"\n"
             << "goto 5\n"
             << "uleb128 \"ULEB\"\n"
             << "sleb128 \"SLEB\"\n"
	             << "goto 5\n"
	             << "bytes eof \"Tail\"\n"
	             << "goto 0\n"
	             << "section \"Explicit\"\n"
	             << "uint8 \"Nested\"\n"
	             << "sectioncollapse\n"
	             << "endsection\n"
	             << "set compressed [binary format H* 789cf348cdc9c90700058c01f5]\n"
	             << "entry \"Inflated\" [zlib_uncompress $compressed]\n";
        if (!file) {
            std::fprintf(stderr, "self-test: unable to write template fixture\n");
            return 1;
        }
    }

    hexfiend::linux_ui::EngineDocument document;
    ViewState view;
    EditHistory history;
    std::array<char, kPathBufferSize> pathBuffer{};
    std::string status;
    std::vector<std::uint8_t> bytes;
    copyToBuffer(pathBuffer, leftPath);
    if (!openDocument(document, pathBuffer, view, history, status)) {
        std::fprintf(stderr, "self-test: open failed: %s\n", status.c_str());
        return 1;
    }
    closeCurrentDocument(document, pathBuffer, view, history, status);
    if (document.isOpen() || pathBuffer[0] != '\0' || !history.undo.empty() || view.selectionOffset != 0 || view.viewOffset != 0) {
        std::fprintf(stderr, "self-test: close document failed: %s\n", status.c_str());
        return 1;
    }
    copyToBuffer(pathBuffer, leftPath);
    if (!openDocument(document, pathBuffer, view, history, status)) {
        std::fprintf(stderr, "self-test: reopen after close failed: %s\n", status.c_str());
        return 1;
    }

    if (!openDataDocument(document, {0x10, 0x20, 0x30}, pathBuffer, view, history, status) ||
        document.length() != 3 ||
        !document.path().empty()) {
        std::fprintf(stderr, "self-test: open data document failed: %s\n", status.c_str());
        return 1;
    }
    bytes.clear();
    document.read(0, 3, bytes);
    if (bytes != std::vector<std::uint8_t>({0x10, 0x20, 0x30})) {
        std::fprintf(stderr, "self-test: open data contents failed\n");
        return 1;
    }
    copyToBuffer(pathBuffer, leftPath);
    if (!openDocument(document, pathBuffer, view, history, status)) {
        std::fprintf(stderr, "self-test: reopen after data failed: %s\n", status.c_str());
        return 1;
    }
    if (!findAndSelect(document, view, std::vector<std::uint8_t>{0xE5, 0x8E}, true, status) ||
        view.selectionOffset != 5 ||
        view.selectionLength != 2) {
        std::fprintf(stderr, "self-test: find next failed: %s\n", status.c_str());
        return 1;
    }
    if (!findAndSelect(document, view, std::vector<std::uint8_t>{0x01}, false, status) ||
        view.selectionOffset != 0 ||
        view.selectionLength != 1) {
        std::fprintf(stderr, "self-test: find previous failed: %s\n", status.c_str());
        return 1;
    }
    selectCharacter(document, view, 1, false);
    if (!findAndSelect(document, view, std::vector<std::uint8_t>{0x7E}, false, status) ||
        view.selectionOffset != 8 ||
        view.selectionLength != 1) {
        std::fprintf(stderr, "self-test: wrapped find previous failed: %s\n", status.c_str());
        return 1;
    }

    selectCharacter(document, view, 0, false);
    replaceSelection(document, view, history, std::vector<std::uint8_t>{0xAA}, status);
    bytes.clear();
    document.read(0, 1, bytes);
    if (bytes.empty() || bytes[0] != 0xAA) {
        std::fprintf(stderr, "self-test: replace failed\n");
        return 1;
    }
    undoEdit(document, view, history, status);
    document.read(0, 1, bytes);
    if (bytes.empty() || bytes[0] != 0x01) {
        std::fprintf(stderr, "self-test: undo failed\n");
        return 1;
    }
    redoEdit(document, view, history, status);
    document.read(0, 1, bytes);
    if (bytes.empty() || bytes[0] != 0xAA) {
        std::fprintf(stderr, "self-test: redo failed\n");
        return 1;
    }
    undoEdit(document, view, history, status);

    selectCharacter(document, view, 1, false);
    selectCharacter(document, view, 4, true);
    if (view.selectionOffset != 1 || view.selectionLength != 3 || view.selectionAnchor != 1 || selectionFocusOffset(view) != 4) {
        std::fprintf(stderr, "self-test: forward extended selection failed\n");
        return 1;
    }
    selectCharacter(document, view, 3, true);
    if (view.selectionOffset != 1 || view.selectionLength != 2 || selectionFocusOffset(view) != 3) {
        std::fprintf(stderr, "self-test: selection shrink failed\n");
        return 1;
    }
    selectCharacter(document, view, 4, false);
    selectCharacter(document, view, 1, true);
    if (view.selectionOffset != 1 || view.selectionLength != 3 || view.selectionAnchor != 4 || selectionFocusOffset(view) != 1) {
        std::fprintf(stderr, "self-test: backward extended selection failed\n");
        return 1;
    }
    view.selectionOffset = 2;
    view.selectionLength = 1;
    view.selectionAnchor = 2;
    if (selectionFocusOffset(view) != 3) {
        std::fprintf(stderr, "self-test: single-byte forward selection focus failed\n");
        return 1;
    }
    view.selectionAnchor = 3;
    if (selectionFocusOffset(view) != 2) {
        std::fprintf(stderr, "self-test: single-byte backward selection focus failed\n");
        return 1;
    }

    selectCharacter(document, view, 1, false);
    selectCharacter(document, view, 3, true);
    extendAdditionalSelection(document, view, 5, true);
    extendAdditionalSelection(document, view, 7, false);
    std::vector<ByteRange> ranges = selectedRanges(view);
    if (ranges.size() != 2 || ranges[0].offset != 1 || ranges[0].length != 2 || ranges[1].offset != 5 || ranges[1].length != 2) {
        std::fprintf(stderr, "self-test: multi-range selection failed\n");
        return 1;
    }
    std::vector<std::uint8_t> selectedBytes;
    if (!readSelection(document, view, selectedBytes, status) ||
        selectedBytes != std::vector<std::uint8_t>({0x02, 0x03, 0xE5, 0x8E})) {
        std::fprintf(stderr, "self-test: multi-range read failed\n");
        return 1;
    }
    const std::uint64_t lengthBeforeMultiDelete = document.length();
    if (!deleteSelectedRanges(document, view, history, status) || document.length() != lengthBeforeMultiDelete - 4) {
        std::fprintf(stderr, "self-test: multi-range delete failed\n");
        return 1;
    }
    bytes.clear();
    document.read(1, 3, bytes);
    if (bytes != std::vector<std::uint8_t>({0x04, 0x05, 0x26})) {
        std::fprintf(stderr, "self-test: multi-range delete contents failed\n");
        return 1;
    }
    undoEdit(document, view, history, status);

    selectCharacter(document, view, 1, false);
    const std::uint64_t originalLength = document.length();
    typeByte(document, view, history, 0xEE, status);
    bytes.clear();
    document.read(1, 2, bytes);
    if (document.length() != originalLength + 1 || bytes.size() != 2 || bytes[0] != 0xEE || bytes[1] != 0x02) {
        std::fprintf(stderr, "self-test: insert typing failed\n");
        return 1;
    }
    if (view.selectionOffset != 2 || view.selectionLength != 0 || view.selectionAnchor != 2) {
        std::fprintf(stderr, "self-test: insert caret placement failed\n");
        return 1;
    }
    clampView(document, view);
    if (view.selectionOffset != 2 || view.selectionLength != 0 || view.selectionAnchor != 2) {
        std::fprintf(stderr, "self-test: clamp moved insertion caret onto a byte\n");
        return 1;
    }
    selectCharacter(document, view, document.length(), false);
    clampView(document, view);
    if (view.selectionOffset != document.length() || view.selectionLength != 0 || view.selectionAnchor != document.length()) {
        std::fprintf(stderr, "self-test: eof insertion caret failed\n");
        return 1;
    }
    std::array<char, kTextBufferSize> eofJumpBuffer{};
    std::snprintf(eofJumpBuffer.data(), eofJumpBuffer.size(), "0x%s", formatHex(document.length()).c_str());
    if (!jumpToOffset(document, view, eofJumpBuffer, status) ||
        view.selectionOffset != document.length() ||
        view.selectionLength != 0 ||
        view.selectionAnchor != document.length()) {
        std::fprintf(stderr, "self-test: eof jump failed\n");
        return 1;
    }
    undoEdit(document, view, history, status);

    selectCharacter(document, view, 1, false);
    const std::uint64_t hexTypingOriginalLength = document.length();
    if (!typeHexNibble(document, view, history, 0xA, status)) {
        std::fprintf(stderr, "self-test: first hex nibble failed\n");
        return 1;
    }
    bytes.clear();
    document.read(1, 1, bytes);
    if (document.length() != hexTypingOriginalLength + 1 || bytes != std::vector<std::uint8_t>({0xA0}) ||
        view.pendingHexNibble != 0xA || view.selectionOffset != 2) {
        std::fprintf(stderr, "self-test: first hex nibble did not insert editable byte\n");
        return 1;
    }
    if (!typeHexNibble(document, view, history, 0x5, status)) {
        std::fprintf(stderr, "self-test: second hex nibble failed\n");
        return 1;
    }
    bytes.clear();
    document.read(1, 1, bytes);
    if (document.length() != hexTypingOriginalLength + 1 || bytes != std::vector<std::uint8_t>({0xA5}) ||
        view.pendingHexNibble != -1 || view.selectionOffset != 2) {
        std::fprintf(stderr, "self-test: second hex nibble did not update editable byte\n");
        return 1;
    }
    undoEdit(document, view, history, status);
    if (document.length() != hexTypingOriginalLength) {
        std::fprintf(stderr, "self-test: hex typing undo failed\n");
        return 1;
    }

    std::array<char, kPathBufferSize> comparePath{};
    copyToBuffer(comparePath, rightPath);
    DiffState diff;
    if (!computeDiff(document, comparePath, diff, status) || diff.ranges.empty()) {
        std::fprintf(stderr, "self-test: diff failed: %s\n", status.c_str());
        return 1;
    }

    copyToBuffer(comparePath, insertedPath);
    if (!computeDiff(document, comparePath, diff, status) ||
        diff.ranges.size() != 1 ||
        diff.ranges[0].leftOffset != 3 ||
        diff.ranges[0].rightOffset != 3 ||
        diff.ranges[0].leftLength != 0 ||
        diff.ranges[0].rightLength != 1) {
        std::fprintf(stderr, "self-test: insertion diff failed: %s\n", status.c_str());
        return 1;
    }
    if (!selectDiffRange(document, view, diff, 0, status) ||
        view.selectionOffset != 3 ||
        view.selectionLength != 0 ||
        diff.selectedRangeIndex != 0) {
        std::fprintf(stderr, "self-test: insertion diff selection failed: %s\n", status.c_str());
        return 1;
    }
    const std::string diffSummary = diffRangeSummary(diff, diff.ranges[0], 0);
    if (diffSummary.find("Difference 1 of 1") == std::string::npos ||
        diffSummary.find("Left: ") == std::string::npos ||
        diffSummary.find("Offset: 0x3") == std::string::npos ||
        diffSummary.find("Right: ") == std::string::npos) {
        std::fprintf(stderr, "self-test: diff summary formatting failed\n");
        return 1;
    }

    copyToBuffer(comparePath, multiInsertedPath);
    if (!computeDiff(document, comparePath, diff, status) ||
        diff.ranges.size() != 2 ||
        diff.ranges[0].leftOffset != 1 ||
        diff.ranges[0].rightOffset != 1 ||
        diff.ranges[0].leftLength != 0 ||
        diff.ranges[0].rightLength != 1 ||
        diff.ranges[1].leftOffset != 3 ||
        diff.ranges[1].rightOffset != 4 ||
        diff.ranges[1].leftLength != 0 ||
        diff.ranges[1].rightLength != 1) {
        std::fprintf(stderr, "self-test: multi-insertion diff failed: %s\n", status.c_str());
        return 1;
    }
    if (!stepDiffRange(document, view, diff, 1, status) || diff.selectedRangeIndex != 1 ||
        view.selectionOffset != 3 || view.selectionLength != 0) {
        std::fprintf(stderr, "self-test: next diff selection failed: %s\n", status.c_str());
        return 1;
    }
    if (!stepDiffRange(document, view, diff, 1, status) || diff.selectedRangeIndex != 0 ||
        view.selectionOffset != 1 || view.selectionLength != 0) {
        std::fprintf(stderr, "self-test: wrapped diff selection failed: %s\n", status.c_str());
        return 1;
    }

    copyToBuffer(comparePath, rangePath);
    if (!computeDiffRange(document, comparePath, 5, 4, diff, status) ||
        diff.ranges.size() != 1 ||
        diff.ranges[0].leftOffset != 7 ||
        diff.ranges[0].rightOffset != 7 ||
        diff.ranges[0].leftLength != 1 ||
        diff.ranges[0].rightLength != 1) {
        std::fprintf(stderr, "self-test: range diff failed: %s\n", status.c_str());
        return 1;
    }

    std::array<char, kPathBufferSize> compareLeftPath{};
    copyToBuffer(compareLeftPath, leftPath);
    copyToBuffer(comparePath, rightPath);
    if (!openFilesForComparison(document, pathBuffer, compareLeftPath, comparePath, view, history, diff, EditMode::Overwrite, status) ||
        !document.isOpen() ||
        document.path() != leftPath ||
        view.editMode != EditMode::Overwrite ||
        diff.ranges.empty()) {
        std::fprintf(stderr, "self-test: compare files open failed: %s\n", status.c_str());
        return 1;
    }

    TemplateEntry entry{"Self Test", templatePath, readTextFile(templatePath), parseTemplateRequirements(readTextFile(templatePath))};
    if (entry.requirements.size() != 1 ||
        !templateRequirementsMatch(document, entry, 0) ||
        findMatchingTemplate(document, std::vector<TemplateEntry>{entry}, 0) != 0) {
        std::fprintf(stderr, "self-test: template matching failed\n");
        return 1;
    }
    TemplateRunResult templateResult = runTemplate(document, entry, 0);
	    const auto hasTemplateRow = [&](const std::string& label, const std::string& value) {
	        return std::any_of(templateResult.rows.begin(), templateResult.rows.end(), [&](const TemplateRow& row) {
	            return row.label == label && row.value == value;
	        });
	    };
	    const bool hasCollapsedExplicitSection = std::any_of(templateResult.rows.begin(), templateResult.rows.end(), [](const TemplateRow& row) {
	        return row.label == "Explicit" && row.isSection && row.collapsed;
	    });
	    const bool hasNestedExplicitRow = std::any_of(templateResult.rows.begin(), templateResult.rows.end(), [](const TemplateRow& row) {
	        return row.path == "Explicit" && row.label == "Nested" && row.value == "1";
	    });
	    if (!templateResult.error.empty() || templateResult.rows.size() < 5 ||
	        !hasTemplateRow("Magic", "0x01020304") ||
	        !hasTemplateRow("Computed", "42") ||
	        !hasTemplateRow("Bits", "4") ||
	        !hasTemplateRow("ULEB", "624485") ||
	        !hasTemplateRow("SLEB", "-2") ||
	        !hasTemplateRow("Tail", "E5 8E 26 7E") ||
	        !hasTemplateRow("Inflated", "Hello") ||
	        !hasCollapsedExplicitSection ||
	        !hasNestedExplicitRow) {
        std::fprintf(stderr, "self-test: template failed: %s\n", templateResult.error.c_str());
        return 1;
    }

    std::vector<MemoryRegion> selfRegions = scanProcessRegions(getpid());
    auto selfRegion = std::find_if(selfRegions.begin(), selfRegions.end(), [](const MemoryRegion& region) {
        return (region.name.empty() || region.name.front() != '[') &&
            region.end > region.start &&
            region.end - region.start <= 256ULL * 1024ULL * 1024ULL;
    });
    if (selfRegion == selfRegions.end()) {
        std::fprintf(stderr, "self-test: no readable process region found\n");
        return 1;
    }
    if (!openProcessRegionSnapshot(document, pathBuffer, view, history, getpid(), *selfRegion, status) ||
        !document.isOpen() ||
        document.length() != selfRegion->end - selfRegion->start ||
        view.editMode != EditMode::ReadOnly) {
        std::fprintf(stderr, "self-test: process snapshot failed: %s\n", status.c_str());
        return 1;
    }
    const std::string snapshotPath = document.path();
    if (!openProcessRegionSnapshot(document, pathBuffer, view, history, getpid(), *selfRegion, status) ||
        !document.isOpen() ||
        document.length() != selfRegion->end - selfRegion->start ||
        view.editMode != EditMode::ReadOnly) {
        std::fprintf(stderr, "self-test: process snapshot refresh failed: %s\n", status.c_str());
        return 1;
    }
    const std::string refreshedSnapshotPath = document.path();
    document.createEmpty();
    if (!snapshotPath.empty()) std::remove(snapshotPath.c_str());
    if (!refreshedSnapshotPath.empty()) std::remove(refreshedSnapshotPath.c_str());

    std::remove(leftPath.c_str());
    std::remove(rightPath.c_str());
    std::remove(insertedPath.c_str());
    std::remove(multiInsertedPath.c_str());
    std::remove(rangePath.c_str());
    std::remove(templatePath.c_str());
    std::remove(browserFilePath.c_str());
    rmdir(browserSubdirectory.c_str());
    rmdir(browserDirectory.c_str());
    std::remove((configPath + "/hexfiend-linux/preferences.conf").c_str());
    rmdir((configPath + "/hexfiend-linux").c_str());
    rmdir(configPath.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") == 0) {
        return runSelfTests();
    }
    const bool renderSmokeTest = argc > 1 && std::strcmp(argv[1], "--render-smoke-test") == 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return renderSmokeTest ? 77 : 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("Hex Fiend", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          kWindowWidth, kWindowHeight,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return renderSmokeTest ? 77 : 1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return renderSmokeTest ? 77 : 1;
    }
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 150");

    if (renderSmokeTest) {
        configureStyle(true);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(360.0f, 120.0f), ImGuiCond_Always);
        ImGui::Begin("Hex Fiend Render Smoke", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("HexFiendLinux");
        ImGui::TextUnformatted("ImGui + SDL2/OpenGL");
        ImGui::End();

        ImGui::Render();
        int displayW = 0;
        int displayH = 0;
        SDL_GL_GetDrawableSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.05f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glFlush();

        bool hasMultipleColors = false;
        if (displayW > 0 && displayH > 0) {
            std::vector<unsigned char> pixels(static_cast<std::size_t>(displayW) * static_cast<std::size_t>(displayH) * 4);
            glReadPixels(0, 0, displayW, displayH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (std::size_t i = 4; i + 3 < pixels.size(); i += 4) {
                if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] || pixels[i + 2] != pixels[2] || pixels[i + 3] != pixels[3]) {
                    hasMultipleColors = true;
                    break;
                }
            }
        }

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();

        if (!hasMultipleColors) {
            std::fprintf(stderr, "render smoke test produced a blank frame\n");
            return 1;
        }
        return 0;
    }

    hexfiend::linux_ui::EngineDocument document;
    ViewState view;
    std::array<char, kPathBufferSize> pathBuffer{};
    std::array<char, kPathBufferSize> drivePathBuffer{};
    std::array<char, kPathBufferSize> compareLeftPathBuffer{};
    std::array<char, kPathBufferSize> comparePathBuffer{};
    std::array<char, kTextBufferSize> compareStartBuffer{};
    std::array<char, kTextBufferSize> compareLengthBuffer{};
    std::array<char, kTextBufferSize> findBuffer{};
    std::array<char, kTextBufferSize> replaceBuffer{};
    std::array<char, kTextBufferSize> jumpOffsetBuffer{};
    FileBrowserState openFileBrowser;
    FileBrowserState compareFileBrowser;
    FileBrowserState compareLeftFileBrowser;
    FileBrowserState compareRightFileBrowser;
    FileBrowserState saveAsFileBrowser;
    std::vector<DeviceEntry> devices;
    std::vector<ProcessEntry> processes;
    std::vector<MemoryRegion> processRegions;
    std::array<char, kTextBufferSize> processFilterBuffer{};
    std::array<char, kTextBufferSize> regionFilterBuffer{};
    std::vector<TemplateEntry> templates;
    int selectedProcess = -1;
    int selectedRegion = -1;
    int selectedTemplate = -1;
    std::uint64_t templateAnchor = 0;
    TemplateRunResult templateResult;
    EditHistory history;
    DiffState diff;
    std::string status = "No file open.";
    AppPreferences preferences = loadPreferences();
    configureStyle(preferences.darkTheme);
    applyPreferences(view, preferences);
    EditMode defaultEditMode = preferences.defaultEditMode;
    bool running = true;
    bool showOpenDialog = false;
    bool showOpenDriveDialog = false;
    bool showOpenProcessDialog = false;
    bool showCompareDialog = false;
    bool showCompareFilesDialog = false;
    bool compareUseRange = false;
    bool showDiffWindow = false;
    bool showSaveAsDialog = false;
    bool showJumpDialog = false;
    bool showPreferencesDialog = false;
    bool showAboutDialog = false;
    bool showUnsavedDialog = false;
    PendingDocumentAction pendingAction = PendingDocumentAction::None;
    std::string pendingOpenPath;
    int pendingProcessSnapshotPid = 0;
    MemoryRegion pendingProcessSnapshotRegion;
    ProcessSnapshotState processSnapshot;

    auto rememberOpenedFile = [&](const std::string& path) {
        if (!rememberRecentFile(preferences, path)) return;
        std::string preferenceStatus;
        if (!savePreferences(preferences, preferenceStatus)) status = preferenceStatus;
    };

    auto performPendingAction = [&]() {
        const PendingDocumentAction action = pendingAction;
        const std::string openPath = pendingOpenPath;
        const int processSnapshotPid = pendingProcessSnapshotPid;
        const MemoryRegion processSnapshotRegion = pendingProcessSnapshotRegion;
        pendingAction = PendingDocumentAction::None;
        pendingOpenPath.clear();
        pendingProcessSnapshotPid = 0;
        pendingProcessSnapshotRegion = {};
        showUnsavedDialog = false;

        switch (action) {
            case PendingDocumentAction::Quit:
                running = false;
                break;
            case PendingDocumentAction::NewDocument:
                newDocument(document, pathBuffer, view, history, status);
                view.editMode = defaultEditMode;
                processSnapshot = {};
                diff = {};
                showDiffWindow = false;
                templateResult = {};
                break;
            case PendingDocumentAction::CloseDocument:
                closeCurrentDocument(document, pathBuffer, view, history, status);
                processSnapshot = {};
                diff = {};
                showDiffWindow = false;
                templateResult = {};
                break;
            case PendingDocumentAction::OpenPath:
                copyToBuffer(pathBuffer, openPath);
                if (openDocument(document, pathBuffer, view, history, status)) {
                    view.editMode = defaultEditMode;
                    processSnapshot = {};
                    diff = {};
                    showDiffWindow = false;
                    templateResult = {};
                    rememberOpenedFile(document.path());
                }
                break;
            case PendingDocumentAction::OpenReadOnlyPath:
                copyToBuffer(pathBuffer, openPath);
                if (openDocument(document, pathBuffer, view, history, status)) {
                    view.editMode = EditMode::ReadOnly;
                    processSnapshot = {};
                    diff = {};
                    showDiffWindow = false;
                    templateResult = {};
                    rememberOpenedFile(document.path());
                }
                break;
            case PendingDocumentAction::OpenProcessSnapshot:
                if (openProcessRegionSnapshot(document, pathBuffer, view, history, processSnapshotPid, processSnapshotRegion, status)) {
                    processSnapshot = ProcessSnapshotState{true, processSnapshotPid, processSnapshotRegion, document.path()};
                    diff = {};
                    showDiffWindow = false;
                    templateResult = {};
                }
                break;
            case PendingDocumentAction::RefreshProcessSnapshot:
                if (processSnapshot.active &&
                    openProcessRegionSnapshot(document, pathBuffer, view, history, processSnapshot.pid, processSnapshot.region, status)) {
                    processSnapshot.path = document.path();
                    status = "Refreshed process " + std::to_string(processSnapshot.pid) + " snapshot 0x" +
                        formatHex(processSnapshot.region.start) + "-0x" + formatHex(processSnapshot.region.end);
                    diff = {};
                    showDiffWindow = false;
                    templateResult = {};
                } else if (!processSnapshot.active) {
                    status = "No process snapshot is open.";
                }
                break;
            case PendingDocumentAction::RevertDocument:
                if (revertDocument(document, pathBuffer, view, history, status)) {
                    processSnapshot = {};
                    diff = {};
                    showDiffWindow = false;
                    templateResult = {};
                }
                break;
            case PendingDocumentAction::None:
                break;
        }
    };

    auto queueDocumentAction = [&](PendingDocumentAction action, const std::string& openPath = std::string()) {
        pendingProcessSnapshotPid = 0;
        pendingProcessSnapshotRegion = {};
        if (document.isModified()) {
            pendingAction = action;
            pendingOpenPath = openPath;
            showUnsavedDialog = true;
        } else {
            pendingAction = action;
            pendingOpenPath = openPath;
            performPendingAction();
        }
    };

    auto queueProcessSnapshotAction = [&](int pid, const MemoryRegion& region) {
        pendingAction = PendingDocumentAction::OpenProcessSnapshot;
        pendingOpenPath.clear();
        pendingProcessSnapshotPid = pid;
        pendingProcessSnapshotRegion = region;
        if (document.isModified()) {
            showUnsavedDialog = true;
        } else {
            performPendingAction();
        }
    };

    const CommandLineOptions commandLine = parseCommandLineOptions(argc, argv);
    if (!commandLine.error.empty()) {
        status = commandLine.error;
    }
    if (!commandLine.diffLeftFile.empty() && !commandLine.diffRightFile.empty()) {
        copyToBuffer(pathBuffer, commandLine.diffLeftFile);
        copyToBuffer(comparePathBuffer, commandLine.diffRightFile);
        if (openDocument(document, pathBuffer, view, history, status)) {
            view.editMode = defaultEditMode;
            rememberOpenedFile(document.path());
            rememberOpenedFile(commandLine.diffRightFile);
            if (computeDiff(document, comparePathBuffer, diff, status)) showDiffWindow = true;
        }
    } else if (!commandLine.filesToOpen.empty()) {
        copyToBuffer(pathBuffer, commandLine.filesToOpen.front());
        if (openDocument(document, pathBuffer, view, history, status)) {
            view.editMode = defaultEditMode;
            rememberOpenedFile(document.path());
        }
    }
    if (commandLine.hasDataToOpen && commandLine.error.empty()) {
        if (openDataDocument(document, commandLine.dataToOpen, pathBuffer, view, history, status)) {
            view.editMode = defaultEditMode;
        }
    }

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) queueDocumentAction(PendingDocumentAction::Quit);
            if (event.type == SDL_DROPFILE && event.drop.file != nullptr) {
                queueDocumentAction(PendingDocumentAction::OpenPath, event.drop.file);
                SDL_free(event.drop.file);
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                queueDocumentAction(PendingDocumentAction::Quit);
            }
        }

        if (document.isOpen()) clampView(document, view, false);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q)) queueDocumentAction(PendingDocumentAction::Quit);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) queueDocumentAction(PendingDocumentAction::NewDocument);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W) && document.isOpen()) queueDocumentAction(PendingDocumentAction::CloseDocument);
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O)) {
            openFileBrowser.initialized = false;
            showOpenDialog = true;
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D) && document.isOpen()) {
            compareFileBrowser.initialized = false;
            showCompareWithFile(view, compareStartBuffer, compareLengthBuffer, compareUseRange, showCompareDialog);
        }
        if (!ImGui::IsAnyItemActive() && showDiffWindow && !diff.ranges.empty() && ImGui::IsKeyPressed(ImGuiKey_F7)) {
            stepDiffRange(document, view, diff, io.KeyShift ? -1 : 1, status);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) view.showFindBanner = true;
        if (view.showFindBanner && ImGui::IsKeyPressed(ImGuiKey_Escape)) view.showFindBanner = false;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G) && document.isOpen()) {
            std::vector<std::uint8_t> pattern;
            if (buildFindPattern(findBuffer, view.findAsHex, pattern, status)) {
                findAndSelect(document, view, pattern, !io.KeyShift, status);
            }
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (io.KeyShift) {
                redoEdit(document, view, history, status);
            } else {
                undoEdit(document, view, history, status);
            }
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L) && document.isOpen()) {
            showJumpToOffset(jumpOffsetBuffer, view, showJumpDialog);
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && document.isOpen()) {
            selectAll(document, view, status);
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && document.isOpen()) {
            copySelection(document, view, io.KeyShift || view.activePane == EditorPane::Ascii, status);
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && document.isOpen()) {
            cutSelection(document, view, history, view.activePane == EditorPane::Ascii, status);
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && document.isOpen()) {
            pasteClipboard(document, view, history, defaultPasteMode(view), status);
        }
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I)) view.editMode = EditMode::Insert;
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O)) view.editMode = EditMode::Overwrite;
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R)) view.editMode = EditMode::ReadOnly;
        if (!ImGui::IsAnyItemActive() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma)) showPreferencesDialog = true;
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S) && document.isOpen()) {
            saveAsFileBrowser.initialized = false;
            showSaveAsDialog = true;
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && document.isOpen()) {
            saveDocument(document, showSaveAsDialog, status);
        }

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Hex Fiend")) {
                if (ImGui::MenuItem("About Hex Fiend")) showAboutDialog = true;
                if (ImGui::MenuItem("Preferences...", "Ctrl+,")) showPreferencesDialog = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quit Hex Fiend", "Ctrl+Q")) queueDocumentAction(PendingDocumentAction::Quit);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New", "Ctrl+N")) queueDocumentAction(PendingDocumentAction::NewDocument);
                if (ImGui::MenuItem("Open File...", "Ctrl+O")) {
                    openFileBrowser.initialized = false;
                    showOpenDialog = true;
                }
                if (ImGui::BeginMenu("Open Recent", !preferences.recentFiles.empty())) {
                    for (const std::string& recentPath : preferences.recentFiles) {
                        if (ImGui::MenuItem(recentPath.c_str())) {
                            queueDocumentAction(PendingDocumentAction::OpenPath, recentPath);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear Recent Files")) {
                        preferences.recentFiles.clear();
                        savePreferences(preferences, status);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Close", "Ctrl+W", false, document.isOpen())) queueDocumentAction(PendingDocumentAction::CloseDocument);
                if (ImGui::MenuItem("Open Drive...")) {
                    devices = scanDevices();
                    drivePathBuffer.fill(0);
                    showOpenDriveDialog = true;
                }
                if (ImGui::MenuItem("Open Process Memory...")) {
                    processes = scanProcesses();
                    processRegions.clear();
                    processFilterBuffer.fill(0);
                    regionFilterBuffer.fill(0);
                    selectedProcess = -1;
                    selectedRegion = -1;
                    showOpenProcessDialog = true;
                }
                if (ImGui::MenuItem("Refresh Process Snapshot", nullptr, false, processSnapshot.active)) {
                    queueDocumentAction(PendingDocumentAction::RefreshProcessSnapshot);
                }
                if (ImGui::MenuItem("Compare with File...", "Shift+Ctrl+D", false, document.isOpen())) {
                    compareFileBrowser.initialized = false;
                    showCompareWithFile(view, compareStartBuffer, compareLengthBuffer, compareUseRange, showCompareDialog);
                }
                if (ImGui::MenuItem("Compare Files...", nullptr, false, !document.isOpen())) {
                    compareLeftPathBuffer.fill(0);
                    comparePathBuffer.fill(0);
                    compareLeftFileBrowser.initialized = false;
                    compareRightFileBrowser.initialized = false;
                    showCompareFilesDialog = true;
                }
                if (ImGui::MenuItem("Next Difference", "F7", false, showDiffWindow && !diff.ranges.empty())) {
                    stepDiffRange(document, view, diff, 1, status);
                }
                if (ImGui::MenuItem("Previous Difference", "Shift+F7", false, showDiffWindow && !diff.ranges.empty())) {
                    stepDiffRange(document, view, diff, -1, status);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S", false, document.isOpen())) {
                    saveDocument(document, showSaveAsDialog, status);
                }
                if (ImGui::MenuItem("Save As...", "Shift+Ctrl+S", false, document.isOpen())) {
                    saveAsFileBrowser.initialized = false;
                    showSaveAsDialog = true;
                }
                if (ImGui::MenuItem("Revert to Saved", nullptr, false, document.isOpen() && !document.path().empty())) {
                    queueDocumentAction(PendingDocumentAction::RevertDocument);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, document.isOpen() && !history.undo.empty())) undoEdit(document, view, history, status);
                if (ImGui::MenuItem("Redo", "Shift+Ctrl+Z", false, document.isOpen() && !history.redo.empty())) redoEdit(document, view, history, status);
                ImGui::Separator();
                if (ImGui::MenuItem("Cut", "Ctrl+X", false, document.isOpen())) cutSelection(document, view, history, view.activePane == EditorPane::Ascii, status);
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, document.isOpen())) copySelection(document, view, view.activePane == EditorPane::Ascii, status);
                if (ImGui::MenuItem("Copy as ASCII", "Shift+Ctrl+C", false, document.isOpen())) copySelection(document, view, true, status);
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, document.isOpen())) pasteClipboard(document, view, history, defaultPasteMode(view), status);
                if (ImGui::MenuItem("Paste as Hex", nullptr, false, document.isOpen())) pasteClipboard(document, view, history, PasteMode::Hex, status);
                if (ImGui::MenuItem("Paste as Text", nullptr, false, document.isOpen())) pasteClipboard(document, view, history, PasteMode::Text, status);
                if (ImGui::MenuItem("Delete", nullptr, false, document.isOpen())) deleteSelection(document, view, history, false, status);
                if (ImGui::MenuItem("Select All", "Ctrl+A", false, document.isOpen())) selectAll(document, view, status);
                ImGui::Separator();
                if (ImGui::BeginMenu("Mode")) {
                    if (ImGui::MenuItem("Read-only", "Shift+Ctrl+R", view.editMode == EditMode::ReadOnly)) view.editMode = EditMode::ReadOnly;
                    if (ImGui::MenuItem("Overwrite", "Shift+Ctrl+O", view.editMode == EditMode::Overwrite)) view.editMode = EditMode::Overwrite;
                    if (ImGui::MenuItem("Insert", "Shift+Ctrl+I", view.editMode == EditMode::Insert)) view.editMode = EditMode::Insert;
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Jump to Offset", "Ctrl+L", false, document.isOpen())) {
                    showJumpToOffset(jumpOffsetBuffer, view, showJumpDialog);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Find", "Ctrl+F")) view.showFindBanner = true;
                if (ImGui::MenuItem("Find Next", "Ctrl+G", false, document.isOpen())) {
                    std::vector<std::uint8_t> pattern;
                    if (buildFindPattern(findBuffer, view.findAsHex, pattern, status)) findAndSelect(document, view, pattern, true, status);
                }
                if (ImGui::MenuItem("Find Previous", "Shift+Ctrl+G", false, document.isOpen())) {
                    std::vector<std::uint8_t> pattern;
                    if (buildFindPattern(findBuffer, view.findAsHex, pattern, status)) findAndSelect(document, view, pattern, false, status);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                bool persistentViewChanged = false;
                if (ImGui::MenuItem("Dark Theme", nullptr, preferences.darkTheme)) {
                    preferences.darkTheme = !preferences.darkTheme;
                    configureStyle(preferences.darkTheme);
                    savePreferences(preferences, status);
                }
                ImGui::Separator();
                ImGui::MenuItem("Column View", nullptr, &view.showColumnHeader);
                persistentViewChanged |= ImGui::MenuItem("Line Numbers", nullptr, &view.showLineNumbers);
                persistentViewChanged |= ImGui::MenuItem("Hexadecimal", nullptr, &view.showHex);
                persistentViewChanged |= ImGui::MenuItem("Text", nullptr, &view.showAscii);
                persistentViewChanged |= ImGui::MenuItem("Data Inspector", nullptr, &view.showDataInspector);
                ImGui::MenuItem("Binary Templates", nullptr, &view.showBinaryTemplates);
                persistentViewChanged |= ImGui::MenuItem("Status Bar", nullptr, &view.showStatusBar);
                persistentViewChanged |= ImGui::MenuItem("Scroller", nullptr, &view.showScroller);
                if (persistentViewChanged) {
                    ensureByteRepresenterVisible(view);
                    capturePreferences(view, defaultEditMode, preferences);
                    savePreferences(preferences, status);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (showOpenDialog) ImGui::OpenPopup("Open File");
        if (showOpenDriveDialog) ImGui::OpenPopup("Open Drive");
        if (showOpenProcessDialog) ImGui::OpenPopup("Open Process Memory");
        if (showCompareDialog) ImGui::OpenPopup("Compare with File");
        if (showCompareFilesDialog) ImGui::OpenPopup("Compare Files");
        if (showSaveAsDialog) ImGui::OpenPopup("Save As");
        if (showJumpDialog) ImGui::OpenPopup("Jump to Offset");
        if (showPreferencesDialog) ImGui::OpenPopup("Preferences");
        if (showAboutDialog) ImGui::OpenPopup("About Hex Fiend");
        if (showUnsavedDialog) ImGui::OpenPopup("Unsaved Changes");
        showOpenDialog = false;
        showOpenDriveDialog = false;
        showOpenProcessDialog = false;
        showCompareDialog = false;
        showCompareFilesDialog = false;
        showSaveAsDialog = false;
        showJumpDialog = false;
        showPreferencesDialog = false;
        showAboutDialog = false;
        showUnsavedDialog = false;

        if (ImGui::BeginPopupModal("Open File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(520.0f);
            const bool submitOpen = ImGui::InputText("Path", pathBuffer.data(), pathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            const bool pickedOpenPath = drawFileBrowser("open-file-browser", openFileBrowser, pathBuffer, false);
            if (pickedOpenPath || submitOpen || ImGui::Button("Open", ImVec2(90.0f, 0.0f))) {
                if (handleFileBrowserAction(openFileBrowser, pathBuffer, false, status)) {
                    queueDocumentAction(PendingDocumentAction::OpenPath, pathBuffer.data());
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
                pendingAction = PendingDocumentAction::None;
                pendingOpenPath.clear();
                pendingProcessSnapshotPid = 0;
                pendingProcessSnapshotRegion = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Open Drive", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::Button("Refresh", ImVec2(90.0f, 0.0f))) devices = scanDevices();
            ImGui::SameLine();
            ImGui::TextDisabled("%zu device%s", devices.size(), devices.size() == 1 ? "" : "s");

            if (ImGui::BeginTable("open-drive-devices", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(520.0f, 190.0f))) {
                ImGui::TableSetupColumn("BSD Name", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const DeviceEntry& device : devices) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool selected = std::strcmp(drivePathBuffer.data(), device.path.c_str()) == 0;
                    if (ImGui::Selectable(device.path.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        copyToBuffer(drivePathBuffer, device.path);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(device.label.empty() ? "-" : device.label.c_str());
                }
                ImGui::EndTable();
            }

            ImGui::SetNextItemWidth(520.0f);
            const bool submitDrive = ImGui::InputText("Path", drivePathBuffer.data(), drivePathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (submitDrive || ImGui::Button("Select", ImVec2(90.0f, 0.0f))) {
                queueDocumentAction(PendingDocumentAction::OpenReadOnlyPath, drivePathBuffer.data());
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Open Process Memory", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::Button("Refresh", ImVec2(90.0f, 0.0f))) {
                processes = scanProcesses();
                processRegions.clear();
                processFilterBuffer.fill(0);
                regionFilterBuffer.fill(0);
                selectedProcess = -1;
                selectedRegion = -1;
            }
            ImGui::SameLine();
            if (ImGui::InputText("Filter", processFilterBuffer.data(), processFilterBuffer.size())) {
                selectedProcess = -1;
                selectedRegion = -1;
                processRegions.clear();
            }
            std::size_t visibleProcesses = 0;
            for (const ProcessEntry& process : processes) {
                if (processMatchesFilter(process, processFilterBuffer.data())) visibleProcesses++;
            }
            ImGui::TextDisabled("%zu of %zu process%s",
                                visibleProcesses,
                                processes.size(),
                                processes.size() == 1 ? "" : "es");

            if (ImGui::BeginTable("open-process-list", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(640.0f, 150.0f))) {
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < processes.size(); ++i) {
                    const ProcessEntry& process = processes[i];
                    if (!processMatchesFilter(process, processFilterBuffer.data())) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool selected = selectedProcess == static_cast<int>(i);
                    if (ImGui::Selectable(std::to_string(process.pid).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        selectedProcess = static_cast<int>(i);
                        selectedRegion = -1;
                        processRegions = scanProcessRegions(process.pid);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(process.name.c_str());
                }
                ImGui::EndTable();
            }

            ImGui::TextDisabled("%zu readable region%s", processRegions.size(), processRegions.size() == 1 ? "" : "s");
            if (ImGui::InputText("Region filter", regionFilterBuffer.data(), regionFilterBuffer.size())) {
                selectedRegion = -1;
            }
            std::size_t visibleRegions = 0;
            for (const MemoryRegion& region : processRegions) {
                if (memoryRegionMatchesFilter(region, regionFilterBuffer.data())) visibleRegions++;
            }
            ImGui::TextDisabled("%zu of %zu readable region%s",
                                visibleRegions,
                                processRegions.size(),
                                processRegions.size() == 1 ? "" : "s");
            if (ImGui::BeginTable("open-process-regions", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(640.0f, 190.0f))) {
                ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Perms", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < processRegions.size(); ++i) {
                    const MemoryRegion& region = processRegions[i];
                    if (!memoryRegionMatchesFilter(region, regionFilterBuffer.data())) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool selected = selectedRegion == static_cast<int>(i);
                    const std::string start = "0x" + formatHex(region.start);
                    if (ImGui::Selectable(start.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        selectedRegion = static_cast<int>(i);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(formatBytes(region.end - region.start).c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(region.permissions.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(region.name.c_str());
                }
                ImGui::EndTable();
            }

            const bool canOpenProcessRegion =
                selectedProcess >= 0 && selectedProcess < static_cast<int>(processes.size()) &&
                selectedRegion >= 0 && selectedRegion < static_cast<int>(processRegions.size());
            if (!canOpenProcessRegion) ImGui::BeginDisabled();
            if (ImGui::Button("Open Snapshot", ImVec2(120.0f, 0.0f))) {
                const ProcessEntry& process = processes[static_cast<std::size_t>(selectedProcess)];
                const MemoryRegion& region = processRegions[static_cast<std::size_t>(selectedRegion)];
                queueProcessSnapshotAction(process.pid, region);
                ImGui::CloseCurrentPopup();
            }
            if (!canOpenProcessRegion) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Compare with File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(520.0f);
            bool submitCompare = ImGui::InputText("Path", comparePathBuffer.data(), comparePathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            submitCompare |= drawFileBrowser("compare-file-browser", compareFileBrowser, comparePathBuffer, false);
            ImGui::Checkbox("Compare range", &compareUseRange);
            if (compareUseRange) {
                ImGui::SetNextItemWidth(180.0f);
                submitCompare |= ImGui::InputText("Start", compareStartBuffer.data(), compareStartBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(180.0f);
                submitCompare |= ImGui::InputText("Length", compareLengthBuffer.data(), compareLengthBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            }
            if (submitCompare || ImGui::Button("Compare", ImVec2(90.0f, 0.0f))) {
                bool compared = false;
                if (!handleFileBrowserAction(compareFileBrowser, comparePathBuffer, false, status)) {
                    compared = false;
                } else if (compareUseRange) {
                    std::uint64_t start = 0;
                    std::uint64_t length = 0;
                    if (!parseOffset(compareStartBuffer.data(), start) || !parseOffset(compareLengthBuffer.data(), length)) {
                        status = "Enter decimal or hexadecimal range values.";
                    } else {
                        compared = computeDiffRange(document, comparePathBuffer, start, length, diff, status);
                    }
                } else {
                    compared = computeDiff(document, comparePathBuffer, diff, status);
                }
                if (compared) {
                    showDiffWindow = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Compare Files", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(520.0f);
            bool submitCompareFiles = ImGui::InputText("Left", compareLeftPathBuffer.data(), compareLeftPathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            drawFileBrowser("compare-left-file-browser", compareLeftFileBrowser, compareLeftPathBuffer, false);
            ImGui::SetNextItemWidth(520.0f);
            submitCompareFiles |= ImGui::InputText("Right", comparePathBuffer.data(), comparePathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            submitCompareFiles |= drawFileBrowser("compare-right-file-browser", compareRightFileBrowser, comparePathBuffer, false);
            if (submitCompareFiles || ImGui::Button("Compare", ImVec2(90.0f, 0.0f))) {
                const bool leftReady = handleFileBrowserAction(compareLeftFileBrowser, compareLeftPathBuffer, false, status);
                const bool rightReady = handleFileBrowserAction(compareRightFileBrowser, comparePathBuffer, false, status);
                if (leftReady && rightReady &&
                    openFilesForComparison(document, pathBuffer, compareLeftPathBuffer, comparePathBuffer, view, history, diff, defaultEditMode, status)) {
                    showDiffWindow = true;
                    templateResult = {};
                    rememberOpenedFile(document.path());
                    rememberOpenedFile(comparePathBuffer.data());
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("General");
            bool preferencesChanged = false;
            bool themeChanged = false;
            themeChanged = ImGui::Checkbox("Dark theme", &preferences.darkTheme);
            if (themeChanged) {
                configureStyle(preferences.darkTheme);
                preferencesChanged = true;
            }
            const char* editModeLabels[] = {"Insert", "Overwrite", "Read-Only"};
            int defaultModeIndex = editModeIndex(defaultEditMode);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Default edit mode for opening files", &defaultModeIndex, editModeLabels, 3)) {
                defaultEditMode = editModeFromIndex(defaultModeIndex);
                preferencesChanged = true;
            }
            int bytesPerRow = static_cast<int>(view.bytesPerRow);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputInt("Bytes per row", &bytesPerRow)) {
                view.bytesPerRow = static_cast<std::uint64_t>(std::clamp(bytesPerRow, 4, 64));
                preferencesChanged = true;
            }
            ImGui::Separator();
            preferencesChanged |= ImGui::Checkbox("Line numbers", &view.showLineNumbers);
            preferencesChanged |= ImGui::Checkbox("Hexadecimal view", &view.showHex);
            preferencesChanged |= ImGui::Checkbox("Text view", &view.showAscii);
            preferencesChanged |= ImGui::Checkbox("Data inspector", &view.showDataInspector);
            preferencesChanged |= ImGui::Checkbox("Status bar", &view.showStatusBar);
            preferencesChanged |= ImGui::Checkbox("Scroller", &view.showScroller);
            if (preferencesChanged) {
                ensureByteRepresenterVisible(view);
                capturePreferences(view, defaultEditMode, preferences);
                savePreferences(preferences, status);
            }
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("About Hex Fiend", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Hex Fiend");
            ImGui::Separator();
            ImGui::TextUnformatted("Linux ImGui port");
            ImGui::Text("Backend: SDL2, OpenGL, HexFiendCore");
            ImGui::Text("Document: %s", document.isOpen() ? (document.path().empty() ? "Untitled" : document.path().c_str()) : "None");
            if (document.isOpen()) ImGui::Text("Length: %s", formatBytes(document.length()).c_str());
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string unsavedName = document.path().empty() ? "Untitled" : document.path();
            ImGui::Text("Save changes to %s?", unsavedName.c_str());
            ImGui::TextDisabled("Your changes will be lost if you discard them.");
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(90.0f, 0.0f))) {
                saveDocument(document, showSaveAsDialog, status);
                if (!document.isModified()) performPendingAction();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(90.0f, 0.0f))) {
                performPendingAction();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
                pendingAction = PendingDocumentAction::None;
                pendingOpenPath.clear();
                pendingProcessSnapshotPid = 0;
                pendingProcessSnapshotRegion = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Save As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(520.0f);
            const bool submitSaveAs = ImGui::InputText("Path", pathBuffer.data(), pathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            drawFileBrowser("save-as-file-browser", saveAsFileBrowser, pathBuffer, false);
            if (submitSaveAs || ImGui::Button("Save", ImVec2(90.0f, 0.0f))) {
                if (!handleFileBrowserAction(saveAsFileBrowser, pathBuffer, false, status)) {
                    status = "Choose a file name.";
                } else if (document.saveAs(pathBuffer.data())) {
                    status = "Saved " + document.path();
                    rememberOpenedFile(document.path());
                    if (pendingAction != PendingDocumentAction::None) performPendingAction();
                    ImGui::CloseCurrentPopup();
                } else {
                    status = document.error();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Jump to Offset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(220.0f);
            const bool submitJump = ImGui::InputText("Offset", jumpOffsetBuffer.data(), jumpOffsetBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (submitJump || ImGui::Button("Jump", ImVec2(90.0f, 0.0f))) {
                if (jumpToOffset(document, view, jumpOffsetBuffer, status)) ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::SetNextWindowPos(ImVec2(0.0f, ImGui::GetFrameHeight()), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - ImGui::GetFrameHeight()), ImGuiCond_Always);
        const std::string documentName = document.path().empty() ? "Untitled" : document.path();
        const std::string title = document.isOpen()
            ? (document.isModified() ? "*" : "") + documentName + editModeTitle(view.editMode)
            : "Untitled";
        ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        if (view.showFindBanner) {
            drawFindBanner(document, view, history, findBuffer, replaceBuffer, status);
        }

        if (!document.isOpen()) {
            const float buttonWidth = 170.0f;
            const float buttonHeight = 0.0f;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const int columns = availableWidth >= buttonWidth * 4.0f + gap * 3.0f ? 4 :
                                availableWidth >= buttonWidth * 2.0f + gap ? 2 : 1;
            const float rowWidth = buttonWidth * static_cast<float>(columns) + gap * static_cast<float>(columns - 1);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 18.0f);
            if (availableWidth > rowWidth) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - rowWidth) * 0.5f);
            }

            if (ImGui::Button("New", ImVec2(buttonWidth, buttonHeight))) {
                queueDocumentAction(PendingDocumentAction::NewDocument);
            }
            if (columns > 1) ImGui::SameLine();
            if (ImGui::Button("Open File...", ImVec2(buttonWidth, buttonHeight))) {
                openFileBrowser.initialized = false;
                showOpenDialog = true;
            }
            if (columns > 2) {
                ImGui::SameLine();
            } else if (columns == 2 && availableWidth > rowWidth) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - rowWidth) * 0.5f);
            }
            if (ImGui::Button("Open Drive...", ImVec2(buttonWidth, buttonHeight))) {
                devices = scanDevices();
                drivePathBuffer.fill(0);
                showOpenDriveDialog = true;
            }
            if (columns > 1) ImGui::SameLine();
            if (ImGui::Button("Open Process Memory...", ImVec2(buttonWidth, buttonHeight))) {
                processes = scanProcesses();
                processRegions.clear();
                processFilterBuffer.fill(0);
                regionFilterBuffer.fill(0);
                selectedProcess = -1;
                selectedRegion = -1;
                showOpenProcessDialog = true;
            }
            if (availableWidth > buttonWidth) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - buttonWidth) * 0.5f);
            }
            if (ImGui::Button("Compare Files...", ImVec2(buttonWidth, buttonHeight))) {
                compareLeftPathBuffer.fill(0);
                comparePathBuffer.fill(0);
                compareLeftFileBrowser.initialized = false;
                compareRightFileBrowser.initialized = false;
                showCompareFilesDialog = true;
            }
            if (!preferences.recentFiles.empty()) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14.0f);
                const float recentWidth = std::min(620.0f, ImGui::GetContentRegionAvail().x);
                if (ImGui::GetContentRegionAvail().x > recentWidth) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - recentWidth) * 0.5f);
                }
                if (ImGui::BeginTable("empty-recent-files", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(recentWidth, 150.0f))) {
                    ImGui::TableSetupColumn("Recent Files", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for (const std::string& recentPath : preferences.recentFiles) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Selectable(recentPath.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                            queueDocumentAction(PendingDocumentAction::OpenPath, recentPath);
                        }
                    }
                    ImGui::EndTable();
                }
            }
            } else {
                const float inspectorHeight = view.showDataInspector ? kDataInspectorHeight : 0.0f;
                const float statusHeight = view.showStatusBar ? 24.0f : 0.0f;
                const float templateWidth = view.showBinaryTemplates ? 266.0f : 0.0f;
                const float toolbarHeight = documentToolbarHeight(ImGui::GetContentRegionAvail().x);
                drawDocumentToolbar(document, view, history, diff, compareStartBuffer, compareLengthBuffer, jumpOffsetBuffer,
                                    compareUseRange, showCompareDialog, showJumpDialog, showSaveAsDialog, status);
                ImGui::BeginChild("representer-area", ImVec2(-templateWidth, -inspectorHeight - statusHeight - toolbarHeight), false);
            drawHexRepresenters(document, view, status);
            ImGui::EndChild();
            if (view.showBinaryTemplates) {
                ImGui::SameLine();
                drawBinaryTemplatePanel(document, view, templates, selectedTemplate, templateAnchor, templateResult, status);
            }

            if (view.showDataInspector) {
                drawDataInspector(document, view);
            }

            if (view.showStatusBar) {
                ImGui::Separator();
                if (ImGui::BeginTable("status-bar", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
                    ImGui::TableSetupColumn("Selection", ImGuiTableColumnFlags_WidthStretch, 0.52f);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.48f);
                    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 66.0f);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const std::string selectionSummary = statusSelectionSummary(document, view);
                    ImGui::TextUnformatted(selectionSummary.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("%s", status.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (document.isModified()) {
                        ImGui::TextDisabled("modified");
                    }
                    ImGui::EndTable();
                }
            }

            handleEditorKeys(document, view, history, status);
        }

        ImGui::End();

        drawDiffWindow(document, view, diff, showDiffWindow, status);

        ImGui::Render();
        int displayW = 0;
        int displayH = 0;
        SDL_GL_GetDrawableSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        const ImVec4 clearColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
