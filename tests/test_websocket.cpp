#include "test_framework.h"
#include "server/websocket_server.h"
#include "util/base64.h"
#include "util/sha1.h"

using namespace testhub;

TEST_CASE("ws: sha1 and base64 match known vectors") {
    CHECK_EQ(Sha1::hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(Sha1::hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(Base64::encode("hello"), std::string("aGVsbG8="));
    CHECK_EQ(Base64::encode(""), std::string(""));
    CHECK_EQ(Base64::decode("aGVsbG8="), std::string("hello"));
    std::string binary("\x00\xff\x10" "binary", 9);
    CHECK_EQ(Base64::decode(Base64::encode(binary)), binary);
}

TEST_CASE("ws: accept key follows RFC 6455 example") {
    CHECK_EQ(WebSocketServer::computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="),
             std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST_CASE("ws: frame encoding for small, medium and large payloads") {
    std::string small = WebSocketServer::encodeFrame("hi");
    REQUIRE_EQ(small.size(), static_cast<size_t>(4));
    CHECK_EQ(static_cast<unsigned char>(small[0]), 0x81u);
    CHECK_EQ(static_cast<unsigned char>(small[1]), 0x02u);
    CHECK_EQ(small.substr(2), std::string("hi"));

    std::string medium = WebSocketServer::encodeFrame(std::string(300, 'x'));
    REQUIRE_EQ(medium.size(), static_cast<size_t>(4 + 300));
    CHECK_EQ(static_cast<unsigned char>(medium[1]), 126u);
    CHECK_EQ(static_cast<unsigned char>(medium[2]), 0x01u);
    CHECK_EQ(static_cast<unsigned char>(medium[3]), 0x2Cu);

    std::string large = WebSocketServer::encodeFrame(std::string(70000, 'y'));
    REQUIRE_EQ(large.size(), static_cast<size_t>(10 + 70000));
    CHECK_EQ(static_cast<unsigned char>(large[1]), 127u);

    std::string ping = WebSocketServer::encodeFrame("", 0x9);
    CHECK_EQ(static_cast<unsigned char>(ping[0]), 0x89u);
}
