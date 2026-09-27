#include "rmlTouchRouter.h"

#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/Event.h"

namespace juceRmlUi
{
	namespace
	{
		// Same parameter set the context generates for its own mouse events, so existing
		// listeners (helper::getMousePos, getMouseButton, getKeyMod*) work unchanged.
		Rml::Dictionary makeParameters(const int _touchId, const Rml::Vector2i _position,
			const int _keyModifiers, const bool _cancel)
		{
			static const char* const modifierNames[] = { "ctrl_key", "shift_key", "alt_key", "meta_key",
				"caps_lock_key", "num_lock_key", "scroll_lock_key" };

			Rml::Dictionary parameters;
			parameters["mouse_x"] = _position.x;
			parameters["mouse_y"] = _position.y;
			parameters["button"] = 0;
			for(int i = 0; i < 7; ++i)
				parameters[modifierNames[i]] = (_keyModifiers & (1 << i)) ? 1 : 0;
			parameters[TouchRouter::g_touchIdParameter] = _touchId;
			if(_cancel)
				parameters[TouchRouter::g_touchCancelParameter] = 1;
			return parameters;
		}
	}

	void TouchRouter::setTouchCapture(Rml::Element* _element, const bool _capture)
	{
		if(!_element)
			return;
		if(_capture)
			_element->SetAttribute(g_captureAttribute, true);
		else
			_element->RemoveAttribute(g_captureAttribute);
	}

	Rml::Element* TouchRouter::findCaptureRoot(Rml::Element* _hit)
	{
		for(auto* e = _hit; e; e = e->GetParentNode())
		{
			if(e->HasAttribute(g_captureAttribute))
				return e;
		}
		return nullptr;
	}

	int TouchRouter::getTouchId(const Rml::Event& _event)
	{
		return _event.GetParameter<int>(g_touchIdParameter, -1);
	}

	bool TouchRouter::isTouchCancel(const Rml::Event& _event)
	{
		return _event.GetParameter<int>(g_touchCancelParameter, 0) != 0;
	}

	TouchRouter::Route TouchRouter::down(Rml::Context& _context, const int _touchId,
		const Rml::Vector2i _position, const int _keyModifiers)
	{
		// A repeated down for a finger we still track means its up got lost. Finish the
		// old gesture first so the element does not stay held forever.
		if(isCaptured(_touchId))
			release(_touchId, _position, _keyModifiers, true);

		auto* hit = _context.GetElementAtPoint({ static_cast<float>(_position.x), static_cast<float>(_position.y) });
		auto* root = findCaptureRoot(hit);

		if(!root)
		{
			if(m_contextFinger && *m_contextFinger != _touchId)
				return Route::Ignored;
			m_contextFinger = _touchId;
			return Route::Context;
		}

		Finger finger;
		finger.target = hit->GetObserverPtr(hit->GetCoreInstance());
		finger.root = root->GetObserverPtr(root->GetCoreInstance());
		finger.position = _position;
		finger.drags = !rootHasOtherFinger(root, _touchId);
		m_fingers[_touchId] = finger;

		if(finger.drags)
		{
			auto parameters = makeParameters(_touchId, _position, _keyModifiers, false);
			hit->DispatchEvent(Rml::EventId::Mousedown, parameters);
		}
		return Route::Captured;
	}

	TouchRouter::Route TouchRouter::move(const int _touchId, const Rml::Vector2i _position, const int _keyModifiers)
	{
		if(isContextFinger(_touchId))
			return Route::Context;

		const auto it = m_fingers.find(_touchId);
		if(it == m_fingers.end())
			return Route::Ignored;

		auto& finger = it->second;
		if(finger.position == _position)
			return Route::Captured;
		finger.position = _position;

		// A second finger on the same control holds it but does not drag it: two
		// fingers feeding one knob would make it jump between their positions.
		if(!finger.drags)
			return Route::Captured;

		if(auto* target = finger.target.get())
		{
			auto parameters = makeParameters(_touchId, _position, _keyModifiers, false);
			target->DispatchEvent(Rml::EventId::Drag, parameters);
		}
		return Route::Captured;
	}

	TouchRouter::Route TouchRouter::up(const int _touchId, const Rml::Vector2i _position, const int _keyModifiers)
	{
		if(isContextFinger(_touchId))
		{
			m_contextFinger.reset();
			return Route::Context;
		}

		if(!isCaptured(_touchId))
			return Route::Ignored;

		release(_touchId, _position, _keyModifiers, false);
		return Route::Captured;
	}

	bool TouchRouter::cancelAll(const int _keyModifiers)
	{
		while(!m_fingers.empty())
		{
			const auto it = m_fingers.begin();
			release(it->first, it->second.position, _keyModifiers, true);
		}

		const auto hadContextFinger = m_contextFinger.has_value();
		m_contextFinger.reset();
		return hadContextFinger;
	}

	bool TouchRouter::rootHasOtherFinger(const Rml::Element* _root, const int _exceptTouchId) const
	{
		for(const auto& [id, finger] : m_fingers)
		{
			if(id != _exceptTouchId && finger.root.get() == _root)
				return true;
		}
		return false;
	}

	void TouchRouter::release(const int _touchId, const Rml::Vector2i _position,
		const int _keyModifiers, const bool _cancel)
	{
		const auto it = m_fingers.find(_touchId);
		if(it == m_fingers.end())
			return;

		const auto finger = it->second;
		m_fingers.erase(it);

		auto* root = finger.root.get();
		auto* target = finger.target.get();

		// The element went away while held (document reload, skin switch).
		if(!root || !target)
			return;

		// Another finger still holds this control: keep it held and send nothing. The
		// remaining fingers stay passive, since ElemKnob and the LCD gesture take their
		// drag origin from Mousedown only and would jump if a joiner started dragging.
		// Whichever finger lifts last sends the Mouseup.
		if(rootHasOtherFinger(root, _touchId))
			return;

		auto parameters = makeParameters(_touchId, _position, _keyModifiers, _cancel);
		target->DispatchEvent(Rml::EventId::Mouseup, parameters);

		if(_cancel)
			return;

		// Mirror RmlUi: a click needs the release to happen over the element that was
		// pressed. The element may have been removed by the Mouseup handler.
		if(auto* stillThere = finger.target.get(); stillThere && root == finger.root.get()
			&& root->IsPointWithinElement({ static_cast<float>(_position.x), static_cast<float>(_position.y) }))
		{
			auto clickParameters = makeParameters(_touchId, _position, _keyModifiers, false);
			stillThere->DispatchEvent(Rml::EventId::Click, clickParameters);
		}
	}
}
