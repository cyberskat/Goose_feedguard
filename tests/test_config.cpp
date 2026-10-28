#include "feedguard_config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <openssl/ssl.h>
#include <simdjson.h>

TEST_CASE("Approved market configuration") {
    REQUIRE(feedguard::category == "linear");
    REQUIRE(feedguard::symbol == "BTCUSDT");
    REQUIRE(feedguard::topic == "orderbook.50.BTCUSDT");
}

TEST_CASE("JSON dependency parses a subscription response") {
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    const auto json = simdjson::padded_string(std::string_view{R"({"success":true,"op":"subscribe"})"});
    REQUIRE(parser.parse(json).get(document) == simdjson::SUCCESS);
    bool success = false;
    REQUIRE(document["success"].get(success) == simdjson::SUCCESS);
    REQUIRE(success);
}

TEST_CASE("OpenSSL creates a TLS client context") {
    auto* context = SSL_CTX_new(TLS_client_method());
    const bool created = context != nullptr;
    SSL_CTX_free(context);
    REQUIRE(created);
}
