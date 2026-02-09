#include <doctest/doctest.h>

#include <netpipe/protocol/http/transport_adapter.hpp>

TEST_CASE("HTTP transport adapter framed mode") {
    netpipe::Message payload{'p', 'i', 'n', 'g'};
    auto frame = netpipe::http::TransportAdapter::encode_unit(netpipe::http::StreamMode::FramedMessages, payload);

    netpipe::http::TransportAdapter adapter(netpipe::http::StreamMode::FramedMessages);
    CHECK(adapter.has_complete_unit() == false);

    netpipe::Message first_half(frame.begin(), frame.begin() + 2);
    netpipe::Message second_half(frame.begin() + 2, frame.end());

    adapter.feed(first_half);
    CHECK(adapter.has_complete_unit() == false);

    adapter.feed(second_half);
    CHECK(adapter.has_complete_unit() == true);

    auto decoded = adapter.pop_unit();
    CHECK(decoded.is_ok());
    CHECK(decoded.value() == payload);
    CHECK(adapter.has_complete_unit() == false);
}

TEST_CASE("HTTP transport adapter raw mode") {
    netpipe::http::TransportAdapter adapter(netpipe::http::StreamMode::RawBytes);

    netpipe::Message chunk1{'G', 'E', 'T', ' '};
    netpipe::Message chunk2{'/', ' ', 'H', 'T', 'T', 'P'};
    adapter.feed(chunk1);
    adapter.feed(chunk2);

    CHECK(adapter.has_complete_unit() == true);

    auto decoded = adapter.pop_unit();
    CHECK(decoded.is_ok());
    CHECK(decoded.value().size() == chunk1.size() + chunk2.size());

    auto empty = adapter.pop_unit();
    CHECK(empty.is_err());
}
