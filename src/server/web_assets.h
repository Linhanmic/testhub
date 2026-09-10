/*
 * TestHub - 内嵌 Web UI 资源表
 * 具体数据由 CMake 在构建期从 web/ 目录生成（见 cmake/EmbedResources.cmake）。
 */

#pragma once

#include <cstddef>

namespace testhub {
namespace web_assets {

struct Asset {
    const char* path;           // 相对路径，例如 "index.html"
    const unsigned char* data;
    std::size_t size;
};

extern const Asset kAssets[];
extern const std::size_t kAssetCount;

} // namespace web_assets
} // namespace testhub
