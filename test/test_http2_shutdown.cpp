#include <doctest/doctest.h>

#include <netpipe/protocol/http2/shutdown.hpp>

TEST_CASE("HTTP/2 RST_STREAM encode/decode") {
    auto frame = netpipe::http2::make_rst_stream(7, netpipe::http2::ErrorCode::Cancel);
    auto parsed = netpipe::http2::parse_rst_stream(frame);
    CHECK(parsed.is_ok());
    CHECK(parsed.value() == netpipe::http2::ErrorCode::Cancel);
}

TEST_CASE("HTTP/2 GOAWAY encode/decode") {
    netpipe::http2::GoAway out;
    out.last_stream_id = 11;
    out.error_code = netpipe::http2::ErrorCode::NoError;
    out.debug_data = {'b', 'y', 'e'};

    auto frame = netpipe::http2::make_goaway(out);
    auto parsed = netpipe::http2::parse_goaway(frame);
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().last_stream_id == 11);
    CHECK(parsed.value().error_code == netpipe::http2::ErrorCode::NoError);
    CHECK(parsed.value().debug_data == out.debug_data);
}

TEST_CASE("HTTP/2 shutdown manager enters draining and tracks streams") {
    netpipe::http2::ShutdownManager manager;
    manager.on_stream_opened(1);
    manager.on_stream_opened(3);
    CHECK(manager.active_stream_count() == 2);

    auto goaway = manager.start_shutdown(netpipe::http2::ErrorCode::NoError);
    REQUIRE(goaway.is_ok());
    CHECK(manager.draining());
    CHECK(manager.goaway_sent());

    auto parsed = netpipe::http2::parse_goaway(goaway.value());
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().last_stream_id == 3);
}

TEST_CASE("HTTP/2 peer GOAWAY limits new streams") {
    netpipe::http2::ShutdownManager manager;

    netpipe::http2::GoAway peer;
    peer.last_stream_id = 5;
    peer.error_code = netpipe::http2::ErrorCode::NoError;
    auto peer_frame = netpipe::http2::make_goaway(peer);

    CHECK(manager.process_incoming(peer_frame).is_ok());
    CHECK(manager.draining());
    CHECK(manager.can_open_new_stream(3));
    CHECK(manager.can_open_new_stream(7) == false);
}
