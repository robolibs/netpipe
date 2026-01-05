#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>
#include <wirebit/wirebit.hpp>

// Example: Tunnel wirebit Ethernet frames over netpipe TCP
// This demonstrates bridging L2 Ethernet over a TCP connection

void ethernet_tunnel_server() {
    echo::info("Ethernet Tunnel Server starting...");

    // Create wirebit Ethernet endpoint (simulated network interface)
    auto eth_link_res = wirebit::ShmLink::create(wirebit::String("eth0"), 1024 * 1024);
    if (eth_link_res.is_err()) {
        echo::error("Failed to create ethernet link");
        return;
    }
    auto eth_link = std::make_shared<wirebit::ShmLink>(std::move(eth_link_res.value()));

    // Configure Ethernet with MAC address
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000}; // 1 Gbps
    wirebit::EthEndpoint eth(eth_link, eth_cfg, 1, local_mac);

    echo::info("Local Ethernet: MAC=02:00:00:00:00:01");

    // Create netpipe TCP server for tunneling
    netpipe::TcpStream tcp_server;
    netpipe::TcpEndpoint tcp_endpoint{"0.0.0.0", 9001};

    auto listen_res = tcp_server.listen(tcp_endpoint);
    if (listen_res.is_err()) {
        echo::error("TCP listen failed: ", listen_res.error().message.c_str());
        return;
    }

    echo::info("Waiting for tunnel client on port 9001...");
    auto client_res = tcp_server.accept();
    if (client_res.is_err()) {
        echo::error("TCP accept failed: ", client_res.error().message.c_str());
        return;
    }

    auto tcp_client = std::move(client_res.value());
    echo::info("Tunnel established! Bridging Ethernet <-> TCP");

    // Tunnel loop
    for (int i = 0; i < 20; i++) {
        // Ethernet -> TCP tunnel
        eth.process();
        auto eth_result = eth.recv_eth();
        if (eth_result.is_ok()) {
            wirebit::Bytes eth_frame = eth_result.value();

            // Parse frame to show what we're tunneling
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(eth_frame, dst, src, ethertype, payload);

            echo::debug("Ethernet -> TCP: ", eth_frame.size(), " bytes, ethertype=0x", std::hex, ethertype, std::dec);

            // Send Ethernet frame through TCP tunnel
            netpipe::Message tcp_msg(eth_frame.begin(), eth_frame.end());
            auto send_res = tcp_client->send(tcp_msg);
            if (send_res.is_err()) {
                echo::error("TCP send failed: ", send_res.error().message.c_str());
                break;
            }
        }

        // TCP tunnel -> Ethernet
        auto tcp_result = tcp_client->recv();
        if (tcp_result.is_ok()) {
            netpipe::Message tcp_msg = tcp_result.value();
            wirebit::Bytes eth_frame(tcp_msg.begin(), tcp_msg.end());

            echo::debug("TCP -> Ethernet: ", eth_frame.size(), " bytes");

            // Send frame to local Ethernet
            auto send_res = eth.send_eth(eth_frame);
            if (send_res.is_err()) {
                echo::error("Ethernet send failed: ", send_res.error().message.c_str());
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    tcp_client->close();
    tcp_server.close();
    echo::info("Tunnel closed");
}

void ethernet_tunnel_client() {
    echo::info("Ethernet Tunnel Client starting...");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for server

    // Attach to wirebit Ethernet endpoint
    auto eth_link_res = wirebit::ShmLink::attach(wirebit::String("eth0"));
    if (eth_link_res.is_err()) {
        echo::error("Failed to attach to ethernet link");
        return;
    }
    auto eth_link = std::make_shared<wirebit::ShmLink>(std::move(eth_link_res.value()));

    // Configure Ethernet with different MAC
    wirebit::MacAddr local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000}; // 1 Gbps
    wirebit::EthEndpoint eth(eth_link, eth_cfg, 2, local_mac);

    echo::info("Local Ethernet: MAC=02:00:00:00:00:02");

    // Connect to TCP tunnel
    netpipe::TcpStream tcp;
    netpipe::TcpEndpoint tcp_endpoint{"127.0.0.1", 9001};

    auto connect_res = tcp.connect(tcp_endpoint);
    if (connect_res.is_err()) {
        echo::error("TCP connect failed: ", connect_res.error().message.c_str());
        return;
    }

    echo::info("Connected to tunnel server!");

    // Send test Ethernet frames through the tunnel
    wirebit::MacAddr remote_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

    for (int i = 0; i < 5; i++) {
        // Create test payload
        wirebit::String test_str = wirebit::String("Ethernet packet #") + wirebit::String(std::to_string(i).c_str());
        wirebit::Bytes payload(test_str.begin(), test_str.end());

        // Create Ethernet frame (IPv4 ethertype 0x0800)
        wirebit::Bytes eth_frame = wirebit::make_eth_frame(remote_mac, // Destination MAC
                                                           local_mac,  // Source MAC
                                                           0x0800,     // Ethertype (IPv4)
                                                           payload);

        echo::info("Sending Ethernet frame: ", test_str.c_str(), " (", eth_frame.size(), " bytes)");

        // Send through local Ethernet (will be tunneled via TCP)
        auto send_res = eth.send_eth(eth_frame);
        if (send_res.is_err()) {
            echo::error("Ethernet send failed");
            break;
        }

        // Receive response through tunnel
        eth.process();
        auto recv_res = eth.recv_eth();
        if (recv_res.is_ok()) {
            wirebit::Bytes received_frame = recv_res.value();

            // Parse received frame
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes recv_payload;
            wirebit::parse_eth_frame(received_frame, dst, src, ethertype, recv_payload);

            wirebit::String recv_str(reinterpret_cast<const char *>(recv_payload.data()));
            echo::info("Received frame: ", recv_str.c_str(), " (", received_frame.size(), " bytes)");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    tcp.close();
    echo::info("Client done");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("Usage: ", argv[0], " [server|client]");
        echo::info("");
        echo::info("Ethernet Tunnel Example - Bridge L2 Ethernet over TCP");
        echo::info("This demonstrates how wirebit Ethernet endpoints can be");
        echo::info("tunneled over netpipe TCP connections.");
        echo::info("");
        echo::info("  server - Start the Ethernet tunnel server");
        echo::info("  client - Connect and send Ethernet frames");
        return 1;
    }

    wirebit::String mode(argv[1]);
    if (mode == "server") {
        ethernet_tunnel_server();
    } else if (mode == "client") {
        ethernet_tunnel_client();
    } else {
        echo::error("Unknown mode: ", mode.c_str());
        return 1;
    }

    return 0;
}
