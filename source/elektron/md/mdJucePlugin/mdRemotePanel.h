#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include "juce_gui_basics/juce_gui_basics.h"
#include "RmlUi/Core/ObserverPtr.h"
namespace Rml { class Element; }

#include "mdRemotePanelProtocol.h"
#include "mdRemotePanelOwnership.h"
#include "mdRemotePanelServer.h"

#include "mdLib/mdtypes.h"

namespace synthLib
{
	class Plugin;
}

namespace md
{
	class FrontPanelPublisher;
}

namespace mdJucePlugin
{
	// LAN touch panel for a running MD/MM instance (the iPad page). Additive and
	// desktop-only: it only uses the existing panel ingress (Device::sendPanelEvent
	// under Plugin::withDeviceLocked) and the published front-panel snapshot. No
	// network, encoding or allocation happens on the audio thread; processAudio()
	// is an optional onset probe that only touches atomics.
	//
	// Row ownership: PanelRowState is a plain bitmask, so the desktop editor and a
	// remote finger could release each other's held bits. Every scan-row write from
	// either side therefore goes through submitEditorRow()/the remote path here,
	// which keeps a refcount per remote (row, bit) and ORs it with the editor's mask.
	class Editor;
	class RemotePanel : private juce::AsyncUpdater, private juce::Timer
	{
	public:
		struct Config
		{
			uint16_t port = 7788;
			uint32_t portAttempts = 8;
			std::string webRoot;
			std::string logFile;
			uint32_t maxFps = 60;
		};

		RemotePanel(synthLib::Plugin& _plugin, md::MachineModel _model, Config _config);
		~RemotePanel();

		void attachEditor(Editor*); // message thread only
		void detachEditor(Editor*);

		bool isRunning() const { return m_running; }
		uint16_t port() const { return m_server.port(); }

		static bool isRowCommand(uint8_t _command) { return _command >= 0x20 && _command <= 0x26; }
		// Desktop editor scan-row write, merged with remote holds. Returns queue acceptance.
		bool submitEditorRow(uint8_t _row, uint8_t _mask);

		// Audio thread, after the device rendered. Lock-free, allocation-free.
		// _callbackStartNs is steady_clock time taken on entry to processBlock.
		void processAudio(const float* _left, const float* _right, int _frames, double _sampleRate,
			uint64_t _callbackStartNs) noexcept;

	private:
		using ClientPtr = remotePanel::Server::ClientPtr;

		struct Snapshot
		{
			std::array<uint8_t, remotePanel::g_vramBytes> vram{};
			std::array<uint8_t, remotePanel::g_ledBanks> leds{};
			uint16_t written = 0;
			uint64_t sequence = 0;
			uint32_t inputEpoch = 0;
		};

		struct ClientState
		{
			// (contact, slot) pairs this client currently holds.
			std::map<std::pair<uint16_t, uint8_t>, bool> holds;
			uint64_t lastReleaseUs=0;
		};

		struct Stats
		{
			std::vector<uint32_t> enqueueUs;	// receive -> accepted by the panel queue
			std::vector<uint32_t> lcdUs;		// accepted -> first changed published snapshot
			std::vector<uint32_t> audioUs;		// receive -> first non-silent rendered sample
		};

		void onConnect(const ClientPtr& _client);
		void onMessage(const ClientPtr& _client, const uint8_t* _data, size_t _size);
		void onDisconnect(const ClientPtr& _client);
		std::vector<uint8_t> buildFrame(remotePanel::Server::Client& _client);

		remotePanel::AckStatus handleHold(uint32_t _clientId, uint8_t _slot, bool _down, uint16_t _contact);
		remotePanel::AckStatus applySlot(uint8_t _slot, bool _down);
		remotePanel::AckStatus sendRowLocked(uint8_t _row);
		remotePanel::AckStatus handleEncoderDelta(uint8_t _encoder, int _detents);
		void flushEncoderRemainders();
		void releaseClient(uint32_t _clientId);
		bool pushPanel(uint8_t _command, uint8_t _argument, bool& _haveDevice);

		void pumpLoop();
		bool pollDisplay();
		void reportStats(double _seconds);
		void log(const std::string& _line);

		synthLib::Plugin& m_plugin;
		const md::MachineModel m_model;
		const Config m_config;
		remotePanel::Server m_server;
		std::atomic<bool> m_running{false};
		std::thread m_pump;
		std::thread m_captureWorker;
		std::atomic<unsigned> m_panelClients{0};
		std::atomic<bool> m_controlsRequested{false};

		// Row merge point shared with the desktop editor.
		std::mutex m_rowMutex;
		std::array<uint8_t, 7> m_editorRows{};
		remotePanel::RowOwners m_rowOwners;

		std::mutex m_clientsMutex;
		std::map<uint32_t, ClientState> m_clients;

		// Encoder detents waiting to be emitted, bounded.
		std::mutex m_encoderMutex;
		std::array<int, remotePanel::g_encoderSlots> m_encoderPending{};

		// Newest published display snapshot.
		std::mutex m_snapshotMutex;
		Snapshot m_snapshot;
		std::shared_ptr<md::FrontPanelPublisher> m_publisher;

		std::atomic<uint32_t> m_inputEpoch{0};
		std::atomic<uint64_t> m_lcdProbeStartUs{0};

		// Audio onset probe (audio thread writes results, pump reads them).
		std::atomic<uint64_t> m_audioProbeArmedNs{0};
		std::atomic<uint64_t> m_audioProbeResultNs{0};
		std::atomic<uint32_t> m_audioProbeOffsetFrames{0};
		std::atomic<uint32_t> m_audioBlockFrames{0};
		std::atomic<uint32_t> m_audioSampleRate{0};
		uint64_t m_silentFrames = 0;	// audio thread only
		std::atomic<float> m_audioPeak{0.0f};	// since the last stats line


		struct Geometry {
			float originX=0, originY=0, width=0, height=0, scale=1;
			uint32_t contextWidth=0, contextHeight=0;
			bool operator==(const Geometry& b) const {
				return originX==b.originX && originY==b.originY && width==b.width && height==b.height
					&& scale==b.scale && contextWidth==b.contextWidth && contextHeight==b.contextHeight;
			}
		};
		struct Capture {
			juce::Image image;
			Geometry geometry;
			uint32_t generation=0;
			uint64_t sequence=0, completedUs=0, ledSequence=0;
		};
		struct TouchEvent {
			ClientPtr client;
			uint8_t phase=0;
			uint16_t contact=0;
			uint32_t sequence=0, generation=0;
			float x=0,y=0;
			uint64_t receivedUs=0;
		};
		struct Contact {
			Rml::ObserverPtr<Rml::Element> element;
			uint8_t slot=0;
			float startX=0,startY=0,x=0,y=0, fraction=0;
			uint64_t downUs=0;
			bool held=false, holdEligible=true, drives=true;
			uint32_t generation=0;
		};
		void timerCallback() override;
		void handleAsyncUpdate() override;
		void queueTouch(const ClientPtr&, const uint8_t*, size_t, uint64_t);
		void servicePanelCapture(); // worker only, no DOM
		void sourceUnavailable(uint8_t reason);
		void clearContacts(uint32_t client=0); // owner only
		void updateContactVisual(Rml::Element*);
		std::vector<uint8_t> buildPanelFrame(remotePanel::Server::Client&);
		void sendSource(const ClientPtr&);
		Editor* m_editor=nullptr; // message thread only
		std::shared_ptr<int> m_captureLifetime=std::make_shared<int>(0);
		std::map<std::pair<uint32_t,uint16_t>, Contact> m_contacts;
		std::mutex m_touchMutex;
		std::deque<TouchEvent> m_touchEvents;
		std::mutex m_panelMutex;
		Capture m_pendingCapture;
		std::shared_ptr<const remotePanel::PanelImage> m_panelImage;
		Geometry m_geometry;
		std::atomic<uint32_t> m_geometryGeneration{1};
		std::atomic<bool> m_sourceAvailable{false};
		std::atomic<uint64_t> m_lastCaptureUs{0};
		std::atomic<uint8_t> m_sourceReason{1};
		uint64_t m_lastCaptureRequestUs=0;
		std::atomic<uint64_t> m_captureRequested{0},m_captureAccepted{0},m_captureCompleted{0};
		std::atomic<uint64_t> m_panelBytes{0},m_panelFrames{0},m_touchReceived{0};
		std::vector<uint8_t> m_lastPng; // worker only

		// Counters.
		std::atomic<uint64_t> m_inputs{0};
		std::atomic<uint64_t> m_rejectedRows{0};
		std::atomic<uint64_t> m_droppedPulses{0};
		std::atomic<uint64_t> m_clampedDetents{0};
		std::atomic<uint64_t> m_framesSent{0};
		std::atomic<uint64_t> m_bytesSent{0};
		std::atomic<uint64_t> m_displayChanges{0};
		std::atomic<uint64_t> m_audioProbeTimeouts{0};

		std::mutex m_statsMutex;
		Stats m_stats;
		std::mutex m_logMutex;
	};
}
