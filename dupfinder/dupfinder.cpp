#include <iostream>
#include <filesystem>
#include <unordered_map>
#include <fstream>
#include <vector>
#include <openssl/sha.h> // Requires OpenSSL for SHA-256 hashing
#include <locale>
#include <iomanip>

namespace fs = std::filesystem;

// Helper function to compute SHA-256 hash of a file
std::wstring compute_file_hash(const fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path.string());
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    constexpr size_t buffer_size = 8192;
    char buffer[buffer_size];
    // size_t total_read = 0;
    while (file.read(buffer, buffer_size)) {
        SHA256_Update(&ctx, buffer, file.gcount());
        // total_read += file.gcount();
        // std::wcout << L"\rHashing: " << file_path.wstring() << L" (" << total_read << L" bytes processed)" << std::flush;
    }
    // Update for any remaining bytes
    SHA256_Update(&ctx, buffer, file.gcount());
    // total_read += file.gcount();
    // std::wcout << L"\rHashing: " << file_path.wstring() << L" (" << total_read << L" bytes processed)" << std::flush;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    std::wstringstream result;
    for (unsigned char c : hash) {
        result << std::setw(2) << std::setfill(L'0') << std::hex << static_cast<int>(c);
    }
    std::wcout << L"\rHashing completed: " << file_path.wstring() << L"                    " << std::endl;
    return result.str();
}

// Function to find duplicate files by hash
std::unordered_map<std::wstring, std::vector<fs::path>> find_duplicate_files(const fs::path& root) {
    std::unordered_map<std::wstring, std::vector<fs::path>> hash_to_files;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            try {
                auto hash = compute_file_hash(entry.path());
                hash_to_files[hash].push_back(entry.path());
            }
            catch (const std::exception& e) {
                std::wcerr << L"Error processing file " << entry.path().wstring() << L": " << e.what() << L'\n';
            }
        }
    }

    // Remove entries with only one file (unique files)
    for (auto it = hash_to_files.begin(); it != hash_to_files.end();) {
        if (it->second.size() < 2) {
            it = hash_to_files.erase(it);
        }
        else {
            ++it;
        }
    }

    return hash_to_files;
}

int main() {
    fs::path directory_to_scan = L"D:\\WebM"; // Change to your directory
    fs::path duplicates_folder = L"./duplicates";

    try {
        // Set the locale to handle Unicode properly
        std::locale::global(std::locale("en_US.UTF-8"));

        // Create the duplicates folder if it doesn't exist
        if (!fs::exists(duplicates_folder)) {
            fs::create_directory(duplicates_folder);
        }

        auto duplicates = find_duplicate_files(directory_to_scan);

        for (const auto& [hash, files] : duplicates) {
            std::wcout << L"Duplicate files (Hash: " << hash << L"):\n";

            // Move the first file to the duplicates folder
            auto first_file = files.front();
            fs::path destination = duplicates_folder / first_file.filename();

            // Ensure unique naming if file already exists in duplicates folder
            int counter = 1;
            while (fs::exists(destination)) {
                destination = duplicates_folder / (first_file.stem().wstring() + L"_" + std::to_wstring(counter) + first_file.extension().wstring());
                ++counter;
            }

            try {
                fs::copy(first_file, destination, fs::copy_options::overwrite_existing);
                std::wcout << L"  Copied: " << first_file.wstring() << L" -> " << destination.wstring() << L'\n';
            }
            catch (const std::exception& e) {
                std::wcerr << L"  Error copying " << first_file.wstring() << L": " << e.what() << L'\n';
                continue;
            }

            // Delete all duplicate files (including the first one from original location)
            for (const auto& file : files) {
                try {
                    fs::remove(file);
                    std::wcout << L"  Deleted: " << file.wstring() << L'\n';
                }
                catch (const std::exception& e) {
                    std::wcerr << L"  Error deleting " << file.wstring() << L": " << e.what() << L'\n';
                }
            }
        }

        if (duplicates.empty()) {
            std::wcout << L"No duplicate files found.\n";
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Error: " << e.what() << L'\n';
    }

    return 0;
}
