// SPDX-License-Identifier: Apache-2.0
// Controlled native-crash drill: wires the same local-only Crashpad pipeline as
// the server, then dereferences a null pointer so a minidump drill can verify
// the handler end to end.
#include <crashpad/client/crash_report_database.h>
#include <crashpad/client/crashpad_client.h>
#include <crashpad/client/settings.h>
#include <base/files/file_path.h>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int crash(const std::filesystem::path& handler, const std::filesystem::path& database) {
    namespace fs = std::filesystem;
    fs::create_directories(database);
    fs::create_directories(database / "handler-metrics");
    auto reports = crashpad::CrashReportDatabase::Initialize(base::FilePath(database.string()));
    if (!reports || !reports->GetSettings()->SetUploadsEnabled(false)) {
        std::cerr << "cannot initialize local crash database\n";
        return 2;
    }
    crashpad::CrashpadClient client;
    if (!client.StartHandler(base::FilePath(handler.string()), base::FilePath(database.string()),
        base::FilePath((database / "handler-metrics").string()), "",
        {{"product", "mistlane-crash-sample"}}, {}, true, false)) {
        std::cerr << "cannot start crashpad handler\n";
        return 3;
    }
    std::cout << "crashing" << std::endl;
    int* invalid = nullptr;
    return *invalid;
}
}

int main(int argc, char** argv) {
    std::filesystem::path handler, database;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 < argc && key == "--crashpad-handler") handler = argv[++i];
        else if (i + 1 < argc && key == "--crashpad-database") database = argv[++i];
        else {
            std::cerr << "usage: mistlane-crash-sample --crashpad-handler FILE --crashpad-database DIR\n";
            return 1;
        }
    }
    if (handler.empty() || database.empty()) {
        std::cerr << "both --crashpad-handler and --crashpad-database are required\n";
        return 1;
    }
    return crash(handler, database);
}
