#include "juceRmlUi/rmlTouchRouter.h"

#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/Core.h"
#include "RmlUi/Core/CoreInstance.h"
#include "RmlUi/Core/ElementDocument.h"
#include "RmlUi/Core/Event.h"
#include "RmlUi/Core/EventListener.h"
#include "RmlUi/Core/RenderInterface.h"
#include "RmlUi/Core/SystemInterface.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	using juceRmlUi::TouchRouter;
	using Route = TouchRouter::Route;

	bool check(const bool _condition, const char* _message)
	{
		if(!_condition)
			std::fprintf(stderr, "rmlTouchRouter_test: %s\n", _message);
		return _condition;
	}

	class RenderInterface final : public Rml::RenderInterface
	{
	public:
		explicit RenderInterface(Rml::CoreInstance& _core) : Rml::RenderInterface(_core) {}

		Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override
		{
			return Rml::CompiledGeometryHandle(++m_nextGeometry);
		}
		void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
		void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
		Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
		Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return {}; }
		void ReleaseTexture(Rml::TextureHandle) override {}
		void EnableScissorRegion(bool) override {}
		void SetScissorRegion(Rml::Rectanglei) override {}

	private:
		uintptr_t m_nextGeometry = 0;
	};

	class SystemInterface final : public Rml::SystemInterface
	{
	public:
		explicit SystemInterface(Rml::CoreInstance& _core) : Rml::SystemInterface(_core) {}
		double GetElapsedTime() override { return 1.0; }
	};

	const char* eventName(const Rml::EventId _id)
	{
		switch(_id)
		{
		case Rml::EventId::Mouseout: return "mouseout";
		case Rml::EventId::Mousedown: return "mousedown";
		case Rml::EventId::Mouseup: return "mouseup";
		case Rml::EventId::Click: return "click";
		case Rml::EventId::Drag: return "drag";
		default: return "other";
		}
	}

	// Logs "<event>:<id>#<touch>[@x,y][!cancel]"; touch -1 means the context mouse.
	class EventLog final : public Rml::EventListener
	{
	public:
		void ProcessEvent(Rml::Event& _event) override
		{
			// Listen in the capture phase on the document so every event is logged once.
			const auto* target = _event.GetTargetElement();
			if(!target || target->GetId().empty())
				return;
			auto entry = std::string(eventName(_event.GetId())) + ":" + target->GetId()
				+ "#" + std::to_string(TouchRouter::getTouchId(_event));
			if(_event.GetId() == Rml::EventId::Drag)
				entry += "@" + std::to_string(_event.GetParameter<int>("mouse_x", -1))
					+ "," + std::to_string(_event.GetParameter<int>("mouse_y", -1));
			if(TouchRouter::isTouchCancel(_event))
				entry += "!cancel";
			events.push_back(entry);
		}

		std::vector<std::string> events;
	};

	// Three 100x100 cells in a row: a and b capture touches, c is ordinary UI.
	class Fixture
	{
	public:
		Fixture() : renderer(core), system(core)
		{
			Rml::SetRenderInterface(core, &renderer);
			Rml::SetSystemInterface(core, &system);
			initialized = Rml::Initialise(core);
			if(!initialized)
				return;

			context = Rml::CreateContext(core, "touch-router-test", { 300, 100 });
			if(!context)
				return;

			static constexpr const char* source = R"(
<rml>
<head><style>
body { margin: 0; width: 300px; height: 100px; }
div { position: absolute; top: 0; width: 100px; height: 100px; }
#a { left: 0; }
#b { left: 100px; }
#c { left: 200px; }
#aInner { left: 25px; top: 25px; width: 50px; height: 50px; }
</style></head>
<body><div id="a" touchcapture="1"><div id="aInner"></div></div><div id="b"></div><div id="c"></div></body>
</rml>)";
			document = context->LoadDocumentFromMemory(source);
			if(!document)
				return;

			// Exercise the programmatic path for b.
			TouchRouter::setTouchCapture(document->GetElementById("b"));

			for(const auto event : { Rml::EventId::Mouseout, Rml::EventId::Mousedown,
				Rml::EventId::Mouseup, Rml::EventId::Click, Rml::EventId::Drag })
				document->AddEventListener(event, &log, true);

			document->Show();
			context->Update();
		}

		~Fixture()
		{
			if(document)
				document->Close();
			if(context)
				Rml::RemoveContext(core, "touch-router-test");
			if(initialized)
				Rml::Shutdown(core);
		}

		bool valid() const { return initialized && context && document; }

		Rml::CoreInstance core;
		RenderInterface renderer;
		SystemInterface system;
		Rml::Context* context = nullptr;
		Rml::ElementDocument* document = nullptr;
		EventLog log;
		bool initialized = false;
	};

	bool expectEvents(const EventLog& _log, const std::vector<std::string>& _expected, const char* _scenario)
	{
		if(_log.events == _expected)
			return true;

		std::fprintf(stderr, "rmlTouchRouter_test: %s mismatch\n  actual:", _scenario);
		for(const auto& event : _log.events)
			std::fprintf(stderr, " %s", event.c_str());
		std::fprintf(stderr, "\n  expected:");
		for(const auto& event : _expected)
			std::fprintf(stderr, " %s", event.c_str());
		std::fprintf(stderr, "\n");
		return false;
	}

	// Trig + trig (or Function + trig): lifting either finger releases only its own
	// control, and neither sees a Mouseout.
	bool testIndependentHolds()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		bool ok = true;
		ok &= check(router.down(*f.context, 1, { 50, 10 }, 0) == Route::Captured, "a captured");
		ok &= check(router.down(*f.context, 2, { 150, 50 }, 0) == Route::Captured, "b captured");
		ok &= check(router.up(1, { 50, 10 }, 0) == Route::Captured, "a up");
		ok &= check(router.isCaptured(2) && !router.isCaptured(1), "b still held");
		ok &= check(router.up(2, { 150, 50 }, 0) == Route::Captured, "b up");
		ok &= expectEvents(f.log, {
			"mousedown:a#1", "mousedown:b#2", "mouseup:a#1", "click:a#1", "mouseup:b#2", "click:b#2"
		}, "independent holds");
		return ok;
	}

	// Two fingers on one control: the first lift must not release it.
	bool testSharedControl()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		bool ok = true;
		router.down(*f.context, 1, { 10, 10 }, 0);
		router.down(*f.context, 2, { 50, 50 }, 0);	// lands on aInner, same capture root
		router.move(2, { 60, 60 }, 0);				// joiner does not drag
		router.up(1, { 10, 10 }, 0);				// first finger lifts: still held
		ok &= check(router.isCaptured(2), "second finger still holds a");
		router.move(1, { 20, 20 }, 0);				// stale finger: ignored
		router.up(2, { 60, 60 }, 0);
		ok &= expectEvents(f.log, {
			"mousedown:a#1", "mouseup:aInner#2", "click:aInner#2"
		}, "shared control");
		return ok;
	}

	// Two encoders turned at once: each finger drags its own knob with its own coordinates.
	bool testIndependentDrags()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		router.down(*f.context, 1, { 10, 90 }, 0);
		router.down(*f.context, 2, { 110, 90 }, 0);
		router.move(1, { 10, 80 }, 0);
		router.move(2, { 110, 70 }, 0);
		router.move(2, { 110, 70 }, 0);				// unchanged position: no event
		router.move(1, { 250, 80 }, 0);				// dragging off the control keeps capture
		router.up(1, { 250, 80 }, 0);				// released outside: no click
		router.up(2, { 110, 70 }, 0);
		return expectEvents(f.log, {
			"mousedown:a#1", "mousedown:b#2", "drag:a#1@10,80", "drag:b#2@110,70", "drag:a#1@250,80",
			"mouseup:a#1", "mouseup:b#2", "click:b#2"
		}, "independent drags");
	}

	// Ordinary UI keeps using the context mouse, one finger at a time, alongside captures.
	bool testContextFinger()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		bool ok = true;
		ok &= check(router.down(*f.context, 1, { 250, 50 }, 0) == Route::Context, "c goes to context");
		ok &= check(router.isContextFinger(1), "finger 1 owns context");
		ok &= check(router.down(*f.context, 2, { 260, 50 }, 0) == Route::Ignored, "second context finger ignored");
		ok &= check(router.move(2, { 261, 50 }, 0) == Route::Ignored, "ignored finger stays ignored");
		ok &= check(router.down(*f.context, 3, { 50, 50 }, 0) == Route::Captured, "capture beside context finger");
		ok &= check(router.move(1, { 240, 50 }, 0) == Route::Context, "context finger moves via context");
		ok &= check(router.up(1, { 240, 50 }, 0) == Route::Context, "context finger up via context");
		ok &= check(!router.hasContextFinger(), "context released");
		ok &= check(router.down(*f.context, 2, { 260, 50 }, 0) == Route::Context, "context free again");
		ok &= check(router.isCaptured(3), "capture untouched by context finger");
		return ok;
	}

	// Cancellation (touchesCancelled, focus loss, hiding) releases everything without clicks.
	bool testCancel()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		bool ok = true;
		router.down(*f.context, 1, { 50, 50 }, 0);
		router.down(*f.context, 2, { 150, 50 }, 0);
		router.down(*f.context, 3, { 250, 50 }, 0);
		ok &= check(router.cancelAll(), "cancel reports the context finger");
		ok &= check(router.capturedCount() == 0 && !router.hasContextFinger(), "all fingers gone");
		ok &= check(router.up(1, { 50, 50 }, 0) == Route::Ignored, "late up after cancel ignored");
		ok &= expectEvents(f.log, {
			"mousedown:aInner#1", "mousedown:b#2", "mouseup:aInner#1!cancel", "mouseup:b#2!cancel"
		}, "cancel");
		return ok;
	}

	// A finger id reused before its up arrived must not leave the old control held.
	bool testLostUp()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		router.down(*f.context, 1, { 10, 10 }, 0);
		router.down(*f.context, 1, { 150, 10 }, 0);
		router.up(1, { 150, 10 }, 0);
		return expectEvents(f.log, {
			"mousedown:a#1", "mouseup:a#1!cancel", "mousedown:b#1", "mouseup:b#1", "click:b#1"
		}, "lost up");
	}

	// The held element disappears (skin reload): later events for that finger are dropped safely.
	bool testRemovedElement()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		TouchRouter router;
		router.down(*f.context, 1, { 150, 10 }, 0);
		auto* b = f.document->GetElementById("b");
		b->GetParentNode()->RemoveChild(b);
		f.context->Update();
		router.move(1, { 155, 10 }, 0);
		const auto route = router.up(1, { 155, 10 }, 0);
		return check(route == Route::Captured && router.capturedCount() == 0, "removed element handled")
			& expectEvents(f.log, { "mousedown:b#1" }, "removed element");
	}

	bool testModifiersForwarded()
	{
		Fixture f;
		if(!check(f.valid(), "fixture"))
			return false;
		bool sawAlt = false;
		struct AltProbe final : Rml::EventListener
		{
			bool* seen = nullptr;
			void ProcessEvent(Rml::Event& _event) override { *seen = _event.GetParameter<int>("alt_key", 0) != 0; }
		} probe;
		probe.seen = &sawAlt;
		f.document->GetElementById("b")->AddEventListener(Rml::EventId::Mousedown, &probe);
		TouchRouter router;
		router.down(*f.context, 4, { 150, 10 }, Rml::Input::KM_ALT);
		router.cancelAll();
		f.document->GetElementById("b")->RemoveEventListener(Rml::EventId::Mousedown, &probe);
		return check(sawAlt, "alt modifier forwarded");
	}
}

int main()
{
	bool success = true;
	success &= testIndependentHolds();
	success &= testSharedControl();
	success &= testIndependentDrags();
	success &= testContextFinger();
	success &= testCancel();
	success &= testLostUp();
	success &= testRemovedElement();
	success &= testModifiersForwarded();
	if(success)
		std::puts("rmlTouchRouter_test: PASS");
	return success ? 0 : 1;
}
