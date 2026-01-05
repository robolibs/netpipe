#include <chrono>
#include <cstring>
#include <datapod/datapod.hpp>
#include <netpipe/netpipe.hpp>
#include <thread>
#include <wirebit/wirebit.hpp>

// Example: Type-tagged serialization using dp::type_hash<T>()
// Server sends DIFFERENT types (Pose, Point) and client knows which is which!
//
// Wire format:
// ┌──────────────────┬─────────────────────┐
// │ type_hash (8B)   │ Payload (N bytes)   │
// └──────────────────┴─────────────────────┘

using namespace datapod;

// ═══════════════════════════════════════════════════════════════════════════
// GENERIC SERIALIZATION - Works for ANY dp:: type!
// ═══════════════════════════════════════════════════════════════════════════

template <typename T> netpipe::Message pack(const T &data) {
    // Create buffer with type hash + payload
    std::vector<dp::u8> buf(8 + sizeof(T));

    hash_t hash = type_hash<T>();           // Unique hash for this type!
    std::memcpy(&buf[0], &hash, 8);         // 8 bytes type hash
    std::memcpy(&buf[8], &data, sizeof(T)); // Payload

    // Convert to netpipe::Message
    return netpipe::Message(buf.begin(), buf.end());
}

hash_t get_type_hash(const netpipe::Message &msg) {
    hash_t hash;
    std::memcpy(&hash, &msg[0], 8);
    return hash;
}

template <typename T> T unpack(const netpipe::Message &msg) {
    T data;
    std::memcpy(&data, &msg[8], sizeof(T));
    return data;
}

// ═══════════════════════════════════════════════════════════════════════════
// SERVER
// ═══════════════════════════════════════════════════════════════════════════

void server() {
    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  TYPE-TAGGED TUNNEL SERVER").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    // Print type hashes so we can see them
    echo::info("Type hashes:").cyan();
    echo::info("  Pose:  ", type_hash<Pose>()).cyan();
    echo::info("  Point: ", type_hash<Point>()).cyan();

    // Step 1: Create TAP interface
    echo::info("[1/4] Creating TAP interface...").yellow();
    wirebit::TapConfig tap_cfg{
        .interface_name = "tap0", .create_if_missing = true, .destroy_on_close = true, .set_up_on_create = true};
    auto tap_link_res = wirebit::TapLink::create(tap_cfg);
    if (tap_link_res.is_err()) {
        echo::error("Failed to create TAP: ", tap_link_res.error().message.c_str());
        return;
    }
    auto tap_link = std::make_shared<wirebit::TapLink>(std::move(tap_link_res.value()));
    echo::info("✓ TAP interface ready: tap0").green();

    system("sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null");

    // Step 2: Create Ethernet endpoint
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000};
    wirebit::EthEndpoint eth(tap_link, eth_cfg, 1, local_mac);
    echo::info("✓ Ethernet endpoint ready").green();

    // Step 3: Start TCP tunnel server
    echo::info("[2/4] Starting TCP tunnel server...").yellow();
    netpipe::TcpStream tunnel_server;
    netpipe::TcpEndpoint tunnel_endpoint{"0.0.0.0", 9001};
    auto listen_res = tunnel_server.listen(tunnel_endpoint);
    if (listen_res.is_err()) {
        echo::error("Tunnel listen failed: ", listen_res.error().message.c_str());
        return;
    }
    echo::info("✓ Tunnel listening on port 9001").green();

    echo::info("[3/4] Waiting for tunnel client...").yellow();
    auto tunnel_client_res = tunnel_server.accept();
    if (tunnel_client_res.is_err()) {
        echo::error("Tunnel accept failed: ", tunnel_client_res.error().message.c_str());
        return;
    }
    auto tunnel_client = std::move(tunnel_client_res.value());
    echo::info("✓ Tunnel established!").green();

    // Step 4: Start data server on TAP interface
    echo::info("[4/4] Starting data server on 10.0.0.1:7777...").yellow();
    netpipe::TcpStream data_server;
    netpipe::TcpEndpoint data_endpoint{"10.0.0.1", 7777};
    auto data_listen_res = data_server.listen(data_endpoint);
    if (data_listen_res.is_err()) {
        echo::error("Data listen failed: ", data_listen_res.error().message.c_str());
        return;
    }

    // Launch tunnel thread
    std::thread tunnel_thread([&]() {
        while (true) {
            eth.process();
            auto eth_result = eth.recv_eth();
            if (eth_result.is_ok()) {
                netpipe::Message tcp_msg(eth_result.value().begin(), eth_result.value().end());
                tunnel_client->send(tcp_msg);
            }
            auto tcp_result = tunnel_client->recv();
            if (tcp_result.is_ok()) {
                wirebit::Bytes eth_frame(tcp_result.value().begin(), tcp_result.value().end());
                eth.send_eth(eth_frame);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    echo::info("✓ Waiting for data client...").green();
    auto data_client_res = data_server.accept();
    if (data_client_res.is_err()) {
        echo::error("Data accept failed: ", data_client_res.error().message.c_str());
        return;
    }
    auto data_client = std::move(data_client_res.value());

    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  ALL CONNECTED! Sending alternating Pose/Point...").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    double time = 0.0;

    for (int i = 0; i < 100; i++) {
        if (i % 2 == 0) {
            // Send POSE (full position + rotation)
            Pose pose;
            pose.point.x = 5.0 * std::cos(time);
            pose.point.y = 5.0 * std::sin(time);
            pose.point.z = time * 0.1;
            pose.rotation.w = std::cos(time / 2.0);
            pose.rotation.x = 0.0;
            pose.rotation.y = 0.0;
            pose.rotation.z = std::sin(time / 2.0);

            auto msg = pack(pose);
            data_client->send(msg);

            echo::info("Sent POSE #", i, ": pos=(", pose.point.x, ", ", pose.point.y, ", ", pose.point.z, ")").cyan();
        } else {
            // Send POINT (just position, no rotation)
            Point point;
            point.x = 3.0 * std::cos(time * 2.0);
            point.y = 3.0 * std::sin(time * 2.0);
            point.z = time * 0.2;

            auto msg = pack(point);
            data_client->send(msg);

            echo::info("Sent POINT #", i, ": (", point.x, ", ", point.y, ", ", point.z, ")").yellow();
        }

        time += 0.1;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    echo::info("Server done!").green();
    tunnel_thread.detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// CLIENT
// ═══════════════════════════════════════════════════════════════════════════

void client() {
    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  TYPE-TAGGED TUNNEL CLIENT").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    // Print type hashes so we can compare
    echo::info("Expected type hashes:").cyan();
    echo::info("  Pose:  ", type_hash<Pose>()).cyan();
    echo::info("  Point: ", type_hash<Point>()).cyan();

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Step 1: Create TAP interface
    echo::info("[1/4] Creating TAP interface...").yellow();
    wirebit::TapConfig tap_cfg{
        .interface_name = "tap1", .create_if_missing = true, .destroy_on_close = true, .set_up_on_create = true};
    auto tap_link_res = wirebit::TapLink::create(tap_cfg);
    if (tap_link_res.is_err()) {
        echo::error("Failed to create TAP: ", tap_link_res.error().message.c_str());
        return;
    }
    auto tap_link = std::make_shared<wirebit::TapLink>(std::move(tap_link_res.value()));
    echo::info("✓ TAP interface ready: tap1").green();

    system("sudo ip addr add 10.0.0.2/24 dev tap1 2>/dev/null");

    // Step 2: Create Ethernet endpoint
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000};
    wirebit::EthEndpoint eth(tap_link, eth_cfg, 2, local_mac);
    echo::info("✓ Ethernet endpoint ready").green();

    // Step 3: Connect to tunnel server
    echo::info("[2/4] Connecting to tunnel server...").yellow();
    netpipe::TcpStream tunnel;
    netpipe::TcpEndpoint tunnel_endpoint{"127.0.0.1", 9001};
    auto connect_res = tunnel.connect(tunnel_endpoint);
    if (connect_res.is_err()) {
        echo::error("Tunnel connect failed: ", connect_res.error().message.c_str());
        return;
    }
    echo::info("✓ Tunnel connected!").green();

    // Launch tunnel thread
    std::thread tunnel_thread([&]() {
        while (true) {
            eth.process();
            auto eth_result = eth.recv_eth();
            if (eth_result.is_ok()) {
                netpipe::Message tcp_msg(eth_result.value().begin(), eth_result.value().end());
                tunnel.send(tcp_msg);
            }
            auto tcp_result = tunnel.recv();
            if (tcp_result.is_ok()) {
                wirebit::Bytes eth_frame(tcp_result.value().begin(), tcp_result.value().end());
                eth.send_eth(eth_frame);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Step 4: Connect to data server through TAP
    echo::info("[3/4] Connecting to data server at 10.0.0.1:7777...").yellow();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    netpipe::TcpStream data_stream;
    netpipe::TcpEndpoint data_endpoint{"10.0.0.1", 7777};
    auto data_connect_res = data_stream.connect(data_endpoint);
    if (data_connect_res.is_err()) {
        echo::error("Data connect failed: ", data_connect_res.error().message.c_str());
        return;
    }

    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  ALL CONNECTED! Receiving type-tagged data...").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    int pose_count = 0;
    int point_count = 0;
    int unknown_count = 0;

    for (int i = 0; i < 100; i++) {
        auto recv_res = data_stream.recv();
        if (recv_res.is_err()) {
            echo::error("Recv failed: ", recv_res.error().message.c_str());
            break;
        }

        auto msg = recv_res.value();
        hash_t received_hash = get_type_hash(msg);

        // Check type and unpack accordingly
        if (received_hash == type_hash<Pose>()) {
            Pose pose = unpack<Pose>(msg);
            echo::info("Recv POSE #", i, ": pos=(", pose.point.x, ", ", pose.point.y, ", ", pose.point.z,
                       ") rot_w=", pose.rotation.w)
                .cyan();
            pose_count++;

        } else if (received_hash == type_hash<Point>()) {
            Point point = unpack<Point>(msg);
            echo::info("Recv POINT #", i, ": (", point.x, ", ", point.y, ", ", point.z, ")").yellow();
            point_count++;

        } else {
            echo::error("Unknown type hash: ", received_hash);
            unknown_count++;
        }
    }

    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  SUMMARY:").green();
    echo::info("    Poses received:  ", pose_count).cyan();
    echo::info("    Points received: ", point_count).yellow();
    echo::info("    Unknown types:   ", unknown_count).red();
    echo::info("═══════════════════════════════════════════════════════").green();

    echo::info("Client done!").green();
    tunnel_thread.detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("═══════════════════════════════════════════════════════");
        echo::info("  TYPE-TAGGED TUNNEL - Multiple types over TAP");
        echo::info("═══════════════════════════════════════════════════════");
        echo::info("");
        echo::info("Usage: ", argv[0], " [server|client]");
        echo::info("");
        echo::info("This demonstrates dp::type_hash<T>() for type-tagged");
        echo::info("serialization. Server sends alternating Pose and Point,");
        echo::info("client automatically detects which type it received!");
        echo::info("");
        echo::info("Wire format:");
        echo::info("  ┌──────────────────┬─────────────────────┐");
        echo::info("  │ type_hash (8B)   │ Payload (N bytes)   │");
        echo::info("  └──────────────────┴─────────────────────┘");
        echo::info("");
        echo::info("Terminal 1: ./type_tagged_tunnel server");
        echo::info("Terminal 2: ./type_tagged_tunnel client");
        echo::info("═══════════════════════════════════════════════════════");
        return 1;
    }

    dp::String mode(argv[1]);
    if (mode == "server") {
        server();
    } else if (mode == "client") {
        client();
    } else {
        echo::error("Unknown mode: ", mode.c_str());
        return 1;
    }

    return 0;
}
