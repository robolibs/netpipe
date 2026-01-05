#include <chrono>
#include <cstring>
#include <datapod/datapod.hpp>
#include <netpipe/netpipe.hpp>
#include <thread>
#include <wirebit/wirebit.hpp>

// Example: Complete TAP tunnel + Pose streaming in ONE program!
// Server: Creates TAP, runs tunnel, AND serves pose data
// Client: Creates TAP, runs tunnel, AND receives pose data
//
// Just 2 terminals needed:
//   Terminal 1: ./pose_tunnel server
//   Terminal 2: ./pose_tunnel client

using namespace datapod;

// Serialize Pose to bytes
inline netpipe::Message serialize_pose(const Pose &pose) {
    netpipe::Message msg(sizeof(Pose));
    std::memcpy(msg.data(), &pose, sizeof(Pose));
    return msg;
}

// Deserialize bytes to Pose
inline Pose deserialize_pose(const netpipe::Message &msg) {
    Pose pose;
    if (msg.size() >= sizeof(Pose)) {
        std::memcpy(&pose, msg.data(), sizeof(Pose));
    }
    return pose;
}

void server() {
    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  POSE TUNNEL SERVER - TAP + Tunnel + Pose Streaming").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    // Step 1: Create TAP interface (or attach if exists)
    echo::info("[1/4] Creating TAP interface...").yellow();
    wirebit::TapConfig tap_cfg{
        .interface_name = "tap0", .create_if_missing = true, .destroy_on_close = true, .set_up_on_create = true};
    auto tap_link_res = wirebit::TapLink::create(tap_cfg);
    if (tap_link_res.is_err()) {
        echo::error("Failed to create TAP: ", tap_link_res.error().message.c_str());
        echo::error("Try: sudo ip link delete tap0  (to clean up)");
        return;
    }
    auto tap_link = std::make_shared<wirebit::TapLink>(std::move(tap_link_res.value()));
    echo::info("✓ TAP interface ready: tap0").green();

    // Assign IP
    system("sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null");

    // Step 2: Create Ethernet endpoint
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000};
    wirebit::EthEndpoint eth(tap_link, eth_cfg, 1, local_mac);
    echo::info("✓ Ethernet endpoint ready: MAC=02:00:00:00:00:01").green();

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

    // Step 4: Start pose server on TAP interface
    echo::info("[4/4] Starting pose server on 10.0.0.1:7777...").yellow();
    netpipe::TcpStream pose_server;
    netpipe::TcpEndpoint pose_endpoint{"10.0.0.1", 7777};
    auto pose_listen_res = pose_server.listen(pose_endpoint);
    if (pose_listen_res.is_err()) {
        echo::error("Pose listen failed: ", pose_listen_res.error().message.c_str());
        return;
    }

    // Launch tunnel thread
    std::thread tunnel_thread([&]() {
        while (true) {
            // TAP -> TCP
            eth.process();
            auto eth_result = eth.recv_eth();
            if (eth_result.is_ok()) {
                netpipe::Message tcp_msg(eth_result.value().begin(), eth_result.value().end());
                tunnel_client->send(tcp_msg);
            }

            // TCP -> TAP
            auto tcp_result = tunnel_client->recv();
            if (tcp_result.is_ok()) {
                wirebit::Bytes eth_frame(tcp_result.value().begin(), tcp_result.value().end());
                eth.send_eth(eth_frame);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    echo::info("✓ Pose server ready, waiting for pose client...").green();
    auto pose_client_res = pose_server.accept();
    if (pose_client_res.is_err()) {
        echo::error("Pose accept failed: ", pose_client_res.error().message.c_str());
        return;
    }
    auto pose_client = std::move(pose_client_res.value());

    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  ALL CONNECTED! Streaming pose data...").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    // Simulate robot movement
    Pose robot_pose = pose::make(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    double time = 0.0;

    for (int i = 0; i < 200; i++) {
        // Circular motion with rising height
        robot_pose.point.x = 5.0 * std::cos(time);
        robot_pose.point.y = 5.0 * std::sin(time);
        robot_pose.point.z = time * 0.1;

        // Rotation around Z
        double angle = time;
        robot_pose.rotation.w = std::cos(angle / 2.0);
        robot_pose.rotation.z = std::sin(angle / 2.0);

        auto msg = serialize_pose(robot_pose);
        auto send_res = pose_client->send(msg);
        if (send_res.is_err()) {
            echo::error("Send failed: ", send_res.error().message.c_str());
            break;
        }

        if (i % 10 == 0) {
            echo::info("Sent pose #", i, ": pos=(", robot_pose.point.x, ", ", robot_pose.point.y, ", ",
                       robot_pose.point.z, ")")
                .cyan();
        }

        time += 0.1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    echo::info("Server done!").green();
    tunnel_thread.detach();
}

void client() {
    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  POSE TUNNEL CLIENT - TAP + Tunnel + Pose Receiving").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Step 1: Create TAP interface (or attach if exists)
    echo::info("[1/4] Creating TAP interface...").yellow();
    wirebit::TapConfig tap_cfg{
        .interface_name = "tap1", .create_if_missing = true, .destroy_on_close = true, .set_up_on_create = true};
    auto tap_link_res = wirebit::TapLink::create(tap_cfg);
    if (tap_link_res.is_err()) {
        echo::error("Failed to create TAP: ", tap_link_res.error().message.c_str());
        echo::error("Try: sudo ip link delete tap1  (to clean up)");
        return;
    }
    auto tap_link = std::make_shared<wirebit::TapLink>(std::move(tap_link_res.value()));
    echo::info("✓ TAP interface ready: tap1").green();

    // Assign IP
    system("sudo ip addr add 10.0.0.2/24 dev tap1 2>/dev/null");

    // Step 2: Create Ethernet endpoint
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000};
    wirebit::EthEndpoint eth(tap_link, eth_cfg, 2, local_mac);
    echo::info("✓ Ethernet endpoint ready: MAC=02:00:00:00:00:02").green();

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
            // TAP -> TCP
            eth.process();
            auto eth_result = eth.recv_eth();
            if (eth_result.is_ok()) {
                netpipe::Message tcp_msg(eth_result.value().begin(), eth_result.value().end());
                tunnel.send(tcp_msg);
            }

            // TCP -> TAP
            auto tcp_result = tunnel.recv();
            if (tcp_result.is_ok()) {
                wirebit::Bytes eth_frame(tcp_result.value().begin(), tcp_result.value().end());
                eth.send_eth(eth_frame);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Step 4: Connect to pose server through TAP
    echo::info("[3/4] Connecting to pose server at 10.0.0.1:7777...").yellow();
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Let tunnel stabilize

    netpipe::TcpStream pose_stream;
    netpipe::TcpEndpoint pose_endpoint{"10.0.0.1", 7777};
    auto pose_connect_res = pose_stream.connect(pose_endpoint);
    if (pose_connect_res.is_err()) {
        echo::error("Pose connect failed: ", pose_connect_res.error().message.c_str());
        return;
    }

    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("  ALL CONNECTED! Receiving pose data...").green();
    echo::info("═══════════════════════════════════════════════════════").green();

    int count = 0;
    while (count < 200) {
        auto recv_res = pose_stream.recv();
        if (recv_res.is_err()) {
            echo::error("Recv failed: ", recv_res.error().message.c_str());
            break;
        }

        Pose received_pose = deserialize_pose(recv_res.value());

        if (count % 10 == 0) {
            echo::info("Recv pose #", count, ": pos=(", received_pose.point.x, ", ", received_pose.point.y, ", ",
                       received_pose.point.z, ")")
                .magenta();
        }

        count++;
    }

    echo::info("Client done!").green();
    tunnel_thread.detach();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("═══════════════════════════════════════════════════════");
        echo::info("  POSE TUNNEL - Complete TAP + Pose Streaming");
        echo::info("═══════════════════════════════════════════════════════");
        echo::info("");
        echo::info("Usage: ", argv[0], " [server|client]");
        echo::info("");
        echo::info("This is a COMPLETE example that does EVERYTHING:");
        echo::info("  • Creates TAP interfaces (tap0/tap1)");
        echo::info("  • Runs Ethernet L2 tunnel over TCP");
        echo::info("  • Streams dp::Pose data through the tunnel");
        echo::info("");
        echo::info("Just 2 terminals:");
        echo::info("  Terminal 1: ./pose_tunnel server");
        echo::info("  Terminal 2: ./pose_tunnel client");
        echo::info("");
        echo::info("Monitor with:");
        echo::info("  sudo tcpdump -i tap0 port 7777");
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
