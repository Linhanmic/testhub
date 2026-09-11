/*
 * TestHub - 标签过滤表达式
 * 支持：tag1 & tag2、tag1 | tag2、!tag、括号，以及 and/or/not 关键字与逗号（视为 &）。
 * 例："smoke & !slow"、"(login | search) & regression"
 */

#pragma once

#include "../util/string_util.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace testhub {

class TagFilter {
public:
    TagFilter() = default;

    /**
     * 解析表达式；失败时 error 非空并且 filter 匹配所有
     */
    static TagFilter parse(const std::string& expression, std::string* error = nullptr) {
        TagFilter f;
        f.expression_ = StringUtil::trim(expression);
        if (f.expression_.empty()) return f;
        Parser p(f.expression_);
        std::string err;
        f.root_ = p.parseExpression(err);
        if (!err.empty() || !p.atEnd()) {
            if (err.empty()) err = "Unexpected token near position " + std::to_string(p.position());
            if (error) *error = err;
            f.root_.reset();
            f.invalid_ = true;
        }
        return f;
    }

    /**
     * 合并多个表达式（全部满足）
     */
    static TagFilter all(const std::vector<std::string>& expressions, std::string* error = nullptr) {
        std::string combined;
        for (const auto& e : expressions) {
            std::string t = StringUtil::trim(e);
            if (t.empty()) continue;
            if (!combined.empty()) combined += " & ";
            combined += "(" + t + ")";
        }
        return parse(combined, error);
    }

    bool empty() const { return !root_; }
    bool valid() const { return !invalid_; }
    const std::string& expression() const { return expression_; }

    bool matches(const std::vector<std::string>& tags) const {
        if (!root_) return true;
        return root_->eval(tags);
    }

private:
    struct Node {
        enum Kind { Tag, And, Or, Not } kind;
        std::string tag;
        std::unique_ptr<Node> left, right;

        bool eval(const std::vector<std::string>& tags) const {
            switch (kind) {
                case Tag: {
                    std::string lower = StringUtil::toLower(tag);
                    return std::any_of(tags.begin(), tags.end(),
                                       [&](const std::string& t) { return StringUtil::toLower(StringUtil::trim(t)) == lower; });
                }
                case And: return left->eval(tags) && right->eval(tags);
                case Or: return left->eval(tags) || right->eval(tags);
                case Not: return !left->eval(tags);
            }
            return false;
        }
    };

    class Parser {
    public:
        explicit Parser(const std::string& s) : s_(s) {}

        bool atEnd() { skipWs(); return pos_ >= s_.size(); }
        size_t position() const { return pos_; }

        std::unique_ptr<Node> parseExpression(std::string& err) { return parseOr(err); }

    private:
        const std::string& s_;
        size_t pos_ = 0;

        void skipWs() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }

        bool consume(const char* tok) {
            skipWs();
            size_t len = std::char_traits<char>::length(tok);
            if (s_.compare(pos_, len, tok) == 0) {
                // 关键字需要边界
                bool word = std::isalpha(static_cast<unsigned char>(tok[0]));
                if (word && pos_ + len < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_ + len])) || s_[pos_ + len] == '_')) {
                    return false;
                }
                pos_ += len;
                return true;
            }
            return false;
        }

        std::unique_ptr<Node> parseOr(std::string& err) {
            auto left = parseAnd(err);
            if (!left) return nullptr;
            while (true) {
                if (consume("||") || consume("|") || consume("or") || consume("OR")) {
                    auto right = parseAnd(err);
                    if (!right) return nullptr;
                    auto n = std::make_unique<Node>();
                    n->kind = Node::Or;
                    n->left = std::move(left);
                    n->right = std::move(right);
                    left = std::move(n);
                } else {
                    return left;
                }
            }
        }

        std::unique_ptr<Node> parseAnd(std::string& err) {
            auto left = parseUnary(err);
            if (!left) return nullptr;
            while (true) {
                if (consume("&&") || consume("&") || consume(",") || consume("and") || consume("AND")) {
                    auto right = parseUnary(err);
                    if (!right) return nullptr;
                    auto n = std::make_unique<Node>();
                    n->kind = Node::And;
                    n->left = std::move(left);
                    n->right = std::move(right);
                    left = std::move(n);
                } else {
                    return left;
                }
            }
        }

        std::unique_ptr<Node> parseUnary(std::string& err) {
            if (consume("!") || consume("not") || consume("NOT")) {
                auto operand = parseUnary(err);
                if (!operand) return nullptr;
                auto n = std::make_unique<Node>();
                n->kind = Node::Not;
                n->left = std::move(operand);
                return n;
            }
            if (consume("(")) {
                auto inner = parseOr(err);
                if (!inner) return nullptr;
                if (!consume(")")) {
                    err = "Missing ')' in tag expression";
                    return nullptr;
                }
                return inner;
            }
            skipWs();
            size_t start = pos_;
            while (pos_ < s_.size()) {
                char c = s_[pos_];
                if (c == '&' || c == '|' || c == '!' || c == '(' || c == ')' || c == ',' || std::isspace(static_cast<unsigned char>(c))) break;
                ++pos_;
            }
            if (pos_ == start) {
                err = "Expected tag name at position " + std::to_string(pos_);
                return nullptr;
            }
            auto n = std::make_unique<Node>();
            n->kind = Node::Tag;
            n->tag = s_.substr(start, pos_ - start);
            return n;
        }
    };

    std::string expression_;
    std::shared_ptr<Node> root_;
    bool invalid_ = false;
};

} // namespace testhub
