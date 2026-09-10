/*
 * TestHub - 文件工具
 */

#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace testhub {

/**
 * 文件工具类
 */
class FileUtil {
public:
    /**
     * 读取文件内容
     */
    static std::string readFile(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return "";
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    /**
     * 写入文件内容
     */
    static bool writeFile(const std::string& filePath, const std::string& content) {
        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }
        
        file << content;
        return static_cast<bool>(file);
    }

    /**
     * 删除文件（不存在时返回 false）
     */
    static bool deleteFile(const std::string& filePath) {
        std::error_code ec;
        return std::filesystem::remove(filePath, ec) && !ec;
    }

    /**
     * 检查文件是否存在
     */
    static bool fileExists(const std::string& filePath) {
        return std::filesystem::exists(filePath) && std::filesystem::is_regular_file(filePath);
    }

    /**
     * 检查目录是否存在
     */
    static bool directoryExists(const std::string& dirPath) {
        return std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath);
    }

    /**
     * 创建目录
     */
    static bool createDirectory(const std::string& dirPath) {
        try {
            return std::filesystem::create_directories(dirPath);
        } catch (...) {
            return false;
        }
    }

    /**
     * 列出目录中的文件
     */
    static std::vector<std::string> listFiles(const std::string& dirPath) {
        std::vector<std::string> files;
        
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path().string());
                }
            }
        } catch (...) {
            // 忽略错误
        }
        
        return files;
    }

    /**
     * 列出目录中的文件（递归）
     */
    static std::vector<std::string> listFilesRecursive(const std::string& dirPath) {
        std::vector<std::string> files;
        
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path().string());
                }
            }
        } catch (...) {
            // 忽略错误
        }
        
        return files;
    }

    /**
     * 获取文件扩展名
     */
    static std::string getExtension(const std::string& filePath) {
        std::filesystem::path path(filePath);
        return path.extension().string();
    }

    /**
     * 获取文件名（不含扩展名）
     */
    static std::string getFileNameWithoutExtension(const std::string& filePath) {
        std::filesystem::path path(filePath);
        return path.stem().string();
    }

    /**
     * 获取文件名（含扩展名）
     */
    static std::string getFileName(const std::string& filePath) {
        std::filesystem::path path(filePath);
        return path.filename().string();
    }

    /**
     * 获取目录路径
     */
    static std::string getDirectoryPath(const std::string& filePath) {
        std::filesystem::path path(filePath);
        return path.parent_path().string();
    }

    /**
     * 连接路径
     */
    static std::string joinPath(const std::string& path1, const std::string& path2) {
        std::filesystem::path p1(path1);
        std::filesystem::path p2(path2);
        return (p1 / p2).string();
    }
};

} // namespace testhub
