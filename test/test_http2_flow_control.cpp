#include <doctest/doctest.h>

#include <netpipe/protocol/http2/flow_control.hpp>

TEST_CASE("HTTP/2 flow-control consumes and updates windows") {
    netpipe::http2::FlowController fc;
    fc.ensure_stream(1);

    CHECK(fc.connection_send_window() == netpipe::http2::FlowController::DEFAULT_WINDOW);
    CHECK(fc.stream_send_window(1) == netpipe::http2::FlowController::DEFAULT_WINDOW);

    CHECK(fc.consume_outbound(1, 1024).is_ok());
    CHECK(fc.connection_send_window() == netpipe::http2::FlowController::DEFAULT_WINDOW - 1024);
    CHECK(fc.stream_send_window(1) == netpipe::http2::FlowController::DEFAULT_WINDOW - 1024);

    CHECK(fc.update_connection_send_window(2048).is_ok());
    CHECK(fc.update_stream_send_window(1, 2048).is_ok());
    CHECK(fc.connection_send_window() == netpipe::http2::FlowController::DEFAULT_WINDOW + 1024);
    CHECK(fc.stream_send_window(1) == netpipe::http2::FlowController::DEFAULT_WINDOW + 1024);
}

TEST_CASE("HTTP/2 flow-control rejects exhaustion and overflow") {
    netpipe::http2::FlowController fc;
    fc.ensure_stream(3);

    CHECK(fc.consume_outbound(3, netpipe::http2::FlowController::DEFAULT_WINDOW + 1).is_err());
    CHECK(fc.update_connection_send_window(0).is_err());

    auto max_win = netpipe::http2::FlowController::MAX_WINDOW;
    CHECK(fc.update_connection_send_window(max_win - fc.connection_send_window()).is_ok());
    CHECK(fc.update_connection_send_window(1).is_err());
}

TEST_CASE("HTTP/2 priority scheduler favors higher weighted work") {
    netpipe::http2::FlowController fc;
    fc.ensure_stream(1);
    fc.ensure_stream(5);

    CHECK(fc.set_priority(1, 0, 10, false).is_ok());
    CHECK(fc.set_priority(5, 0, 200, false).is_ok());

    fc.enqueue_stream_data(1, 1000);
    fc.enqueue_stream_data(5, 200);

    auto next = fc.pick_next_stream();
    REQUIRE(next.has_value());
    CHECK(next.value() == 5);

    CHECK(fc.mark_stream_scheduled(5, 200).is_ok());
    auto next_after = fc.pick_next_stream();
    REQUIRE(next_after.has_value());
    CHECK(next_after.value() == 1);
}

TEST_CASE("HTTP/2 priority validation rejects invalid input") {
    netpipe::http2::FlowController fc;
    CHECK(fc.set_priority(0, 0, 10, false).is_err());
    CHECK(fc.set_priority(1, 0, 0, false).is_err());
    CHECK(fc.set_priority(1, 0, 255, false).is_ok());
}
