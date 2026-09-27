#include "mdRemotePanelServer.h"

#include <CommonCrypto/CommonDigest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mdJucePlugin::remotePanel
{
	namespace
	{
		constexpr size_t g_maxControlMessages = 1024;
		constexpr size_t g_maxMessageBytes = 64 * 1024;

		std::string base64(const uint8_t* _data, const size_t _size)
		{
			static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string out;
			for(size_t i = 0; i < _size; i += 3)
			{
				const uint32_t b0 = _data[i];
				const uint32_t b1 = i + 1 < _size ? _data[i + 1] : 0;
				const uint32_t b2 = i + 2 < _size ? _data[i + 2] : 0;
				const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
				out += table[(v >> 18) & 63];
				out += table[(v >> 12) & 63];
				out += i + 1 < _size ? table[(v >> 6) & 63] : '=';
				out += i + 2 < _size ? table[v & 63] : '=';
			}
			return out;
		}

		bool readExact(const int _fd, uint8_t* _dst, size_t _size)
		{
			while(_size)
			{
				const auto n = ::recv(_fd, _dst, _size, 0);
				if(n <= 0)
					return false;
				_dst += n;
				_size -= static_cast<size_t>(n);
			}
			return true;
		}

		bool writeAll(const int _fd, const uint8_t* _src, size_t _size)
		{
			while(_size)
			{
				const auto n = ::send(_fd, _src, _size, 0);
				if(n <= 0)
					return false;
				_src += n;
				_size -= static_cast<size_t>(n);
			}
			return true;
		}

		bool writeString(const int _fd, const std::string& _s)
		{
			return writeAll(_fd, reinterpret_cast<const uint8_t*>(_s.data()), _s.size());
		}

		std::vector<uint8_t> wsFrame(const std::vector<uint8_t>& _payload, const bool _text)
		{
			std::vector<uint8_t> out;
			out.reserve(_payload.size() + 10);
			out.push_back(_text ? 0x81 : 0x82);	// FIN + text/binary
			const auto size = _payload.size();
			if(size < 126)
			{
				out.push_back(static_cast<uint8_t>(size));
			}
			else if(size < 65536)
			{
				out.push_back(126);
				out.push_back(static_cast<uint8_t>(size >> 8));
				out.push_back(static_cast<uint8_t>(size));
			}
			else
			{
				out.push_back(127);
				for(int i = 0; i < 8; ++i)
					out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (56 - 8 * i)));
			}
			out.insert(out.end(), _payload.begin(), _payload.end());
			return out;
		}

		const char* contentType(const std::string& _path)
		{
			const auto dot = _path.rfind('.');
			const auto ext = dot == std::string::npos ? std::string() : _path.substr(dot + 1);
			if(ext == "html") return "text/html; charset=utf-8";
			if(ext == "js") return "text/javascript; charset=utf-8";
			if(ext == "css") return "text/css; charset=utf-8";
			if(ext == "json") return "application/json";
			if(ext == "png") return "image/png";
			if(ext == "svg") return "image/svg+xml";
			if(ext == "webmanifest") return "application/manifest+json";
			return "application/octet-stream";
		}
	}

	Server::Server(Callbacks _callbacks, std::string _webRoot)
		: m_callbacks(std::move(_callbacks)), m_webRoot(std::move(_webRoot))
	{
	}

	Server::~Server()
	{
		stop();
	}

	bool Server::start(const uint16_t _port, const uint32_t _attempts)
	{
		for(uint32_t i = 0; i < _attempts; ++i)
		{
			const auto port = static_cast<uint16_t>(_port + i);
			const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
			if(fd < 0)
				return false;
			int one = 1;
			setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = htonl(INADDR_ANY);
			if(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(fd, 16) == 0)
			{
				m_listenFd = fd;
				m_port = port;
				m_running = true;
				m_acceptThread = std::thread([this] { acceptLoop(); });
				return true;
			}
			::close(fd);
		}
		return false;
	}

	void Server::stop()
	{
		if(!m_running.exchange(false))
			return;
		if(m_acceptThread.joinable())
			m_acceptThread.join();
		::close(m_listenFd);
		m_listenFd = -1;

		std::vector<ClientPtr> clients;
		{
			std::lock_guard lock(m_clientsMutex);
			clients.swap(m_clients);
		}
		for(auto& c : clients)
			closeClient(c);
		for(auto& c : clients)
		{
			if(c->reader.joinable())
				c->reader.join();
			if(c->writer.joinable())
				c->writer.join();
		}
	}

	void Server::closeClient(const ClientPtr& _client)
	{
		{
			std::lock_guard lock(_client->mutex);
			_client->open = false;
		}
		_client->cv.notify_all();
		::shutdown(_client->fd, SHUT_RDWR);
	}

	size_t Server::clientCount()
	{
		std::lock_guard lock(m_clientsMutex);
		return static_cast<size_t>(std::count_if(m_clients.begin(), m_clients.end(),
			[](const ClientPtr& _c) { return _c->open.load(); }));
	}

	void Server::reap()
	{
		std::vector<ClientPtr> finished;
		{
			std::lock_guard lock(m_clientsMutex);
			for(auto it = m_clients.begin(); it != m_clients.end();)
			{
				if((*it)->readerDone && (*it)->writerDone)
				{
					finished.push_back(*it);
					it = m_clients.erase(it);
				}
				else
					++it;
			}
		}
		for(auto& c : finished)
		{
			if(c->reader.joinable())
				c->reader.join();
			if(c->writer.joinable())
				c->writer.join();
			::close(c->fd);
		}
	}

	void Server::acceptLoop()
	{
		while(m_running)
		{
			pollfd p{m_listenFd, POLLIN, 0};
			if(::poll(&p, 1, 200) <= 0)
				continue;
			sockaddr_in peer{};
			socklen_t len = sizeof(peer);
			const int fd = ::accept(m_listenFd, reinterpret_cast<sockaddr*>(&peer), &len);
			if(fd < 0)
				continue;
			int one = 1;
			// Panel edges are tiny and latency-critical: never let Nagle batch them.
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
			timeval tv{2, 0};
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

			auto client = std::make_shared<Client>();
			client->fd = fd;
			char ip[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
			client->peer = ip;

			std::lock_guard lock(m_clientsMutex);
			client->id = m_nextId++;
			m_clients.push_back(client);
			client->reader = std::thread([this, client] { readerLoop(client); });
		}
	}

	void Server::serveFile(const int _fd, const std::string& _target) const
	{
		auto path = _target.substr(0, _target.find('?'));
		if(path.empty() || path == "/")
			path = "/index.html";
		const bool safe = path.find("..") == std::string::npos && path.find('\\') == std::string::npos
			&& path[0] == '/';
		std::ifstream file;
		if(safe && !m_webRoot.empty())
			file.open(m_webRoot + path, std::ios::binary);
		if(!file)
		{
			writeString(_fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n"
				"Connection: close\r\n\r\nnot found\n");
			return;
		}
		std::stringstream body;
		body << file.rdbuf();
		const auto data = body.str();
		std::string header = "HTTP/1.1 200 OK\r\nContent-Type: ";
		header += contentType(path);
		header += "\r\nContent-Length: " + std::to_string(data.size());
		header += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
		writeString(_fd, header);
		writeString(_fd, data);
	}

	bool Server::handshake(const ClientPtr& _client)
	{
		const int fd = _client->fd;
		std::string req;
		char c;
		while(req.size() < 8192)
		{
			if(::recv(fd, &c, 1, 0) != 1)
				return false;
			req += c;
			if(req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0)
				break;
		}
		std::string key;
		std::string target = "/";
		size_t pos = 0;
		bool first = true;
		while(pos < req.size())
		{
			const auto eol = req.find("\r\n", pos);
			if(eol == std::string::npos)
				break;
			const auto line = req.substr(pos, eol - pos);
			pos = eol + 2;
			if(first)
			{
				first = false;
				const auto a = line.find(' ');
				const auto b = line.find(' ', a + 1);
				if(a != std::string::npos && b != std::string::npos)
					target = line.substr(a + 1, b - a - 1);
				continue;
			}
			const auto colon = line.find(':');
			if(colon == std::string::npos)
				continue;
			std::string name = line.substr(0, colon);
			for(auto& ch : name)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if(name == "sec-websocket-key")
			{
				key = line.substr(colon + 1);
				key.erase(0, key.find_first_not_of(' '));
				key.erase(key.find_last_not_of(" \r") + 1);
			}
		}
		if(key.empty())
		{
			serveFile(fd, target);
			return false;
		}
		const std::string accept = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		uint8_t digest[CC_SHA1_DIGEST_LENGTH];
		CC_SHA1(accept.data(), static_cast<CC_LONG>(accept.size()), digest);
		return writeString(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			"Sec-WebSocket-Accept: " + base64(digest, sizeof(digest)) + "\r\n\r\n");
	}

	void Server::readerLoop(const ClientPtr& _client)
	{
		const int fd = _client->fd;
		if(!handshake(_client))
		{
			_client->open = false;
			::shutdown(fd, SHUT_RDWR);
			_client->writerDone = true;
			_client->readerDone = true;
			return;
		}
		std::printf("[remotePanel] client %u connected from %s\n", _client->id, _client->peer.c_str());
		_client->writer = std::thread([this, _client] { writerLoop(_client); });
		m_callbacks.onConnect(_client);

		std::vector<uint8_t> payload;
		std::vector<uint8_t> message;
		while(m_running && _client->open)
		{
			uint8_t hdr[2];
			if(!readExact(fd, hdr, 2))
				break;
			const bool fin = hdr[0] & 0x80;
			const uint8_t opcode = hdr[0] & 0x0f;
			const bool masked = hdr[1] & 0x80;
			uint64_t len = hdr[1] & 0x7f;
			if(len == 126)
			{
				uint8_t ext[2];
				if(!readExact(fd, ext, 2))
					break;
				len = (uint64_t(ext[0]) << 8) | ext[1];
			}
			else if(len == 127)
			{
				uint8_t ext[8];
				if(!readExact(fd, ext, 8))
					break;
				len = 0;
				for(const auto b : ext)
					len = (len << 8) | b;
			}
			if(len > g_maxMessageBytes)
				break;
			uint8_t mask[4]{};
			if(masked && !readExact(fd, mask, 4))
				break;
			payload.resize(static_cast<size_t>(len));
			if(len && !readExact(fd, payload.data(), payload.size()))
				break;
			if(masked)
				for(size_t i = 0; i < payload.size(); ++i)
					payload[i] ^= mask[i & 3];

			if(opcode == 0x8)
				break;
			if(opcode == 0x9)
			{
				// WebSocket ping: answer through the writer as a pong control frame.
				std::vector<uint8_t> pong{0x8a, static_cast<uint8_t>(std::min<size_t>(payload.size(), 125))};
				pong.insert(pong.end(), payload.begin(), payload.begin() + pong[1]);
				std::lock_guard lock(_client->mutex);
				_client->control.push_back({std::move(pong), false});
				_client->cv.notify_one();
				continue;
			}
			if(opcode == 0xa)
				continue;
			message.insert(message.end(), payload.begin(), payload.end());
			if(message.size() > g_maxMessageBytes)
				break;
			if(!fin)
				continue;
			if(!message.empty())
				m_callbacks.onMessage(_client, message.data(), message.size());
			message.clear();
		}

		closeClient(_client);
		m_callbacks.onDisconnect(_client);
		std::printf("[remotePanel] client %u disconnected\n", _client->id);
		_client->readerDone = true;
	}

	void Server::writerLoop(const ClientPtr& _client)
	{
		std::deque<Client::Outgoing> pending;
		while(true)
		{
			bool frame = false;
			{
				std::unique_lock lock(_client->mutex);
				_client->cv.wait(lock, [&] {
					return !_client->open || !_client->control.empty() || _client->frameDirty;
				});
				if(!_client->open)
					break;
				pending.swap(_client->control);
				frame = _client->frameDirty;
				_client->frameDirty = false;
			}
			bool ok = true;
			for(auto& m : pending)
			{
				// Raw pre-framed pongs start with 0x8a; everything else is a payload.
				const bool raw = !m.data.empty() && m.data[0] == 0x8a && !m.text && m.data.size() >= 2
					&& m.data.size() == static_cast<size_t>(m.data[1]) + 2;
				const auto bytes = raw ? m.data : wsFrame(m.data, m.text);
				if(!writeAll(_client->fd, bytes.data(), bytes.size()))
				{
					ok = false;
					break;
				}
			}
			pending.clear();
			if(ok && frame && m_callbacks.buildFrame)
			{
				const auto payload = m_callbacks.buildFrame(*_client);
				if(!payload.empty())
				{
					const auto bytes = wsFrame(payload, false);
					ok = writeAll(_client->fd, bytes.data(), bytes.size());
				}
			}
			// At most one bounded panel chunk between control-queue drains.
			if(ok && frame && m_callbacks.buildPanelFrame) {
				const auto payload = m_callbacks.buildPanelFrame(*_client);
				if(!payload.empty()) {
					const auto bytes = wsFrame(payload, false);
					ok = writeAll(_client->fd, bytes.data(), bytes.size());
				}
			}
			if(!ok)
			{
				closeClient(_client);
				break;
			}
		}
		_client->writerDone = true;
	}

	bool Server::send(const ClientPtr& _client, std::vector<uint8_t> _data, const bool _text)
	{
		std::lock_guard lock(_client->mutex);
		if(!_client->open)
			return false;
		if(_client->control.size() >= g_maxControlMessages)
		{
			++m_droppedControl;
			return false;
		}
		_client->control.push_back({std::move(_data), _text});
		_client->cv.notify_one();
		return true;
	}

	void Server::sendToAll(const std::vector<uint8_t>& _data, const bool _text)
	{
		std::vector<ClientPtr> clients;
		{
			std::lock_guard lock(m_clientsMutex);
			clients = m_clients;
		}
		for(auto& c : clients)
			if(c->open && c->writer.joinable())
				send(c, _data, _text);
	}

	void Server::notifyFrame()
	{
		std::vector<ClientPtr> clients;
		{
			std::lock_guard lock(m_clientsMutex);
			clients = m_clients;
		}
		for(auto& c : clients)
		{
			std::lock_guard lock(c->mutex);
			c->frameDirty = true;
			c->cv.notify_one();
		}
	}

	void Server::invalidateFrame(const ClientPtr& _client)
	{
		std::lock_guard lock(_client->mutex);
		_client->frameDirty = true;
		_client->cv.notify_one();
	}
}
