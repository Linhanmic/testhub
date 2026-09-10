/*
 * TestHub - 规范仓库实现
 */

#include "spec_repository.h"
#include "../util/file_util.h"
#include "../util/logger.h"
#include "../util/string_util.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace testhub {
namespace spec {

namespace {

std::string normalize(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    return abs.lexically_normal().generic_string();
}

bool isInside(const std::string& root, const std::string& candidate) {
    if (root.empty()) return true;
    std::string r = root;
    if (r.back() != '/') r.push_back('/');
    return candidate == root || StringUtil::startsWith(candidate, r);
}

} // namespace

bool SpecRepository::isSpecFile(const std::string& path) {
    std::string ext = StringUtil::toLower(FileUtil::getExtension(path));
    return ext == ".spec" || ext == ".md";
}

bool SpecRepository::isConceptFile(const std::string& path) {
    return StringUtil::toLower(FileUtil::getExtension(path)) == ".cpt";
}

void SpecRepository::configure(const std::string& specsDir, const std::string& conceptsDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    specsDir_ = specsDir.empty() ? normalize(fs::path("specs")) : normalize(fs::path(specsDir));
    if (conceptsDir.empty()) {
        conceptsDir_ = normalize(fs::path(specsDir_) / "concepts");
    } else {
        fs::path cp(conceptsDir);
        conceptsDir_ = cp.is_absolute() ? normalize(cp) : normalize(fs::path(specsDir_) / cp);
    }
}

std::vector<ParseError> SpecRepository::reloadConcepts() {
    std::lock_guard<std::mutex> lock(mutex_);
    concepts_.clear();
    std::vector<ParseError> errors;
    std::vector<std::string> roots = {conceptsDir_};
    if (conceptsDir_ != specsDir_) roots.push_back(specsDir_);
    std::vector<std::string> seen;
    for (const auto& root : roots) {
        if (!FileUtil::directoryExists(root)) continue;
        for (const auto& file : FileUtil::listFilesRecursive(root)) {
            if (!isConceptFile(file)) continue;
            std::string abs = normalize(file);
            if (std::find(seen.begin(), seen.end(), abs) != seen.end()) continue;
            seen.push_back(abs);
            std::string content = FileUtil::readFile(abs);
            auto errs = concepts_.parse(content, toRelative(abs));
            errors.insert(errors.end(), errs.begin(), errs.end());
        }
    }
    TH_LOG_INFO("specs", "Loaded " + std::to_string(concepts_.size()) + " concept(s) from " + conceptsDir_);
    return errors;
}

std::vector<std::string> SpecRepository::collectSpecFiles(const std::string& dir) const {
    std::vector<std::string> files;
    for (const auto& f : FileUtil::listFilesRecursive(dir)) {
        if (isSpecFile(f)) files.push_back(normalize(f));
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> SpecRepository::resolve(const std::vector<std::string>& inputs,
                                                 std::vector<std::string>* missing) const {
    std::vector<std::string> result;
    auto add = [&](const std::string& p) {
        if (std::find(result.begin(), result.end(), p) == result.end()) result.push_back(p);
    };

    std::vector<std::string> effective = inputs;
    if (effective.empty()) effective.push_back("");

    for (const auto& raw : effective) {
        std::string input = StringUtil::trim(raw);
        std::vector<fs::path> candidates;
        if (input.empty()) {
            candidates.push_back(specsDir_);
        } else {
            fs::path p(input);
            if (p.is_absolute()) {
                candidates.push_back(p);
            } else {
                candidates.push_back(fs::path(specsDir_) / p);
                candidates.push_back(p);  // 相对当前工作目录
                // 兼容 "specs/xxx.spec" 这种带根目录名的写法
                fs::path root(specsDir_);
                if (!p.empty() && p.begin()->string() == root.filename().string()) {
                    fs::path rest;
                    bool first = true;
                    for (const auto& part : p) {
                        if (first) { first = false; continue; }
                        rest /= part;
                    }
                    candidates.push_back(root / rest);
                }
            }
        }
        bool found = false;
        for (const auto& c : candidates) {
            std::string abs = normalize(c);
            if (FileUtil::directoryExists(abs)) {
                for (const auto& f : collectSpecFiles(abs)) add(f);
                found = true;
                break;
            }
            if (FileUtil::fileExists(abs)) {
                add(abs);
                found = true;
                break;
            }
        }
        if (!found && missing) missing->push_back(input);
    }
    return result;
}

ParseResult SpecRepository::load(const std::string& path) const {
    std::string abs = normalize(fs::path(path));
    if (!FileUtil::fileExists(abs)) {
        ParseResult r;
        r.errors.push_back({toRelative(abs), 0, "File not found: " + path, ""});
        return r;
    }
    std::string content = FileUtil::readFile(abs);
    return parseText(content, toRelative(abs));
}

ParseResult SpecRepository::parseText(const std::string& content, const std::string& fileName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    SpecParser parser(&concepts_);
    return parser.parse(content, fileName);
}

std::vector<SpecSummary> SpecRepository::list() const {
    std::vector<SpecSummary> out;
    if (!FileUtil::directoryExists(specsDir_)) return out;
    for (const auto& file : collectSpecFiles(specsDir_)) {
        SpecSummary s;
        s.absolutePath = file;
        s.file = toRelative(file);
        std::error_code ec;
        s.size = static_cast<long long>(fs::file_size(file, ec));
        auto mtime = fs::last_write_time(file, ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
                mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            s.modifiedAt = sctp.time_since_epoch().count();
        }
        ParseResult r = load(file);
        if (r.specification) {
            s.heading = r.specification->heading;
            s.tags = r.specification->tags;
            s.scenarioCount = static_cast<int>(r.specification->scenarios.size());
            s.isDataDriven = r.specification->isDataDriven();
        }
        s.valid = r.errors.empty();
        s.errors = r.errors;
        out.push_back(s);
    }
    return out;
}

std::string SpecRepository::toRelative(const std::string& absolutePath) const {
    std::string abs = normalize(fs::path(absolutePath));
    if (isInside(specsDir_, abs) && abs.size() > specsDir_.size()) {
        return abs.substr(specsDir_.size() + 1);
    }
    return abs;
}

std::string SpecRepository::toAbsoluteInside(const std::string& relativePath) const {
    if (relativePath.empty()) return "";
    fs::path p(relativePath);
    std::string abs = p.is_absolute() ? normalize(p) : normalize(fs::path(specsDir_) / p);
    if (!isInside(specsDir_, abs)) return "";
    return abs;
}

bool SpecRepository::readRaw(const std::string& relativePath, std::string& content) const {
    std::string abs = toAbsoluteInside(relativePath);
    if (abs.empty() || !FileUtil::fileExists(abs)) return false;
    content = FileUtil::readFile(abs);
    return true;
}

bool SpecRepository::writeRaw(const std::string& relativePath, const std::string& content, std::string* error) const {
    std::string abs = toAbsoluteInside(relativePath);
    if (abs.empty()) {
        if (error) *error = "Path is outside of the specs directory";
        return false;
    }
    if (!isSpecFile(abs) && !isConceptFile(abs)) {
        if (error) *error = "Only .spec, .md and .cpt files can be written";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(abs).parent_path(), ec);
    if (!FileUtil::writeFile(abs, content)) {
        if (error) *error = "Failed to write file";
        return false;
    }
    return true;
}

bool SpecRepository::remove(const std::string& relativePath, std::string* error) const {
    std::string abs = toAbsoluteInside(relativePath);
    if (abs.empty()) {
        if (error) *error = "Path is outside of the specs directory";
        return false;
    }
    if (!FileUtil::fileExists(abs)) {
        if (error) *error = "File not found";
        return false;
    }
    std::error_code ec;
    fs::remove(abs, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

} // namespace spec
} // namespace testhub
