#include <doctest/doctest.h>

#include <netpipe/protocol/http1/incremental.hpp>

TEST_CASE("HTTP/1.1 incremental request parser with Content-Length") {
    netpipe::http1::IncrementalRequestParser parser;

    netpipe::Message p1{'P',  'O',  'S', 'T', ' ', '/', 'v', '1', ' ', 'H',  'T',  'T',  'P',  '/',  '1',  '.', '1',
                        '\r', '\n', 'H', 'o', 's', 't', ':', ' ', 'x', '\r', '\n', 'C',  'o',  'n',  't',  'e', 'n',
                        't',  '-',  'L', 'e', 'n', 'g', 't', 'h', ':', ' ',  '4',  '\r', '\n', '\r', '\n', 'p'};
    parser.feed(p1);

    auto first = parser.try_parse();
    REQUIRE(first.is_ok());
    CHECK(first.value().has_value() == false);

    parser.feed(netpipe::Message{'i', 'n', 'g'});
    auto second = parser.try_parse();
    REQUIRE(second.is_ok());
    REQUIRE(second.value().has_value());
    CHECK(second.value().value().body.size() == 4);
}

TEST_CASE("HTTP/1.1 incremental request parser chunked with trailers") {
    netpipe::http1::IncrementalRequestParser parser;

    parser.feed(netpipe::Message{'P',  'O',  'S',  'T',  ' ',  '/',  'u', 'p',  ' ',  'H', 'T', 'T', 'P',
                                 '/',  '1',  '.',  '1',  '\r', '\n', 'H', 'o',  's',  't', ':', ' ', 'x',
                                 '\r', '\n', 'T',  'r',  'a',  'n',  's', 'f',  'e',  'r', '-', 'E', 'n',
                                 'c',  'o',  'd',  'i',  'n',  'g',  ':', ' ',  'c',  'h', 'u', 'n', 'k',
                                 'e',  'd',  '\r', '\n', '\r', '\n', '4', '\r', '\n', 'd', 'a', 't'});
    auto a = parser.try_parse();
    REQUIRE(a.is_ok());
    CHECK(a.value().has_value() == false);

    parser.feed(netpipe::Message{'a', '\r', '\n', '0', '\r', '\n', 'X', '-', 'S', 'u', 'm', ':', ' ', '4', '\r'});
    auto b = parser.try_parse();
    REQUIRE(b.is_ok());
    CHECK(b.value().has_value() == false);

    parser.feed(netpipe::Message{'\n', '\r', '\n'});
    auto c = parser.try_parse();
    REQUIRE(c.is_ok());
    REQUIRE(c.value().has_value());
    CHECK(c.value().value().body.size() == 4);
    REQUIRE(c.value().value().trailers.size() == 1);
    CHECK(c.value().value().trailers[0].name == "X-Sum");
}

TEST_CASE("HTTP/1.1 incremental response parser with chunked trailers") {
    netpipe::http1::IncrementalResponseParser parser;

    parser.feed(netpipe::Message{'H', 'T', 'T',  'P',  '/',  '1',  '.',  '1',  ' ', '2',  '0',  '0', ' ',
                                 'O', 'K', '\r', '\n', 'T',  'r',  'a',  'n',  's', 'f',  'e',  'r', '-',
                                 'E', 'n', 'c',  'o',  'd',  'i',  'n',  'g',  ':', ' ',  'c',  'h', 'u',
                                 'n', 'k', 'e',  'd',  '\r', '\n', '\r', '\n', '2', '\r', '\n', 'o'});

    auto x = parser.try_parse();
    REQUIRE(x.is_ok());
    CHECK(x.value().has_value() == false);

    parser.feed(netpipe::Message{'k', '\r', '\n', '0', '\r', '\n', 'X',  '-',  'T',  'r',
                                 'a', 'c',  'e',  ':', ' ',  '1',  '\r', '\n', '\r', '\n'});
    auto y = parser.try_parse();
    REQUIRE(y.is_ok());
    REQUIRE(y.value().has_value());
    CHECK(y.value().value().body.size() == 2);
    REQUIRE(y.value().value().trailers.size() == 1);
    CHECK(y.value().value().trailers[0].name == "X-Trace");
}
