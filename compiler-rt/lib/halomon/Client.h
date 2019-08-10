#pragma once

#include "boost/asio.hpp"

#include "Messages.pb.h"
#include "MessageKind.h"
#include "Channel.h"

#include <cinttypes>
#include "halomon/Error.h"

namespace asio = boost::asio;
namespace ip = boost::asio::ip;

namespace halo {

class Client {
private:
  asio::io_service IOService;
  ip::tcp::resolver Resolver;
  ip::tcp::socket Socket;
  ip::tcp::resolver::query Query;
  ip::tcp::endpoint Endpoint;

public:
  Channel Chan;

  Client(std::string server_hostname, std::string port) :
    IOService(),
    Resolver(IOService),
    Socket(IOService),
    Query(server_hostname, port),
    Endpoint(*Resolver.resolve(Query)),
    Chan(Socket) {}

  // returns number of handlers that were run
  size_t poll() {
    return IOService.poll();
  }

  // returns true if connection established.
  bool connect() {
    boost::system::error_code Err;
    Socket.connect(Endpoint, Err);

    if (Err) {
      if (LOG) log << "Failed to connect: " << Err.message() << "\n";
      // we have to close the socket manually
      Socket.close();
      return false;
    } else {
      if (LOG) log << "Connected to: " << Endpoint << "\n";
      return true;
    }
  }

};

} // namespace halo
