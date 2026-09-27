#pragma once

// Wire protocol between the MD/MM plug-in and a remote touch panel (the iPad page
// served by the plug-in itself). Binary WebSocket frames; the first byte is the
// message type; integers are little-endian. Keep in sync with web/panel.js.

#include <cstdint>

namespace mdJucePlugin::remotePanel
{
	enum class Msg : uint8_t
	{
		// panel -> plug-in
		Hello        = 0x01, // [01][protocol:u8]
		Button       = 0x10, // [10][control:u8][down:u8][flags:u8][contact:u16][seq:u32][clientMs:f64]
		EncoderDelta = 0x11, // [11][encoder:u8][detents:i8][flags:u8][contact:u16][seq:u32][clientMs:f64]
		EncoderPress = 0x12, // [12][encoder:u8][down:u8][flags:u8][contact:u16][seq:u32][clientMs:f64]
		ReleaseAll   = 0x13, // [13]  page hidden / pointer cancel storm / reconnect
		Touch        = 0x14, // [14][phase:u8][contact:u16][seq:u32][generation:u32][x:f32][y:f32]
		PanelReady   = 0x15, // [15][generation:u32][capture:u64] decoded complete keyframe
		PanelSource  = 0x84, // source state and geometry, see doc/remote-panel-v3.md
		PanelChunk   = 0x85, // bounded chunks of a lossless PNG keyframe
		TouchAck     = 0x86, // resolution/ingress timing, slot and ownership
		Ping         = 0x20, // [20][opaque:8]

		// plug-in -> panel
		Info         = 0x81, // [81][model:u8 0=MD 1=MM][protocol:u8]
		Frame        = 0x82, // [82][frameSeq:u32][inputEpoch:u32][written:u16][tileMask:u16][leds:14][tiles:64*n]
		Ack          = 0x83, // [83][seq:u32][status:u8][inputEpoch:u32][hostUs:u32]
		Pong         = 0xA0, // [A0][opaque:8][hostUs:u64]
	};

	// Ack status. Every input is acknowledged as soon as it has been handled; the
	// panel never waits for an ack before sending the next edge.
	enum class AckStatus : uint8_t
	{
		Rejected    = 0, // panel queue full (row state is retained by the queue for recovery)
		Accepted    = 1,
		Ignored     = 2, // duplicate press / release without matching press from this contact
		NoDevice    = 3,
		Unsupported = 4, // control not verified for this model
		Unavailable = 6,
		StaleGeometry = 7,
		Miss = 8,
		Malformed = 9,
		Clamped     = 5, // encoder remainder saturated, some detents dropped
	};

	constexpr uint8_t g_protocolVersion = 2;
	constexpr uint8_t g_flagAudioProbe = 0x01; // arm the audio onset probe on this press

	// The LCD travels as 16 tiles: tile = half * 8 + page, 64 column bytes each,
	// bit (y & 7) of byte [x >> 6][y >> 3][x & 63], LSB = top pixel.
	constexpr uint32_t g_tileCount = 16;
	constexpr uint32_t g_tileBytes = 64;
	constexpr uint32_t g_vramBytes = g_tileCount * g_tileBytes;
	constexpr uint32_t g_ledBanks = 14; // raw active-low banks 0x20..0x2d

	// Encoder controls are addressed after the 64 button slots in the ownership table.
	constexpr uint32_t g_buttonSlots = 64;
	constexpr uint32_t g_encoderSlots = 16;
}
