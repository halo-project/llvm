#pragma once

#include "boost/asio.hpp"

#include "RawSample.pb.h"
#include "MessageKind.h"
#include "Channel.h"

#include <cinttypes>
#include <iostream>

namespace asio = boost::asio;
namespace ip = boost::asio::ip;

namespace halo {

class Client {
private:
  asio::io_service IOService;
  ip::tcp::resolver Resolver;
  ip::tcp::socket Socket;
  ip::tcp::resolver::query Query;

public:
  Channel Chan;

  Client(std::string server_hostname, std::string port) :
    IOService(),
    Resolver(IOService),
    Socket(IOService),
    Query(server_hostname, port),
    Chan(Socket) { }

  // returns true if connection established.
  bool connect() {
    boost::system::error_code Err;
    ip::tcp::resolver::iterator I =
        asio::connect(Socket, Resolver.resolve(Query), Err);

    if (Err) {
      std::cerr << "Failed to connect: " << Err.message() << "\n";
      return false;
    } else {
      std::cerr << "Connected to: " << I->endpoint() << "\n";
      return true;
    }
  }

};

} // namespace halo
