/* zip_support.h: ZIP archive support for VICEVita
   
   This file provides functions to handle ZIP archives in the file explorer
   and ROM loading system.
*/

#ifndef ZIP_SUPPORT_H
#define ZIP_SUPPORT_H

#include <stddef.h>  // for size_t

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for ZIP archives - actual implementation is in C++ */
typedef struct ZipArchiveHandle ZipArchiveHandle;

// C-compatible ZIP functions (the only ones exposed to C code)
int zip_is_archive(const char* filename);
ZipArchiveHandle* zip_open_archive(const char* archive_path);
void zip_close_archive(ZipArchiveHandle* archive);
int zip_extract_file(ZipArchiveHandle* archive, const char* filename, char** data, long* size);
int zip_extract_file_to_temp_c(ZipArchiveHandle* archive, const char* filename, char* temp_path, size_t buffer_size);
int zip_file_exists(ZipArchiveHandle* archive, const char* filename);
void zip_cleanup_temp_files(void);
int zip_is_supported_extension(const char* filename);
int zip_list_contents(ZipArchiveHandle* archive, void* entries);
int zip_extract_rom_to_temp_c(const char* zip_path, const char** supported_extensions, char* temp_rom_path, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // ZIP_SUPPORT_H
