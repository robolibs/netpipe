#include <doctest/doctest.h>

#include <netpipe/protocol/http/transport_adapter.hpp>
#include <netpipe/protocol/http1/parser.hpp>

TEST_CASE("HTTP/1.1 strict mode rejects LF-only line endings") {
    auto strict = netpipe::http1::parse_request_head("GET / HTTP/1.1\n"
                                                      "Host: example.com\n"
                                                      "\n");
    CHECK(strict.is_err());
}

TEST_CASE("HTTP/1.1 lenient mode accepts LF-only line endings") {
    netpipe::http1::ParseOptions options;
    options.strict = false;
    options.accept_lf_line_endings = true;

    auto parsed = netpipe::http1::parse_request_head_with_options("GET / HTTP/1.1\n"
                                                                   "Host: example.com\n"
                                                                   "\n",
                                                                   options);

    CHECK(parsed.is_ok());
    CHECK(parsed.value().headers.size() == 1);
}

TEST_CASE("HTTP/1.1 lenient mode can unfold obs-fold header values") {
    netpipe::http1::ParseOptions options;
    options.strict = false;
    options.allow_obs_fold = true;

    auto parsed = netpipe::http1::parse_request_head_with_options("GET / HTTP/1.1\r\n"
                                                                   "X-Trace: part-a\r\n"
                                                                   " part-b\r\n"
                                                                   "\r\n",
                                                                   options);

    CHECK(parsed.is_ok());
    CHECK(parsed.value().headers.size() == 1);
    CHECK(parsed.value().headers[0].value == "part-a part-b");
}

TEST_CASE("HTTP/1.1 raw transport adapter compatibility") {
    netpipe::http::TransportAdapter adapter(netpipe::http::StreamMode::RawBytes);
    netpipe::Message packet{'G',  'E',  'T', ' ', '/', ' ', 'H', 'T', 'T', 'P',  '/',  '1',  '.', '1',
                            '\r', '\n', 'H', 'o', 's', 't', ':', ' ', 'x', '\r', '\n', '\r', '\n'};
    adapter.feed(packet);

    auto raw = adapter.pop_unit();
    CHECK(raw.is_ok());

    dp::String head(reinterpret_cast<const char *>(raw.value().data()), raw.value().size());
    auto parsed = netpipe::http1::parse_request_head(head);
    CHECK(parsed.is_ok());
    CHECK(parsed.value().target == "/");
}
