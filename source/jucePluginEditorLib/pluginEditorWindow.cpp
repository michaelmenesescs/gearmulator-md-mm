#include "pluginEditorWindow.h"

#include "pluginEditor.h"
#include "pluginEditorState.h"

#include "dsp56kBase/logging.h"

#include "juceRmlPlugin/rmlParameterBinding.h"

#include "juceRmlUi/juceRmlComponent.h"

#include "RmlUi/Core/Elements/ElementFormControlInput.h"

namespace jucePluginEditorLib
{

//==============================================================================
EditorWindow::EditorWindow(juce::AudioProcessor& _p, PluginEditorState& _s, juce::PropertiesFile& _config)
	: AudioProcessorEditor(&_p), m_state(_s), m_config(_config)
	, m_skinLoadedListener(m_state.evSkinLoaded, [this](juce::Component* _component)
		{
			setUiRoot(_component);
		})
	, m_guiScaleListener(m_state.evSetGuiScale, [this](const int _scale)
		{
			if(getNumChildComponents() > 0)
				setGuiScale(static_cast<float>(_scale));
		})
{
	addMouseListener(this, true);

	setUiRoot(m_state.getUiRoot());
}

EditorWindow::~EditorWindow()
{
	setUiRoot(nullptr);
}

void EditorWindow::resized()
{
	AudioProcessorEditor::resized();

	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	const auto w = getWidth();
	const auto h = getHeight();

	if (!m_state.resizeEditor(w,h))
		return;

#if !JUCE_IOS
	// On iOS there is no interactive window-edge drag that bypasses setGuiScale(), so this
	// component is only ever resized by setGuiScale() itself (which persists when asked to)
	// or by applyIosFitScale()'s device-derived, intentionally-not-persisted fit. Persisting
	// here unconditionally would overwrite the user's saved scale with that device-derived
	// value every time the parent bounds change (e.g. on rotation).
	if(m_scaleRestore.shouldPersistResize())
	{
		const auto scaleX = static_cast<float>(w) / static_cast<float>(m_state.getWidth());
		const auto scaleY = static_cast<float>(h) / static_cast<float>(m_state.getHeight());
		const auto scale = std::min(scaleX, scaleY);

		const auto percent = 100.f * scale / m_state.getRootScale();
		m_config.setValue("scale", percent);
		m_config.saveIfNeeded();
	}
#endif

	// Prettymuch unbelievable Juce VST3 bug, but our root component is a child of the VST3 editor component
	// and that one is not resized! The host window is, the first child (our editor component) is, but the
	// root component is not! This is no drama as long as you do not have a juce OpenGL context, because
	// that one uses the "top level component" to set the clipping rectangle! W T F
	startTimer(m_scaleRestore.delayedRestorePending() ? 50 : 1);
}

int EditorWindow::getControlParameterIndex(Component& _component)
{
	// This code relies on the fact that getComponentAt() is called with a XY position
	// first and then the parameter is queried for that returned component afterwards.
	// As we do not have Juce components, we remember the last Rml element that was
	// under the mouse and query the parameter binding for that element here.
	// It would be better if there was a function like "getParameterForPosition" but unfortunately
	// Juce does not provide that.
	if (const auto* editor = m_state.getEditor())
	{
		if (const auto* comp = editor->getRmlComponent())
		{
			if (const auto* binding = editor->getRmlParameterBinding())
			{
				if (const auto* elem = comp->getLastElementByGetComponentAt())
				{
					if (const auto* param = binding->getParameterForElement(elem))
						return param->getParameterIndex();

					if (const auto* parent = elem->GetParentNode())
					{
						if (dynamic_cast<const Rml::ElementFormControlInput*>(parent))
						{
							if (const auto* param = binding->getParameterForElement(parent))
								return param->getParameterIndex();
						}
					}
				}
			}
		}
	}

	return AudioProcessorEditor::getControlParameterIndex(_component);
}

void EditorWindow::setEmbedded(const bool _embedded)
{
	m_scaleRestore.setEmbedded(_embedded);
	if(m_scaleRestore.isEmbedded())
	{
		stopTimer();
		setResizable(false, false);
		setConstrainer(nullptr);
	}
}

void EditorWindow::setGuiScale(const float _percent, const bool _persist/* = true*/)
{
	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	const auto s = _percent / 100.0f * m_state.getRootScale();

	const auto w = static_cast<int>(static_cast<float>(m_state.getWidth()) * s);
	const auto h = static_cast<int>(static_cast<float>(m_state.getHeight()) * s);

	setSize(w, h);

	if(_persist)
	{
		m_config.setValue("scale", _percent);
		m_config.saveIfNeeded();
	}
}

void EditorWindow::setUiRoot(juce::Component* _component)
{
	removeAllChildren();
	setConstrainer(nullptr);

	if(!_component)
		return;

	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	m_sizeConstrainer.setMinimumSize(m_state.getWidth() / 10, m_state.getHeight() / 10);
	m_sizeConstrainer.setMaximumSize(m_state.getWidth() * 4, m_state.getHeight() * 4);

	m_sizeConstrainer.setFixedAspectRatio(static_cast<double>(m_state.getWidth()) / static_cast<double>(m_state.getHeight()));
	
	const auto configuredScale = static_cast<float>(m_config.getDoubleValue("scale", 100));
	const auto attachAction = m_scaleRestore.attachRoot(
		juce::JUCEApplicationBase::isStandaloneApp(), configuredScale);
	if(attachAction.applyConfiguredScale)
	{
#if JUCE_IOS
		// The configured/persisted scale is meaningless on a fixed-size iOS screen;
		// fit the skin to the actual available parent bounds instead.
		applyIosFitScale();
#else
		setGuiScale(configuredScale);
#endif
	}

	_component->setSize(getWidth(), getHeight());

	addAndMakeVisible(_component);

	if(m_scaleRestore.isEmbedded())
	{
		setResizable(false, false);
		setConstrainer(nullptr);
		stopTimer();
	}
	else
	{
		setResizable(true, true);
		setConstrainer(&m_sizeConstrainer);
		startTimer(m_scaleRestore.delayedRestorePending() ? 50 : 1);
	}
}

void EditorWindow::timerCallback()
{
	if(m_scaleRestore.isEmbedded())
	{
		stopTimer();
		return;
	}

	float restoreScale = 100.0f;
	if(m_scaleRestore.consumeDelayedRestore(restoreScale))
	{
		// A standalone host can impose its small placeholder size after the editor
		// has loaded. Reapply the configured size once that native parent exists,
		// and do not persist the placeholder resizes as the user's GUI scale.
#if JUCE_IOS
		applyIosFitScale();
#else
		setGuiScale(restoreScale);
#endif
	}

	fixParentWindowSize();
	stopTimer();
}

void EditorWindow::fixParentWindowSize() const
{
	const auto w = getWidth();
	const auto h = getHeight();

	auto* parent = getParentComponent();

	while (parent)
	{
		auto* grandParent = parent->getParentComponent();

#if JUCE_IOS
		// The outermost component is the physical screen: it is fixed and cannot be grown,
		// and applyIosFitScale() already shrinks the skin to fit it. Intermediate containers
		// still must be grown, otherwise they clip the editor down to their own size.
		if (!grandParent)
			break;
#endif

		if (parent->getWidth() < w || parent->getHeight() < h)
		{
			LOG("Parent " << parent->getName() << " has wrong size: " << parent->getName() <<
				", expected: " << w << "x" << h <<
				", actual: " << parent->getWidth() << "x" << parent->getHeight());
			parent->setSize(w, h);
		}

		parent = grandParent;
	}
}

#if JUCE_IOS
void EditorWindow::parentSizeChanged()
{
	AudioProcessorEditor::parentSizeChanged();
	applyIosFitScale();
}

float EditorWindow::computeIosFitScalePercent() const
{
	if(!m_state.getWidth() || !m_state.getHeight())
		return 100.0f;

	// Deliberately NOT getParentComponent(): the editor is attached inside intermediate
	// containers that have not been laid out yet and report sizes as small as 4x32, which
	// would collapse the skin to a couple of pixels. The screen is the only reliable
	// reference for how much room the skin actually has.
	const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
	if(!display)
		return 100.0f;

	// Keep the skin clear of notches, the status bar and the home indicator where possible.
	const auto usable = display->safeAreaInsets.subtractedFrom(display->userArea);

	const auto availableW = usable.getWidth();
	const auto availableH = usable.getHeight();

	if(availableW <= 0 || availableH <= 0)
		return 100.0f;

	const auto scaleX = static_cast<float>(availableW) / static_cast<float>(m_state.getWidth());
	const auto scaleY = static_cast<float>(availableH) / static_cast<float>(m_state.getHeight());
	const auto fitScale = std::min(scaleX, scaleY);

	return fitScale / m_state.getRootScale() * 100.0f;
}

void EditorWindow::applyIosFitScale()
{
	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	// setGuiScale() triggers resized(), which can re-enter through parentSizeChanged().
	// Without this guard the scale oscillates instead of settling on a value.
	if(m_applyingIosFitScale)
		return;

	const juce::ScopedValueSetter<bool> guard(m_applyingIosFitScale, true);

	// This scale is derived from the physical screen, not chosen by the user, so it must
	// never be written to m_config as if it were the persisted GUI scale preference.
	const auto percent = computeIosFitScalePercent();
	setGuiScale(percent, false);

	// Intermediate containers are laid out smaller than the editor and would clip it.
	fixParentWindowSize();


	// setSize() keeps the top-left corner fixed, so letterboxing would otherwise all end up
	// on the bottom/right. Only centre once the parent is actually big enough to centre in.
	if(const auto* parent = getParentComponent())
	{
		if(parent->getWidth() >= getWidth() && parent->getHeight() >= getHeight())
			setBounds(getBounds().withCentre(parent->getLocalBounds().getCentre()));
	}
}
#endif
}
