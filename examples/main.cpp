#include <netpipe/netpipe.hpp>

int main() {
    echo::info("Netpipe library loaded successfully!");
    echo::info("Available transports:");
    echo::info("  Streams: TcpStream, IpcStream, ShmStream");
    echo::info("  Datagrams: UdpDatagram, LoraDatagram");
    echo::info("  RPC: Rpc layer on top of streams");
    echo::info("");
    echo::info("See examples/tcp_echo_server.cpp and tcp_echo_client.cpp for usage");
    return 0;
}
