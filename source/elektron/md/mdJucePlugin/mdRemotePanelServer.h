#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mdRemotePanelProtocol.h"
#include "mdRemotePanelWire.h"

namespace mdJucePlugin::remotePanel
{
	struct PanelImage {
		uint32_t generation=0, width=0, height=0;
		uint64_t sequence=0, completedUs=0, ledSequence=0;
		std::vector<uint8_t> png, source;
	};

	// Minimal LAN server for the remote panel: RFC 6455 WebSocket (no TLS, no
	// extensions) plus plain HTTP GET for the static panel page. Each client has a
	// reader thread and a writer thread. The writer sends small control messages
	// (acks, pongs) first and builds a display frame only from the newest snapshot,
	// so a slow link drops stale LCD frames instead of queueing them behind input.
	class Server
	{
	public:
		struct Client
		{
			int fd = -1;
			uint32_t id = 0;
			std::string peer;
			std::atomic<bool> open{true};

			// Display state already delivered to this client. Writer thread only.
			std::array<uint8_t, g_vramBytes> sentVram{};
			std::array<uint8_t, g_ledBanks> sentLeds{};
			uint16_t sentWritten = 0;
			uint64_t sentSnapshot = 0;
			uint64_t v2LastFrameUs = 0;
			bool sentValid = false;
			std::atomic<bool> panelSubscribed{false};
			std::atomic<uint32_t> panelReadyGeneration{0};
			std::atomic<uint64_t> panelDeliveredSequence{0};
			std::atomic<uint32_t> panelDeliveredGeneration{0};
			std::shared_ptr<const PanelImage> panelSending;
			size_t panelOffset=0;
			uint32_t panelSourceGeneration=0;
			bool panelSourceAvailable=false;
			uint64_t panelSentSequence=0;
			PanelBudget panelBudget;


		private:
			friend class Server;
			struct Outgoing { std::vector<uint8_t> data; bool text = false; };
			std::mutex mutex;
			std::condition_variable cv;
			std::deque<Outgoing> control;
			bool frameDirty = false;
			std::thread reader;
			std::thread writer;
			std::atomic<bool> readerDone{false};
			std::atomic<bool> writerDone{false};
		};

		using ClientPtr = std::shared_ptr<Client>;
		using OnMessage = std::function<void(const ClientPtr&, const uint8_t*, size_t)>;
		using OnClient = std::function<void(const ClientPtr&)>;
		// Returns the next display frame for this client, or empty if nothing changed.
		using BuildFrame = std::function<std::vector<uint8_t>(Client&)>;

		struct Callbacks
		{
			OnClient onConnect;
			OnMessage onMessage;
			OnClient onDisconnect;
			BuildFrame buildFrame;
			BuildFrame buildPanelFrame;
		};

		Server(Callbacks _callbacks, std::string _webRoot);
		~Server();

		// Binds the first free port in [_port, _port + _attempts).
		bool start(uint16_t _port, uint32_t _attempts);
		void stop();
		uint16_t port() const { return m_port; }

		// Queue a control message; never blocks on the socket.
		bool send(const ClientPtr& _client, std::vector<uint8_t> _data, bool _text = false);
		void sendToAll(const std::vector<uint8_t>& _data, bool _text = false);
		// Ask every writer to build a frame from the newest snapshot.
		void notifyFrame();
		void invalidateFrame(const ClientPtr& _client);

		size_t clientCount();
		uint64_t droppedControlMessages() const { return m_droppedControl.load(); }
		// Joins threads of finished clients. Call periodically from one thread.
		void reap();

	private:
		void acceptLoop();
		void readerLoop(const ClientPtr& _client);
		void writerLoop(const ClientPtr& _client);
		bool handshake(const ClientPtr& _client);
		void serveFile(int _fd, const std::string& _path) const;
		void closeClient(const ClientPtr& _client);

		Callbacks m_callbacks;
		const std::string m_webRoot;
		uint16_t m_port = 0;
		int m_listenFd = -1;
		std::atomic<bool> m_running{false};
		std::thread m_acceptThread;
		std::mutex m_clientsMutex;
		std::vector<ClientPtr> m_clients;	// open and not yet reaped
		uint32_t m_nextId = 1;
		std::atomic<uint64_t> m_droppedControl{0};
	};
}
