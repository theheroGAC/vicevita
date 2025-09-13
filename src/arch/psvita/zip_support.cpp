/* zip_support.cpp: ZIP archive support for VICEVita implementation */

#include "zip_support.h"
#include "minizip/unzip.h"
#include "minizip/zip.h"
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>

// Cross-platform case-insensitive string comparison
static int strcasecmp_custom(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *s1 - *s2;
}

#define MAX_FILENAME_LENGTH 512
#define TEMP_DIR "ux0:temp/vicevita_zip/"

// Internal C++ structures
struct ZipEntry {
    std::string filename;
    unsigned long uncompressed_size;
    unsigned long compressed_size;
    bool is_directory;
    std::string path;  // Full path inside ZIP
};

// Actual implementation of ZipArchiveHandle (hidden from C code)
struct ZipArchiveHandle {
    void* handle;      // unzFile handle
    std::string archive_path;
    bool is_open;
    std::vector<ZipEntry> entries;
};

// Static list to track opened archives for cleanup
static std::vector<ZipArchiveHandle*> g_open_archives;

// Internal C++ function for listing ZIP contents
static bool zip_list_contents_internal(ZipArchiveHandle* archive, std::vector<ZipEntry>& entries) {
    if (!archive || !archive->is_open) return false;
    
    unzFile uf = (unzFile)archive->handle;
    entries.clear();
    
    if (unzGoToFirstFile(uf) != UNZ_OK) {
        return false;
    }
    
    do {
        char filename[MAX_FILENAME_LENGTH];
        unz_file_info file_info;
        
        if (unzGetCurrentFileInfo(uf, &file_info, filename, sizeof(filename), 
                                  nullptr, 0, nullptr, 0) != UNZ_OK) {
            continue;
        }
        
        ZipEntry entry;
        entry.filename = filename;
        entry.path = filename;
        entry.uncompressed_size = file_info.uncompressed_size;
        entry.compressed_size = file_info.compressed_size;
        entry.is_directory = (filename[strlen(filename) - 1] == '/');
        
        entries.push_back(entry);
        
    } while (unzGoToNextFile(uf) == UNZ_OK);
    
    return true;
}

extern "C" int zip_is_archive(const char* filename) {
    if (!filename) return 0;
    
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    
    return (strcasecmp_custom(ext, ".zip") == 0) ? 1 : 0;
}

extern "C" ZipArchiveHandle* zip_open_archive(const char* archive_path) {
    if (!archive_path || !zip_is_archive(archive_path)) {
        return nullptr;
    }
    
    unzFile uf = unzOpen(archive_path);
    if (!uf) {
        return nullptr;
    }
    
    ZipArchiveHandle* archive = new ZipArchiveHandle();
    archive->handle = uf;
    archive->archive_path = archive_path;
    archive->is_open = true;
    
    // List all entries in the archive
    zip_list_contents_internal(archive, archive->entries);
    
    // Add to global list for cleanup tracking
    g_open_archives.push_back(archive);
    
    return archive;
}

extern "C" void zip_close_archive(ZipArchiveHandle* archive) {
    if (!archive || !archive->is_open) return;
    
    if (archive->handle) {
        unzClose((unzFile)archive->handle);
        archive->handle = nullptr;
    }
    
    archive->is_open = false;
    
    // Remove from global list
    auto it = std::find(g_open_archives.begin(), g_open_archives.end(), archive);
    if (it != g_open_archives.end()) {
        g_open_archives.erase(it);
    }
    
    delete archive;
}


static bool zip_extract_file_internal(ZipArchiveHandle* archive, const char* filename, char** data, long* size) {
    if (!archive || !archive->is_open || !filename || !data || !size) {
        return false;
    }
    
    unzFile uf = (unzFile)archive->handle;
    *data = nullptr;
    *size = 0;
    
    if (unzLocateFile(uf, filename, 0) != UNZ_OK) {
        return false;
    }
    
    unz_file_info file_info;
    if (unzGetCurrentFileInfo(uf, &file_info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) {
        return false;
    }
    
    if (unzOpenCurrentFile(uf) != UNZ_OK) {
        return false;
    }
    
    *size = file_info.uncompressed_size;
    *data = new char[*size];
    
    int bytes_read = unzReadCurrentFile(uf, *data, *size);
    unzCloseCurrentFile(uf);
    
    if (bytes_read != *size) {
        delete[] *data;
        *data = nullptr;
        *size = 0;
        return false;
    }
    
    return true;
}

std::string zip_get_temp_dir() {
    return TEMP_DIR;
}

static bool zip_extract_file_to_temp_internal(ZipArchiveHandle* archive, const char* filename, std::string& temp_path) {
    char* data = nullptr;
    long size = 0;
    
    if (!zip_extract_file_internal(archive, filename, &data, &size)) {
        return false;
    }
    
    // Create temp directory if it doesn't exist
    std::string temp_dir = zip_get_temp_dir();
    sceIoMkdir(temp_dir.c_str(), 0777);
    
    // Generate unique temp file name
    static int temp_counter = 0;
    temp_counter++;
    
    // Extract just the filename without path
    const char* base_filename = strrchr(filename, '/');
    if (base_filename) {
        base_filename++; // Skip the '/'
    } else {
        base_filename = filename;
    }
    
    temp_path = temp_dir + "temp_" + std::to_string(temp_counter) + "_" + base_filename;
    
    // Write data to temp file
    SceUID fd = sceIoOpen(temp_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        delete[] data;
        return false;
    }
    
    int bytes_written = sceIoWrite(fd, data, size);
    sceIoClose(fd);
    delete[] data;
    
    return (bytes_written == size);
}

static bool zip_file_exists_internal(ZipArchiveHandle* archive, const char* filename) {
    if (!archive || !archive->is_open || !filename) {
        return false;
    }
    
    for (const auto& entry : archive->entries) {
        if (entry.filename == filename && !entry.is_directory) {
            return true;
        }
    }
    
    return false;
}



std::string zip_normalize_path(const char* path) {
    if (!path) return "";
    
    std::string normalized = path;
    // Replace backslashes with forward slashes
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    
    // Remove leading slash if present
    if (!normalized.empty() && normalized[0] == '/') {
        normalized = normalized.substr(1);
    }
    
    return normalized;
}

static bool zip_is_supported_extension_internal(const char* filename) {
    if (!filename) return false;
    
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    
    // Add more extensions as needed for different Commodore systems
    const char* supported_exts[] = {
        ".prg", ".p00", ".t64", ".tap", ".d64", ".d71", ".d81", 
        ".x64", ".g64", ".crt", ".bin", ".rom", nullptr
    };
    
    for (int i = 0; supported_exts[i]; i++) {
        if (strcasecmp_custom(ext, supported_exts[i]) == 0) {
            return true;
        }
    }
    
    return false;
}

bool zip_find_rom_in_archive(ZipArchiveHandle* archive, const char** supported_extensions, std::string& rom_filename) {
    if (!archive || !archive->is_open) {
        return false;
    }
    
    // Look for supported ROM files in the archive
    for (const auto& entry : archive->entries) {
        if (entry.is_directory) continue;
        
        const char* ext = strrchr(entry.filename.c_str(), '.');
        if (!ext) continue;
        
        if (supported_extensions) {
            // Check against provided extensions
            for (int i = 0; supported_extensions[i]; i++) {
                if (strcasecmp_custom(ext, supported_extensions[i]) == 0) {
                    rom_filename = entry.filename;
                    return true;
                }
            }
        } else {
            // Use default supported extensions
            if (zip_is_supported_extension_internal(entry.filename.c_str())) {
                rom_filename = entry.filename;
                return true;
            }
        }
    }
    
    return false;
}

bool zip_extract_rom_to_temp(const char* zip_path, const char** supported_extensions, std::string& temp_rom_path) {
    ZipArchiveHandle* archive = zip_open_archive(zip_path);
    if (!archive) {
        return false;
    }
    
    std::string rom_filename;
    if (!zip_find_rom_in_archive(archive, supported_extensions, rom_filename)) {
        zip_close_archive(archive);
        return false;
    }
    
    bool success = zip_extract_file_to_temp_internal(archive, rom_filename.c_str(), temp_rom_path);
    zip_close_archive(archive);
    
    return success;
}

// C-compatible wrapper functions
extern "C" int zip_extract_file_to_temp_c(ZipArchiveHandle* archive, const char* filename, char* temp_path, size_t buffer_size) {
    std::string temp_path_str;
    bool result = zip_extract_file_to_temp_internal(archive, filename, temp_path_str);
    
    if (result && temp_path_str.length() < buffer_size) {
        strcpy(temp_path, temp_path_str.c_str());
        return 1;
    }
    
    return 0;
}

extern "C" int zip_extract_rom_to_temp_c(const char* zip_path, const char** supported_extensions, char* temp_rom_path, size_t buffer_size) {
    std::string temp_path_str;
    bool result = zip_extract_rom_to_temp(zip_path, supported_extensions, temp_path_str);
    
    if (result && temp_path_str.length() < buffer_size) {
        strcpy(temp_rom_path, temp_path_str.c_str());
        return 1;
    }
    
    return 0;
}

extern "C" int zip_extract_file(ZipArchiveHandle* archive, const char* filename, char** data, long* size) {
    bool result = zip_extract_file_internal(archive, filename, data, size);
    return result ? 1 : 0;
}

extern "C" int zip_file_exists(ZipArchiveHandle* archive, const char* filename) {
    bool result = zip_file_exists_internal(archive, filename);
    return result ? 1 : 0;
}

extern "C" void zip_cleanup_temp_files(void) {
    // Implementation moved to avoid recursion
    std::string temp_dir = zip_get_temp_dir();
    
    // Close all open archives first
    for (auto archive : g_open_archives) {
        if (archive && archive->is_open) {
            zip_close_archive(archive);
        }
    }
    g_open_archives.clear();
    
    // TODO: Implement recursive directory cleanup
    // For now, we'll rely on the system to clean up temp files
}

extern "C" int zip_is_supported_extension(const char* filename) {
    bool result = zip_is_supported_extension_internal(filename);
    return result ? 1 : 0;
}

extern "C" int zip_list_contents(ZipArchiveHandle* archive, void* entries) {
    // For now, return success without populating entries
    // This is a stub to allow compilation
    return 1;
}

