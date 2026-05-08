#include "EngineDocument.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::vector<unsigned char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const std::string inputPath = "/tmp/hexfiend-linux-engine-smoke-input.bin";
    const std::string outputPath = "/tmp/hexfiend-linux-engine-smoke-output.bin";
    const std::string newOutputPath = "/tmp/hexfiend-linux-engine-smoke-new.bin";

    {
        std::ofstream file(inputPath, std::ios::binary | std::ios::trunc);
        const unsigned char bytes[] = {
            0x48, 0x65, 0x78, 0x20, 0x46, 0x69, 0x65, 0x6E, 0x64,
            0x20, 0x4C, 0x69, 0x6E, 0x75, 0x78
        };
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    hexfiend::linux_ui::EngineDocument document;
    if (!expect(document.open(inputPath), document.error().c_str())) return 1;
    if (!expect(document.isOpen(), "document did not open")) return 1;
    if (!expect(!document.isModified(), "opened document should be clean")) return 1;
    if (!expect(document.length() == 15, "unexpected document length")) return 1;

    std::vector<unsigned char> bytes;
    if (!expect(document.read(0, 3, bytes), "initial read failed")) return 1;
    if (!expect((bytes == std::vector<unsigned char>{0x48, 0x65, 0x78}), "initial read mismatch")) return 1;

    std::uint64_t found = 0;
    if (!expect(document.find(std::vector<unsigned char>{0x46, 0x69, 0x65, 0x6E, 0x64}, 0, true, found), document.error().c_str())) return 1;
    if (!expect(found == 4, "find returned wrong offset")) return 1;

    if (!expect(document.replace(4, 5, std::vector<unsigned char>{0x43, 0x6F, 0x72, 0x65}), document.error().c_str())) return 1;
    if (!expect(document.isModified(), "replace should mark document modified")) return 1;
    if (!expect(document.open(inputPath), document.error().c_str())) return 1;
    if (!expect(!document.isModified(), "reopening document should clear modified state")) return 1;
    if (!expect(document.read(4, 5, bytes), "read after reopen failed")) return 1;
    if (!expect((bytes == std::vector<unsigned char>{0x46, 0x69, 0x65, 0x6E, 0x64}), "reopen did not restore saved bytes")) return 1;

    if (!expect(document.replace(4, 5, std::vector<unsigned char>{0x43, 0x6F, 0x72, 0x65}), document.error().c_str())) return 1;
    if (!expect(document.erase(8, 1), document.error().c_str())) return 1;
    if (!expect(document.saveAs(outputPath), document.error().c_str())) return 1;
    if (!expect(!document.isModified(), "save should clear modified state")) return 1;

    const std::vector<unsigned char> saved = readFile(outputPath);
    const std::vector<unsigned char> expected = {
        0x48, 0x65, 0x78, 0x20, 0x43, 0x6F, 0x72, 0x65,
        0x4C, 0x69, 0x6E, 0x75, 0x78
    };
    if (!expect(saved == expected, "saved bytes mismatch")) return 1;

    if (!expect(document.createEmpty(), document.error().c_str())) return 1;
    if (!expect(document.isOpen(), "new document did not open")) return 1;
    if (!expect(!document.isModified(), "new document should start clean")) return 1;
    if (!expect(document.length() == 0, "new document is not empty")) return 1;
    if (!expect(document.replace(0, 0, std::vector<unsigned char>{0x4E, 0x65, 0x77}), document.error().c_str())) return 1;
    if (!expect(document.isModified(), "editing new document should mark it modified")) return 1;
    if (!expect(document.saveAs(newOutputPath), document.error().c_str())) return 1;
    if (!expect(!document.isModified(), "saving new document should clear modified state")) return 1;
    if (!expect((readFile(newOutputPath) == std::vector<unsigned char>{0x4E, 0x65, 0x77}), "new document saved bytes mismatch")) return 1;

    std::remove(inputPath.c_str());
    std::remove(outputPath.c_str());
    std::remove(newOutputPath.c_str());
    return 0;
}
