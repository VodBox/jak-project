#pragma once

/*!
 * @file FileUtil.h
 * Utility functions for reading and writing files.
 */

#include <string>
#include <vector>

namespace file_util {
std::string get_project_path();
std::string get_file_path(const std::vector<std::string>& input);
bool create_dir_if_needed(const std::string& path);
void write_binary_file(const std::string& name, void* data, size_t size);
void write_rgba_png(const std::string& name, void* data, int w, int h);
void write_text_file(const std::string& file_name, const std::string& text);
std::vector<uint8_t> read_binary_file(const std::string& filename);
std::string read_text_file(const std::string& path);
bool is_printable_char(char c);
std::string combine_path(const std::string& parent, const std::string& child);
std::string base_name(const std::string& filename);
void init_crc();
uint32_t crc32(const uint8_t* data, size_t size);
uint32_t crc32(const std::vector<uint8_t>& data);
void MakeISOName(char* dst, const char* src);
void ISONameFromAnimationName(char* dst, const char* src);
void assert_file_exists(const char* path, const char* error_message);
}  // namespace file_util
