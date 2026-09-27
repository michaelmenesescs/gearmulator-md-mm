#pragma once

#include "RmlUi/Core/ObserverPtr.h"
#include "RmlUi/Core/Types.h"

#include <map>
#include <optional>

namespace Rml
{
	class Context;
	class Element;
	class Event;
}

namespace juceRmlUi
{
	// Per-finger pointer routing for touch screens.
	//
	// RmlUi models exactly one mouse. Feeding several fingers through it makes every
	// new finger move that mouse, which fires Mouseout on the control the previous
	// finger is holding and releases it. The router therefore splits touches in two:
	//
	// - A finger that lands on a touch-capture element (see setTouchCapture(); every
	//   ElemKnob is one) bypasses the context mouse entirely. It owns that element until
	//   it lifts: Mousedown, Drag and Mouseup (+ Click when lifted inside) are dispatched
	//   straight to the element it hit, never Mouseout. Events carry a "touch_id"
	//   parameter so listeners can tell fingers apart.
	// - Any other finger is routed through the context as a normal mouse, but only one
	//   at a time; further fingers on non-capture UI are ignored while it is down.
	//
	// Several fingers on the same capture element share it: only the first one sends
	// Mousedown/Drag, and the Mouseup is sent only when the last of them lifts. A lifted finger
	// therefore never releases a control another finger still holds.
	class TouchRouter
	{
	public:
		enum class Route
		{
			Captured,	// the router dispatched (or deliberately suppressed) the events itself
			Context,	// the caller must feed this finger through the Rml context
			Ignored		// drop the event
		};

		static constexpr const char* g_captureAttribute = "touchcapture";
		static constexpr const char* g_touchIdParameter = "touch_id";
		static constexpr const char* g_touchCancelParameter = "touch_cancel";

		static void setTouchCapture(Rml::Element* _element, bool _capture = true);
		static Rml::Element* findCaptureRoot(Rml::Element* _hit);

		// Returns the touch id carried by an event dispatched by the router, or -1 for
		// events that came through the context (ordinary mouse, or the context finger).
		static int getTouchId(const Rml::Event& _event);
		static bool isTouchCancel(const Rml::Event& _event);

		Route down(Rml::Context& _context, int _touchId, Rml::Vector2i _position, int _keyModifiers);
		Route move(int _touchId, Rml::Vector2i _position, int _keyModifiers);
		Route up(int _touchId, Rml::Vector2i _position, int _keyModifiers);

		// Releases every captured finger with a Mouseup that carries touch_cancel=1 and no
		// Click. Returns true if a finger was routed through the context; the caller must
		// then release the context mouse button itself.
		bool cancelAll(int _keyModifiers = 0);

		// Drops every finger without dispatching anything, for teardown.
		void forgetAll()
		{
			m_fingers.clear();
			m_contextFinger.reset();
		}

		bool isCaptured(int _touchId) const { return m_fingers.find(_touchId) != m_fingers.end(); }
		bool hasContextFinger() const { return m_contextFinger.has_value(); }
		bool isContextFinger(const int _touchId) const { return m_contextFinger && *m_contextFinger == _touchId; }
		size_t capturedCount() const { return m_fingers.size(); }

	private:
		struct Finger
		{
			Rml::ObserverPtr<Rml::Element> target;	// deepest element hit on touch-down
			Rml::ObserverPtr<Rml::Element> root;	// the touch-capture element that owns the finger
			Rml::Vector2i position;
			bool drags = false;						// only the first finger on a root sends Mousedown/Drag
		};

		bool rootHasOtherFinger(const Rml::Element* _root, int _exceptTouchId) const;
		void release(int _touchId, Rml::Vector2i _position, int _keyModifiers, bool _cancel);

		std::map<int, Finger> m_fingers;
		std::optional<int> m_contextFinger;
	};
}
