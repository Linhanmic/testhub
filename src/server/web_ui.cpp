/*
 * TestHub - Web UI 挂载
 * 默认使用编译期内嵌的 web/ 资源；设置 webDir 时改为从磁盘提供（便于前端开发热更新）。
 */

#include "../testhub.h"
#include "../util/file_util.h"
#include "../util/logger.h"
#include "../util/string_util.h"
#include "web_assets.h"

namespace testhub {

namespace {

bool isApiPath(const std::string& path) {
    return StringUtil::startsWith(path, "/api/") || StringUtil::startsWith(path, "/ws");
}

} // namespace

void TestHub::registerWebUi() {
    HttpServer& http = *httpServer_;

    if (!config_.webDir.empty()) {
        if (!FileUtil::directoryExists(config_.webDir)) {
            TH_LOG_WARN("web", "Web directory does not exist: " + config_.webDir + " (falling back to embedded UI)");
        } else {
            TH_LOG_INFO("web", "Serving Web UI from directory: " + config_.webDir);
            http.serveDirectory("/", config_.webDir);
            std::string indexPath = FileUtil::joinPath(config_.webDir, "index.html");
            http.setFallback([indexPath](const HttpRequest& req) {
                if (req.method == "GET" && !isApiPath(req.path) && FileUtil::fileExists(indexPath)) {
                    HttpResponse r = HttpResponse::html(FileUtil::readFile(indexPath));
                    r.headers["Cache-Control"] = "no-cache";
                    return r;
                }
                return HttpResponse::error(404, "Not Found", req.method + " " + req.path);
            });
            return;
        }
    }

    std::string indexHtml;
    for (std::size_t i = 0; i < web_assets::kAssetCount; ++i) {
        const web_assets::Asset& a = web_assets::kAssets[i];
        std::string content(reinterpret_cast<const char*>(a.data), a.size);
        std::string url = std::string("/") + a.path;
        if (std::string(a.path) == "index.html") indexHtml = content;
        http.addStaticAsset(url, content, HttpServer::mimeTypeFor(a.path));
    }
    if (!indexHtml.empty()) {
        http.addStaticAsset("/", indexHtml, "text/html; charset=utf-8");
    }
    TH_LOG_INFO("web", "Embedded Web UI registered (" + std::to_string(web_assets::kAssetCount) + " assets)");

    http.setFallback([indexHtml](const HttpRequest& req) {
        // SPA：非 API 的 GET 请求回退到 index.html（hash 路由不需要，但便于直接访问 /dashboard 等）
        if (req.method == "GET" && !isApiPath(req.path) && !indexHtml.empty() &&
            FileUtil::getExtension(req.path).empty()) {
            HttpResponse r = HttpResponse::html(indexHtml);
            r.headers["Cache-Control"] = "no-cache";
            return r;
        }
        return HttpResponse::error(404, "Not Found", req.method + " " + req.path);
    });
}

} // namespace testhub
