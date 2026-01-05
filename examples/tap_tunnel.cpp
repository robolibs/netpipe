#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>
#include <wirebit/wirebit.hpp>

// Example: Tunnel real Linux TAP interface over netpipe TCP
// This creates a REAL network interface visible to the OS!
//
// Usage:
//   Terminal 1: sudo ./tap_tunnel server
//   Terminal 2: sudo ./tap_tunnel client
//   Terminal 3: sudo tcpdump -i tap0 -XX
//   Terminal 4: ping 10.0.0.2  (from client side)
//
// This demonstrates L2 Ethernet bridging over TCP using REAL hardware interfaces

void tap_tunnel_server() {
    echo::info("TAP Tunnel Server starting...");

    // Create REAL Linux TAP interface (or attach if exists)
    wirebit::TapConfig tap_cfg{
        .interface_name = "tap0", .create_if_missing = true, .destroy_on_close = true, .set_up_on_create = true};

    auto tap_link_res = wirebit::TapLink::create(tap_cfg);
    if (tap_link_res.is_err()) {
        echo::error("Failed to create TAP interface: ", tap_link_res.error().message.c_str());
        echo::error("Try: sudo ip link delete tap0  (to clean up old interface)");
        return;
    }
    auto tap_link = std::make_shared<wirebit::TapLink>(std::move(tap_link_res.value()));

    echo::info("TAP interface created: ", tap_link->interface_name().c_str()).green();
    echo::info("You can now see it with: ip link show ", tap_link->interface_name().c_str()).cyan();
    echo::info("Monitor traffic with: sudo tcpdump -i ", tap_link->interface_name().c_str(), " -XX").cyan();

    // Configure Ethernet endpoint on TAP
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000}; // 1 Gbps
    wirebit::EthEndpoint eth(tap_link, eth_cfg, 1, local_mac);

    echo::info("Ethernet endpoint: MAC=02:00:00:00:00:01");

    // Assign IP address to TAP interface
    echo::info("Assigning IP address 10.0.0.1/24 to tap0...");
    int ret = system("sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null");
    if (ret != 0) {
        echo::warn("Failed to assign IP (may already exist)").yellow();
    }

    // Create netpipe TCP server for tunneling
    netpipe::TcpStream tcp_server;
    netpipe::TcpEndpoint tcp_endpoint{"0.0.0.0", 9001};

    auto listen_res = tcp_server.listen(tcp_endpoint);
    if (listen_res.is_err()) {
        echo::error("TCP listen failed: ", listen_res.error().message.c_str());
        return;
    }

    echo::info("Waiting for tunnel client on port 9001...").yellow();
    auto client_res = tcp_server.accept();
    if (client_res.is_err()) {
        echo::error("TCP accept failed: ", client_res.error().message.c_str());
        return;
    }

    auto tcp_client = std::move(client_res.value());
    echo::info("Tunnel established! Bridging TAP <-> TCP").green();
    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("Try from another terminal:").cyan();
    echo::info("  ping 10.0.0.2").cyan();
    echo::info("  sudo tcpdump -i tap0").cyan();
    echo::info("═══════════════════════════════════════════════════════").green();

    // Bidirectional tunnel loop
    int packet_count = 0;
    while (packet_count < 1000) { // Run for 1000 packets or until error
        // TAP -> TCP tunnel (outgoing packets)
        eth.process();
        auto eth_result = eth.recv_eth();
        if (eth_result.is_ok()) {
            wirebit::Bytes eth_frame = eth_result.value();

            // Parse frame to show what we're tunneling
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(eth_frame, dst, src, ethertype, payload);

            echo::info("TAP → TCP: ", eth_frame.size(), " bytes, ethertype=0x", std::hex, ethertype, std::dec,
                       " dst=", (int)dst[0], ":", (int)dst[1], ":", (int)dst[2], ":", (int)dst[3], ":", (int)dst[4],
                       ":", (int)dst[5])
                .blue();

            // Send Ethernet frame through TCP tunnel
            netpipe::Message tcp_msg(eth_frame.begin(), eth_frame.end());
            auto send_res = tcp_client->send(tcp_msg);
            if (send_res.is_err()) {
                echo::error("TCP send failed: ", send_res.error().message.c_str());
                break;
            }
            packet_count++;
        }

        // TCP tunnel -> TAP (incoming packets)
        auto tcp_result = tcp_client->recv();
        if (tcp_result.is_ok()) {
            netpipe::Message tcp_msg = tcp_result.value();
            wirebit::Bytes eth_frame(tcp_msg.begin(), tcp_msg.end());

            // Parse frame
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(eth_frame, dst, src, ethertype, payload);

            echo::info("TCP → TAP: ", eth_frame.size(), " bytes, ethertype=0x", std::hex, ethertype, std::dec,
                       " src=", (int)src[0], ":", (int)src[1], ":", (int)src[2], ":", (int)src[3], ":", (int)src[4],
                       ":", (int)src[5])
                .magenta();

            // Send frame to TAP interface
            auto send_res = eth.send_eth(eth_frame);
            if (send_res.is_err()) {
                echo::error("TAP send failed: ", send_res.error().message.c_str());
                break;
            }
            packet_count++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    tcp_client->close();
    tcp_server.close();
    echo::info("Tunnel closed");
}

void tap_tunnel_client() {
    echo::info("TAP Tunnel Client starting...");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for server

    // Create REAL Linux TAP interface (or attach if exists)
    wirebit::TapConfig tap_cfg{
        .interface_name = "tap1", .create_if_missing = true, .destroy_on_close = true, .set_up_on_create = true};

    auto tap_link_res = wirebit::TapLink::create(tap_cfg);
    if (tap_link_res.is_err()) {
        echo::error("Failed to create TAP interface: ", tap_link_res.error().message.c_str());
        echo::error("Try: sudo ip link delete tap1  (to clean up old interface)");
        return;
    }
    auto tap_link = std::make_shared<wirebit::TapLink>(std::move(tap_link_res.value()));

    echo::info("TAP interface created: ", tap_link->interface_name().c_str()).green();
    echo::info("You can now see it with: ip link show ", tap_link->interface_name().c_str()).cyan();
    echo::info("Monitor traffic with: sudo tcpdump -i ", tap_link->interface_name().c_str(), " -XX").cyan();

    // Configure Ethernet endpoint on TAP
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000}; // 1 Gbps
    wirebit::EthEndpoint eth(tap_link, eth_cfg, 2, local_mac);

    echo::info("Ethernet endpoint: MAC=02:00:00:00:00:02");

    // Assign IP address to TAP interface
    echo::info("Assigning IP address 10.0.0.2/24 to tap1...");
    int ret = system("sudo ip addr add 10.0.0.2/24 dev tap1 2>/dev/null");
    if (ret != 0) {
        echo::warn("Failed to assign IP (may already exist)").yellow();
    }

    // Connect to TCP tunnel
    netpipe::TcpStream tcp;
    netpipe::TcpEndpoint tcp_endpoint{"127.0.0.1", 9001};

    auto connect_res = tcp.connect(tcp_endpoint);
    if (connect_res.is_err()) {
        echo::error("TCP connect failed: ", connect_res.error().message.c_str());
        return;
    }

    echo::info("Connected to tunnel server!").green();
    echo::info("═══════════════════════════════════════════════════════").green();
    echo::info("Tunnel is now active! Try:").cyan();
    echo::info("  ping 10.0.0.1  (ping the server)").cyan();
    echo::info("  sudo tcpdump -i tap1").cyan();
    echo::info("═══════════════════════════════════════════════════════").green();

    // Bidirectional tunnel loop
    int packet_count = 0;
    while (packet_count < 1000) { // Run for 1000 packets or until error
        // TAP -> TCP tunnel (outgoing packets)
        eth.process();
        auto eth_result = eth.recv_eth();
        if (eth_result.is_ok()) {
            wirebit::Bytes eth_frame = eth_result.value();

            // Parse frame
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(eth_frame, dst, src, ethertype, payload);

            echo::info("TAP → TCP: ", eth_frame.size(), " bytes, ethertype=0x", std::hex, ethertype, std::dec,
                       " dst=", (int)dst[0], ":", (int)dst[1], ":", (int)dst[2], ":", (int)dst[3], ":", (int)dst[4],
                       ":", (int)dst[5])
                .blue();

            // Send through TCP tunnel
            netpipe::Message tcp_msg(eth_frame.begin(), eth_frame.end());
            auto send_res = tcp.send(tcp_msg);
            if (send_res.is_err()) {
                echo::error("TCP send failed: ", send_res.error().message.c_str());
                break;
            }
            packet_count++;
        }

        // TCP tunnel -> TAP (incoming packets)
        auto tcp_result = tcp.recv();
        if (tcp_result.is_ok()) {
            netpipe::Message tcp_msg = tcp_result.value();
            wirebit::Bytes eth_frame(tcp_msg.begin(), tcp_msg.end());

            // Parse frame
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(eth_frame, dst, src, ethertype, payload);

            echo::info("TCP → TAP: ", eth_frame.size(), " bytes, ethertype=0x", std::hex, ethertype, std::dec,
                       " src=", (int)src[0], ":", (int)src[1], ":", (int)src[2], ":", (int)src[3], ":", (int)src[4],
                       ":", (int)src[5])
                .magenta();

            // Send to TAP interface
            auto send_res = eth.send_eth(eth_frame);
            if (send_res.is_err()) {
                echo::error("TAP send failed: ", send_res.error().message.c_str());
                break;
            }
            packet_count++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    tcp.close();
    echo::info("Client done");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("═══════════════════════════════════════════════════════");
        echo::info("  TAP TUNNEL - Real Linux TAP Interface over TCP");
        echo::info("═══════════════════════════════════════════════════════");
        echo::info("");
        echo::info("Usage: ", argv[0], " [server|client|cleanup]");
        echo::info("");
        echo::info("This creates REAL Linux TAP interfaces that are visible");
        echo::info("to the operating system and can be used by any program!");
        echo::info("");
        echo::info("What this does:");
        echo::info("  • Creates tap0 (server) and tap1 (client) interfaces");
        echo::info("  • Assigns IP addresses: 10.0.0.1 and 10.0.0.2");
        echo::info("  • Tunnels all L2 Ethernet frames over TCP");
        echo::info("  • You can ping, tcpdump, wireshark, etc!");
        echo::info("");
        echo::info("Terminal 1: ./tap_tunnel server");
        echo::info("Terminal 2: ./tap_tunnel client");
        echo::info("Terminal 3: ping 10.0.0.2");
        echo::info("Terminal 4: sudo tcpdump -i tap0 -XX");
        echo::info("");
        echo::info("Cleanup: ./tap_tunnel cleanup  (removes tap0 and tap1)");
        echo::info("═══════════════════════════════════════════════════════");
        return 1;
    }

    wirebit::String mode(argv[1]);
    if (mode == "server") {
        tap_tunnel_server();
    } else if (mode == "client") {
        tap_tunnel_client();
    } else if (mode == "cleanup") {
        echo::info("Cleaning up TAP interfaces...");
        system("sudo ip link delete tap0 2>/dev/null");
        system("sudo ip link delete tap1 2>/dev/null");
        echo::info("Cleanup complete!").green();
    } else {
        echo::error("Unknown mode: ", mode.c_str());
        echo::info("Use: server, client, or cleanup");
        return 1;
    }

    return 0;
}
