#include "RelayServer.h"
#include <iostream>
#include <string>
#include <sstream>
#include <TcpServer/EPoll.h>
#include "JsonProtocol.h"

RelayServer::RelayServer()
{
}

int RelayServer::Run()
{
	uWS::Hub h;
	EPoll epoll;

	const char* gameserverHost = getEnvOrDefault(ENV_GAMESERVER_HOST, ENV_GAMESERVER_HOST_DEFAULT);
	const char* gameserverPort = getEnvOrDefault(ENV_GAMESERVER_PORT, ENV_GAMESERVER_PORT_DEFAULT);
	const char* websocketPort = getEnvOrDefault(ENV_WEBSOCKET_PORT, ENV_WEBSOCKET_PORT_DEFAULT);

	fprintf(stderr, "connecting to gameserver on %s port %s...\n", gameserverHost , gameserverPort);
	_clientSocket = connectTcpSocket(gameserverHost , gameserverPort);
	if (_clientSocket < 0)
	{
		perror("connect to server failed");
		return -1;
	}
	fprintf(stderr, "connected.\n");

	_tcpProtocol.SetFrameCompleteCallback(
		[this, &h](uint64_t frame_id)
		{
			auto &logMessages = _tcpProtocol.GetPendingLogItems();
			h.getDefaultGroup<uWS::SERVER>().forEach(
				[this, frame_id, &logMessages](uWS::WebSocket<uWS::SERVER>* sock)
				{
					auto con = static_cast<WebsocketConnection*>(sock->getUserData());
					con->FrameComplete(frame_id, _tcpProtocol);

					auto key = con->getViewerKey();
					if (logMessages.find(key) != logMessages.end())
					{
						for (auto& item: logMessages.at(key))
						{
							con->LogMessage(frame_id, item.message);
						}
					}
				}
			);
			_tcpProtocol.ClearLogItems();

			for (auto& msg: _tcpProtocol.GetPendingMessages())
			{
				std::string s = json(*msg).dump();
				h.getDefaultGroup<uWS::SERVER>().broadcast(s.data(), s.length(), uWS::OpCode::TEXT);
			}
		}
	);

	_tcpProtocol.SetStatsReceivedCallback([this](const MsgPackProtocol::BotStatsMessage& msg) {
		std::string content = json(msg).dump();
		std::stringstream s;
		s << "HTTP/1.0 200 OK\r\n";
		s << "Content-Length: " << content.size() << "\r\n";
		s << "Content-Type: application/json; charset=UTF-8\r\n\r\n";
		s << content;
		_statsHTTPResponse = s.str();
	});

	epoll.AddFileDescriptor(_clientSocket, EPOLLIN|EPOLLPRI|EPOLLERR);

	h.onConnection(
		[](uWS::WebSocket<uWS::SERVER> *ws, uWS::HttpRequest req)
		{
			unsigned int timeout = 1000; // in milliseconds
			if(setsockopt(ws->getFd(), IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(unsigned int)) < 0) {
				std::cerr << "Cannot set user timeout" << std::endl;
			}

			ws->setUserData(new WebsocketConnection(ws));
		}
	);

	h.onDisconnection(
		[](uWS::WebSocket<uWS::SERVER> *ws, int code, const char *message, size_t length)
		{
			auto *con = static_cast<WebsocketConnection*>(ws->getUserData());
			delete con;
		}
	);

	h.onMessage([](uWS::WebSocket<uWS::SERVER> *ws, char *message, size_t length, uWS::OpCode opCode)
	{	
		if (length>MAX_CLIENT_MESSAGE_SIZE)
		{
			ws->close(413, "payload to large");
			return;
		}

		try {
			auto *con = static_cast<WebsocketConnection*>(ws->getUserData());
			std::string s(message, length);
			json data = json::parse(s, nullptr, false);
			if (!data.is_object()) { return; }

			if (data["viewer_key"].is_string())
			{
				std::string key = data["viewer_key"];
				con->setViewerKey(static_cast<uint64_t>(std::stol(key)));
			}
		}
		catch (std::exception e)
		{
			ws->close(418, "invalid request");
			return;
		}
	});

	std::string response = "nope.";
	h.onHttpRequest([&](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t length, size_t remainingBytes)
	{
		if ((req.getMethod()==uWS::METHOD_GET) && (req.getUrl().toString()=="/stats"))
		{
			res->write(_statsHTTPResponse.data(), _statsHTTPResponse.length());
			res->end();
			return;
		}
		res->end(response.data(), response.length());
	});

	if (!h.listen(atoi(websocketPort)))
	{
		return -1;
	}

	epoll.AddFileDescriptor(h.getLoop()->getEpollFd(), EPOLLIN|EPOLLPRI|EPOLLERR|EPOLLRDHUP|EPOLLHUP); // TODO check which events are neccessary
	bool shouldRun = true;
	while (shouldRun)
	{
		epoll.Poll(1000,
			[this, &h, &shouldRun](const epoll_event& ev)
			{
				if (ev.data.fd == _clientSocket)
				{
					shouldRun = _tcpProtocol.Read(_clientSocket);
					return shouldRun;
				}
				else
				{
					h.poll();
				}
				return true;
			}
		);
	}

	return -2;
}

int RelayServer::connectTcpSocket(const char *hostname, const char *port)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; /* Datagram socket */

	struct addrinfo *result;
	int s = getaddrinfo(hostname, port, &hints, &result);
	if (s != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		return s;
	}

	struct addrinfo* rp = nullptr;
	int retval = -1;
	for (rp=result; rp!=nullptr; rp=rp->ai_next)
	{
		int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1) { continue; }

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
		{
			retval = fd;
			break;
		}
		close(fd);
	}

	freeaddrinfo(result);
	return retval;
}

const char *RelayServer::getEnvOrDefault(const char *envVar, const char *defaultValue)
{
	const char* value = getenv(envVar);
	if (value == nullptr)
	{
		value = defaultValue;
	}
	return value;
}
