/*  ===========================================================================
*
*   This file is part of HISE.
*   Copyright 2016 Christoph Hart
*
*   HISE is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   HISE is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with HISE.  If not, see <http://www.gnu.org/licenses/>.
*
*   Commercial licenses for using HISE in an closed source project are
*   available on request. Please visit the project's website to get more
*   information about commercial licensing:
*
*   http://www.hise.audio/
*
*   HISE is based on the JUCE library,
*   which must be separately licensed for closed source applications:
*
*   http://www.juce.com
*
*   ===========================================================================
*/

namespace hise { using namespace juce;


MidiControllerAutomationHandler::MidiControllerAutomationHandler(MainController *mc_) :
anyUsed(false),
mpeData(mc_),
mc(mc_)
{
	tempBuffer.ensureSize(2048);

	clear();
}

void MidiControllerAutomationHandler::addMidiControlledParameter(Processor *interfaceProcessor, int attributeIndex, NormalisableRange<double> parameterRange, int macroIndex)
{
	ScopedLock sl(mc->getLock());

	unlearnedData.processor = interfaceProcessor;
	unlearnedData.attribute = attributeIndex;
	unlearnedData.parameterRange = parameterRange;
	unlearnedData.fullRange = parameterRange;
	unlearnedData.macroIndex = macroIndex;
	unlearnedData.used = true;

}

bool MidiControllerAutomationHandler::isLearningActive() const
{
	return unlearnedData.used;
}

bool MidiControllerAutomationHandler::isLearningActive(Processor *interfaceProcessor, int attributeIndex) const
{
	return unlearnedData.processor == interfaceProcessor && unlearnedData.attribute == attributeIndex;
}

void MidiControllerAutomationHandler::deactivateMidiLearning()
{
	ScopedLock sl(mc->getLock());

	unlearnedData = AutomationData();
}

void MidiControllerAutomationHandler::setUnlearndedMidiControlNumber(int ccNumber, NotificationType notifyListeners)
{
	jassert(isLearningActive());

	ScopedLock sl(mc->getLock());

	unlearnedData.ccNumber = ccNumber;

	automationData[ccNumber].addIfNotAlreadyThere(unlearnedData);
	unlearnedData = AutomationData();

	anyUsed = true;

	if (notifyListeners)
		sendChangeMessage();
}

int MidiControllerAutomationHandler::getMidiControllerNumber(Processor *interfaceProcessor, int attributeIndex) const
{
	for (int i = 0; i < 128; i++)
	{
		for (auto& a : automationData[i])
		{
			if (a.processor == interfaceProcessor && a.attribute == attributeIndex)
			{
				return i;
			}
		}
	}

	return -1;
}

void MidiControllerAutomationHandler::refreshAnyUsedState()
{
	AudioThreadGuard::Suspender suspender;
	LockHelpers::SafeLock sl(mc, LockHelpers::AudioLock);

	ignoreUnused(suspender);

	anyUsed = false;

	for (int i = 0; i < 128; i++)
	{
		for (auto& a : automationData[i])
		{
			if (a.used)
			{
				anyUsed = true;
				return;
			}
		}
	}
}

void MidiControllerAutomationHandler::clear()
{
	for (int i = 0; i < 128; i++)
	{
		automationData[i].clearQuick();
	};

	unlearnedData = AutomationData();

	anyUsed = false;
}

void MidiControllerAutomationHandler::removeMidiControlledParameter(Processor *interfaceProcessor, int attributeIndex, NotificationType notifyListeners)
{
	{
		AudioThreadGuard audioGuard(&(mc->getKillStateHandler()));
		LockHelpers::SafeLock sl(mc, LockHelpers::AudioLock);

		for (int i = 0; i < 128; i++)
		{
			for (auto& a : automationData[i])
			{
				if (a.processor == interfaceProcessor && a.attribute == attributeIndex)
				{
					automationData[i].removeAllInstancesOf(a);
					break;
				}
			}
		}
	}

	refreshAnyUsedState();

	if (notifyListeners == sendNotification)
		sendChangeMessage();
}

MidiControllerAutomationHandler::AutomationData::AutomationData() :
processor(nullptr),
attribute(-1),
parameterRange(NormalisableRange<double>()),
fullRange(NormalisableRange<double>()),
macroIndex(-1),
used(false),
inverted(false)
{

}



void MidiControllerAutomationHandler::AutomationData::clear()
{
	processor = nullptr;
	attribute = -1;
	parameterRange = NormalisableRange<double>();
	fullRange = NormalisableRange<double>();
	macroIndex = -1;
	ccNumber = -1;
	inverted = false;
	used = false;
}



bool MidiControllerAutomationHandler::AutomationData::operator==(const AutomationData& other) const
{
	return other.processor == processor && other.attribute == attribute;
}

void MidiControllerAutomationHandler::AutomationData::restoreFromValueTree(const ValueTree &v)
{
	ccNumber = v.getProperty("Controller", 1);;
	processor = ProcessorHelpers::getFirstProcessorWithName(mc->getMainSynthChain(), v.getProperty("Processor"));
	macroIndex = v.getProperty("MacroIndex");

	auto attributeString = v.getProperty("Attribute", attribute).toString();

	const bool isParameterId = attributeString.containsAnyOf("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");

	// The parameter was stored correctly as ID
	if (isParameterId && processor.get() != nullptr)
	{
		const Identifier pId(attributeString);

		for (int j = 0; j < processor->getNumParameters(); j++)
		{
			if (processor->getIdentifierForParameterIndex(j) == pId)
			{
				attribute = j;
				break;
			}
		}
	}
	else
	{
		// This tries to obtain the correct id.
		auto presetVersion = v.getRoot().getProperty("Version").toString();

		const Identifier pId = UserPresetHelpers::getAutomationIndexFromOldVersion(presetVersion, attributeString.getIntValue());

		if (pId.isNull())
		{
			attribute = attributeString.getIntValue();
		}
		else
		{
			for (int j = 0; j < processor->getNumParameters(); j++)
			{
				if (processor->getIdentifierForParameterIndex(j) == pId)
				{
					attribute = j;
					break;
				}
			}
		}
	}

	double start = v.getProperty("Start");
	double end = v.getProperty("End");
	double skew = v.getProperty("Skew", parameterRange.skew);
	double interval = v.getProperty("Interval", parameterRange.interval);

	auto fullStart = v.getProperty("FullStart", start);
	auto fullEnd = v.getProperty("FullEnd", end);

	parameterRange = NormalisableRange<double>(start, end, interval, skew);
	fullRange = NormalisableRange<double>(fullStart, fullEnd, interval, skew);

	used = true;
	inverted = v.getProperty("Inverted", false);
}

juce::ValueTree MidiControllerAutomationHandler::AutomationData::exportAsValueTree() const
{
	ValueTree cc("Controller");

	cc.setProperty("Controller", ccNumber, nullptr);
	cc.setProperty("Processor", processor->getId(), nullptr);
	cc.setProperty("MacroIndex", macroIndex, nullptr);
	cc.setProperty("Start", parameterRange.start, nullptr);
	cc.setProperty("End", parameterRange.end, nullptr);
	cc.setProperty("FullStart", fullRange.start, nullptr);
	cc.setProperty("FullEnd", fullRange.end, nullptr);
	cc.setProperty("Skew", parameterRange.skew, nullptr);
	cc.setProperty("Interval", parameterRange.interval, nullptr);
	cc.setProperty("Attribute", processor->getIdentifierForParameterIndex(attribute).toString(), nullptr);
	cc.setProperty("Inverted", inverted, nullptr);

	return cc;
}


struct MidiControllerAutomationHandler::MPEData::Data: public Processor::DeleteListener
{
	Data(MPEData& parent_) :
		Processor::DeleteListener(),
		parent(parent_)
	{};

	void add(MPEModulator* m)
	{
		m->addDeleteListener(this);
		connections.addIfNotAlreadyThere(m);
	}

	void remove(MPEModulator* m)
	{
		m->removeDeleteListener(this);
		connections.removeAllInstancesOf(m);
	}

	void processorDeleted(Processor* deletedProcessor) override
	{
		if (auto m = dynamic_cast<MPEModulator*>(deletedProcessor))
		{
			connections.removeAllInstancesOf(m);

            parent.sendAsyncNotificationMessage(m, EventType::MPEModConnectionRemoved);
		}
		else
			jassertfalse;
	}

	void clear()
	{
		for (auto c : connections)
		{
			if (c)
			{
				c->removeDeleteListener(this);
				c->setBypassed(true);
				c->sendChangeMessage();
			}
			else
				jassertfalse;
		}

		connections.clear();
	}

	void updateChildEditorList(bool /*forceUpdate*/) override {};

	MPEData& parent;
	Array<WeakReference<MPEModulator>> connections;
};

MidiControllerAutomationHandler::MPEData::MPEData(MainController* mc) :
	ControlledObject(mc),
	data(new Data(*this)),
	asyncRestorer(*this)
{

}


MidiControllerAutomationHandler::MPEData::~MPEData()
{
	jassert(listeners.size() == 0);
	data = nullptr;
}

void MidiControllerAutomationHandler::MPEData::AsyncRestorer::timerCallback()
{

}



void MidiControllerAutomationHandler::MPEData::restoreFromValueTree(const ValueTree &v)
{
	pendingData = v;

	auto f = [this](Processor* p)
	{
		LockHelpers::noMessageThreadBeyondInitialisation(p->getMainController());

		clear();

		static const Identifier id("ID");

		setMpeMode(pendingData.getProperty("Enabled", false));

		for (auto d : pendingData)
		{
			jassert(d.hasType("Processor"));

			d.setProperty("Type", "MPEModulator", nullptr);
			d.setProperty("Intensity", 1.0f, nullptr);

			ValueTree dummyChild("ChildProcessors");

			d.addChild(dummyChild, -1, nullptr);
			String id_ = d.getProperty(id).toString();

			if (auto mod = findMPEModulator(id_))
			{
				mod->restoreFromValueTree(d);
				addConnection(mod, dontSendNotification);
			}
		}

		

        sendAsyncNotificationMessage(nullptr, EventType::MPEDataReloaded);
        
        return SafeFunctionCall::OK;
	};

	getMainController()->getKillStateHandler().killVoicesAndCall(getMainController()->getMainSynthChain(), f, MainController::KillStateHandler::SampleLoadingThread);

	asyncRestorer.restore(v);
}

juce::ValueTree MidiControllerAutomationHandler::MPEData::exportAsValueTree() const
{
	ValueTree connectionData("MPEData");

	connectionData.setProperty("Enabled", mpeEnabled, nullptr);

	static const Identifier t("Type");
	static const Identifier i_("Intensity");
	

	for (auto mod : data->connections)
	{
		if (mod.get() != nullptr)
		{
			auto child = mod->exportAsValueTree();
			child.removeChild(0, nullptr);
			child.removeChild(0, nullptr);
			jassert(child.getNumChildren() == 0);
			child.removeProperty(t, nullptr);
			child.removeProperty(i_, nullptr);
			
			
			connectionData.addChild(child, -1, nullptr);
		}
	}

	return connectionData;
}

void MidiControllerAutomationHandler::MPEData::sendAsyncNotificationMessage(MPEModulator* mod, EventType type)
{
    WeakReference<MPEModulator> ref(mod);
    
    auto f = [ref, type](Dispatchable* obj)
    {
        if(ref.get() == nullptr)
            return Dispatchable::Status::OK;
        
        auto d = static_cast<MPEData*>(obj);
        
        jassert_message_thread;
        
        ScopedLock sl(d->listeners.getLock());
        
        for (auto l : d->listeners)
        {
            if (l == ref.get())
                continue;
            
            if (l)
            {
                switch(type)
                {
                    case EventType::MPEModConnectionAdded:   l->mpeModulatorAssigned(ref, true); break;
                    case EventType::MPEModConnectionRemoved: l->mpeModulatorAssigned(ref, false); break;
                    case EventType::MPEModeChanged:          l->mpeModeChanged(d->mpeEnabled); break;
                    case EventType::MPEDataReloaded:         l->mpeDataReloaded(); break;
                    default:                                 jassertfalse; break;
                }
            }
        }
        
        return Dispatchable::Status::OK;
    };
    
    getMainController()->getLockFreeDispatcher().callOnMessageThreadAfterSuspension(this, f);

}
    
void MidiControllerAutomationHandler::MPEData::addConnection(MPEModulator* mod, NotificationType notifyListeners/*=sendNotification*/)
{
    jassert(mod->isOnAir());
    
    jassert(LockHelpers::noMessageThreadBeyondInitialisation(mod->getMainController()));
    
	if (!data->connections.contains(mod))
	{
		data->add(mod);

        mod->mpeModulatorAssigned(mod, true);
        
		if (notifyListeners == sendNotification)
            sendAsyncNotificationMessage(mod, EventType::MPEModConnectionAdded);
	}
}

void MidiControllerAutomationHandler::MPEData::removeConnection(MPEModulator* mod, NotificationType notifyListeners/*=sendNotification*/)
{
    if(mod->isOnAir())
    {
        jassert(LockHelpers::noMessageThreadBeyondInitialisation(mod->getMainController()));
    }
    
	if (data->connections.contains(mod))
	{
		data->remove(mod);

        if(mod->isOnAir())
            mod->mpeModulatorAssigned(mod, false);

		if (notifyListeners == sendNotification)
            sendAsyncNotificationMessage(mod, EventType::MPEModConnectionRemoved);
	}
	else if (mod != nullptr)
	{
		sendAmountChangeMessage();
	}
}

MPEModulator* MidiControllerAutomationHandler::MPEData::getModulator(int index) const
{
	return data->connections[index].get();
}

MPEModulator* MidiControllerAutomationHandler::MPEData::findMPEModulator(const String& modName) const
{
	return dynamic_cast<MPEModulator*>(ProcessorHelpers::getFirstProcessorWithName(getMainController()->getMainSynthChain(), modName));
}

juce::StringArray MidiControllerAutomationHandler::MPEData::getListOfUnconnectedModulators(bool prettyName) const
{
	Processor::Iterator<MPEModulator> iter(getMainController()->getMainSynthChain(), false);

	StringArray sa;

	while (auto m = iter.getNextProcessor())
	{
		if (!data->connections.contains(m))
			sa.add(m->getId());
	}

	if (prettyName)
	{
		for (auto& s : sa)
		{
			s = getPrettyName(s);
		}
	}

	return sa;
}

juce::String MidiControllerAutomationHandler::MPEData::getPrettyName(const String& id)
{
	auto n = id.replace("MPE", "");
	String pretty;
	auto ptr = n.getCharPointer();
	bool lastWasUppercase = true;

	while (!ptr.isEmpty())
	{
		if (ptr.isUpperCase() && !lastWasUppercase)
			pretty << " ";

		lastWasUppercase = ptr.isUpperCase();
		pretty << ptr.getAddress()[0];
		ptr++;
	}

	return pretty;
}

void MidiControllerAutomationHandler::MPEData::clear()
{
	data->clear();

	Processor::Iterator<MPEModulator> iter(getMainController()->getMainSynthChain());

	while (auto m = iter.getNextProcessor())
	{
		m->resetToDefault();
	}
	
}

void MidiControllerAutomationHandler::MPEData::reset()
{
	clear();
	mpeEnabled = false;

    
    sendAsyncNotificationMessage(nullptr, EventType::MPEModeChanged);
}

int MidiControllerAutomationHandler::MPEData::size() const
{
	return data->connections.size();
}

void MidiControllerAutomationHandler::MPEData::setMpeMode(bool shouldBeOn)
{
	

	

	getMainController()->getKeyboardState().injectMessage(MidiMessage::controllerEvent(1, 74, 64));
	getMainController()->getKeyboardState().injectMessage(MidiMessage::pitchWheel(1, 8192));
	getMainController()->allNotesOff();

	mpeEnabled = shouldBeOn;

    // Do this synchronously
    ScopedLock sl(listeners.getLock());
    
	for (auto l : listeners)
	{
        if(l != nullptr)
            l->mpeModeChanged(mpeEnabled);
	}
}

bool MidiControllerAutomationHandler::MPEData::contains(MPEModulator* mod) const
{
	return data->connections.contains(mod);
}



ValueTree MidiControllerAutomationHandler::exportAsValueTree() const
{
	ValueTree v("MidiAutomation");

	for (int i = 0; i < 128; i++)
	{
		for (auto& a : automationData[i])
		{
			if (a.used && a.processor != nullptr)
			{
				auto cc = a.exportAsValueTree();
				v.addChild(cc, -1, nullptr);
			}
		}
	}

	return v;
}

void MidiControllerAutomationHandler::restoreFromValueTree(const ValueTree &v)
{
	if (v.getType() != Identifier("MidiAutomation")) return;

	clear();

	for (int i = 0; i < v.getNumChildren(); i++)
	{
		ValueTree cc = v.getChild(i);

		int controller = cc.getProperty("Controller", 1);

		auto& aArray = automationData[controller];

		AutomationData a;
		a.mc = mc;

		a.restoreFromValueTree(cc);

		aArray.addIfNotAlreadyThere(a);
	}

	sendChangeMessage();

	refreshAnyUsedState();
}

void MidiControllerAutomationHandler::handleParameterData(MidiBuffer &b)
{
	const bool bufferEmpty = b.isEmpty();
	const bool noCCsUsed = !anyUsed && !unlearnedData.used;

	if (bufferEmpty || noCCsUsed) return;

	tempBuffer.clear();

	MidiBuffer::Iterator mb(b);
	MidiMessage m;

	int samplePos;

	while (mb.getNextEvent(m, samplePos))
	{
		bool consumed = false;

		if (m.isController())
		{
			const int number = m.getControllerNumber();

			if (isLearningActive())
			{
				setUnlearndedMidiControlNumber(number, sendNotification);
			}

			for (auto& a : automationData[number])
			{
				if (a.used)
				{
					jassert(a.processor.get() != nullptr);

					auto normalizedValue = (double)m.getControllerValue() / 127.0;

					if (a.inverted) normalizedValue = 1.0 - normalizedValue;

					const double value = a.parameterRange.convertFrom0to1(normalizedValue);

					const float snappedValue = (float)a.parameterRange.snapToLegalValue(value);

					if (a.macroIndex != -1)
					{
						a.processor->getMainController()->getMacroManager().getMacroChain()->setMacroControl(a.macroIndex, (float)m.getControllerValue(), sendNotification);
					}
					else
					{
						if (a.lastValue != snappedValue)
						{
							a.processor->setAttribute(a.attribute, snappedValue, sendNotification);
							a.lastValue = snappedValue;
						}
					}

					consumed = true;
				}
			}
		}

		if (!consumed) tempBuffer.addEvent(m, samplePos);
	}

	b.clear();
	b.addEvents(tempBuffer, 0, -1, 0);
}


hise::MidiControllerAutomationHandler::AutomationData MidiControllerAutomationHandler::getDataFromIndex(int index) const
{
	int currentIndex = 0;

	for (int i = 0; i < 128; i++)
	{
		for (const auto& a: automationData[i])
		{
			if (index == currentIndex)
				return AutomationData(a);

			currentIndex++;
		}
	}

	return AutomationData();
}

int MidiControllerAutomationHandler::getNumActiveConnections() const
{
	int numActive = 0;

	for (int i = 0; i < 128; i++)
	{
		numActive += automationData[i].size();
	}

	return numActive;
}

bool MidiControllerAutomationHandler::setNewRangeForParameter(int index, NormalisableRange<double> range)
{
	int currentIndex = 0;

	for (int i = 0; i < 128; i++)
	{
		for (auto& a : automationData[i])
		{
			if (index == currentIndex)
			{
				a.parameterRange = range;
				return true;
			}
			
			currentIndex++;
		}
	}

	return false;
}

bool MidiControllerAutomationHandler::setParameterInverted(int index, bool value)
{
	int currentIndex = 0;

	for (int i = 0; i < 128; i++)
	{
		for (auto& a : automationData[i])
		{
			if (index == currentIndex)
			{
				a.inverted = value;
				return true;
			}

			currentIndex++;
		}
	}

	return false;
}

void ConsoleLogger::logMessage(const String &message)
{
	if (message.startsWith("!"))
	{
		debugError(processor, message.substring(1));
	}
	else
	{
		debugToConsole(processor, message);
	}

	
}

ControlledObject::ControlledObject(MainController *m) :
	controller(m) {
	jassert(m != nullptr);
};

ControlledObject::~ControlledObject()
{
	// Oops, this ControlledObject was not connected to a MainController
	jassert(controller != nullptr);

	masterReference.clear();
};

class DelayedRenderer::Pimpl
{
public:

	Pimpl() {}

	bool shouldDelayRendering() const 
	{
#if IS_STANDALONE_APP || IS_STANDALONE_FRONTEND
		return false;
#else
		return hostType.isFruityLoops();
#endif
	}

#if !(IS_STANDALONE_APP || IS_STANDALONE_FRONTEND)
	PluginHostType hostType;
#endif

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Pimpl)
};

DelayedRenderer::DelayedRenderer(MainController* mc_) :
	pimpl(new Pimpl()),
	mc(mc_)
{
}

DelayedRenderer::~DelayedRenderer()
{
	pimpl = nullptr;
}

bool DelayedRenderer::shouldDelayRendering() const
{
	return pimpl->shouldDelayRendering();
}

CircularAudioSampleBuffer::CircularAudioSampleBuffer(int numChannels_, int numSamples) :
	internalBuffer(numChannels_, numSamples),
	numChannels(numChannels_),
	size(numSamples)
{
	internalBuffer.clear();
	internalMidiBuffer.ensureSize(1024);
}

bool CircularAudioSampleBuffer::writeSamples(const AudioSampleBuffer& source, int offsetInSource, int numSamples)
{
	jassert(source.getNumChannels() == internalBuffer.getNumChannels());

	const bool needsWrapping = writeIndex + numSamples > size;

	if (needsWrapping)
	{
		const int numSamplesBeforeWrap = size - writeIndex;

		if (numSamplesBeforeWrap > 0)
		{
			for (int i = 0; i < numChannels; i++)
			{
				auto w = internalBuffer.getWritePointer(i, writeIndex);
				auto r = source.getReadPointer(i, offsetInSource);

				FloatVectorOperations::copy(w, r, numSamplesBeforeWrap);
			}
		}

		const int numSamplesAfterWrap = numSamples - numSamplesBeforeWrap;

		if (numSamplesAfterWrap > 0)
		{
			for (int i = 0; i < numChannels; i++)
			{
				auto w = internalBuffer.getWritePointer(i, 0);
				auto r = source.getReadPointer(i, offsetInSource + numSamplesBeforeWrap);

				FloatVectorOperations::copy(w, r, numSamplesAfterWrap);
			}

			
		}

		writeIndex = numSamplesAfterWrap;
	}
	else
	{
		for (int i = 0; i < numChannels; i++)
		{
			auto w = internalBuffer.getWritePointer(i, writeIndex);
			auto r = source.getReadPointer(i, offsetInSource);

			FloatVectorOperations::copy(w, r, numSamples);
		}

		writeIndex += numSamples;
	}

	numAvailable += numSamples;

	const bool ok = numAvailable <= size;
	jassert(ok);
	return ok;
}

bool CircularAudioSampleBuffer::readSamples(AudioSampleBuffer& destination, int offsetInDestination, int numSamples)
{
	jassert(destination.getNumChannels() == internalBuffer.getNumChannels());

	numAvailable -= numSamples;

	jassert(numAvailable >= 0);

	const bool needsWrapping = readIndex + numSamples > size;

	if (needsWrapping)
	{
		const int numSamplesBeforeWrap = size - readIndex;

		if (numSamplesBeforeWrap > 0)
		{
			for (int i = 0; i < numChannels; i++)
			{
				auto r = internalBuffer.getReadPointer(i, readIndex);
				auto w = destination.getWritePointer(i, offsetInDestination);

				FloatVectorOperations::copy(w, r, numSamplesBeforeWrap);
			}
		}

		const int numSamplesAfterWrap = numSamples - numSamplesBeforeWrap;

		if (numSamplesAfterWrap > 0)
		{
			for (int i = 0; i < numChannels; i++)
			{
				auto r = internalBuffer.getReadPointer(i, 0);
				auto w = destination.getWritePointer(i, offsetInDestination + numSamplesBeforeWrap);

				FloatVectorOperations::copy(w, r, numSamplesAfterWrap);
			}
		}

		readIndex = numSamplesAfterWrap;
	}
	else
	{
		for (int i = 0; i < numChannels; i++)
		{
			auto r = internalBuffer.getReadPointer(i, readIndex);
			auto w = destination.getWritePointer(i, offsetInDestination);

			FloatVectorOperations::copy(w, r, numSamples);
		}

		readIndex += numSamples;
	}

	const bool ok = numAvailable >= 0;
	jassert(ok);
	return ok;
}

bool CircularAudioSampleBuffer::writeMidiEvents(const MidiBuffer& source, int offsetInSource, int numSamples)
{
	const bool needsWrapping = midiWriteIndex + numSamples > size;

	if (source.isEmpty())
	{
		midiWriteIndex = (midiWriteIndex + numSamples) % size;
		return numAvailable <= size;
	}
	
	if (needsWrapping)
	{
		const int numSamplesBeforeWrap = size - midiWriteIndex;

		if (numSamplesBeforeWrap > 0)
		{
			internalMidiBuffer.clear(midiWriteIndex, numSamplesBeforeWrap);
			internalMidiBuffer.addEvents(source, offsetInSource, numSamplesBeforeWrap, midiWriteIndex);
		}

		const int numSamplesAfterWrap = numSamples - numSamplesBeforeWrap;
		const int offsetAfterWrap = offsetInSource + numSamplesBeforeWrap;

		if (numSamplesAfterWrap > 0)
		{
			internalMidiBuffer.clear(0, numSamplesAfterWrap);
			internalMidiBuffer.addEvents(source, offsetAfterWrap, numSamplesAfterWrap, -offsetAfterWrap);
		}

		midiWriteIndex = numSamplesAfterWrap;
	}
	else
	{
		internalMidiBuffer.clear(midiWriteIndex, numSamples);
		internalMidiBuffer.addEvents(source, offsetInSource, numSamples, midiWriteIndex);

		midiWriteIndex += numSamples;
	}

	const bool ok = numAvailable <= size;
	jassert(ok);
	return ok;
}

bool CircularAudioSampleBuffer::readMidiEvents(MidiBuffer& destination, int offsetInDestination, int numSamples)
{
	const bool needsWrapping = midiReadIndex + numSamples > size;

	jassert(destination.isEmpty());

	if (needsWrapping)
	{
		const int numSamplesBeforeWrap = size - midiReadIndex;
		const int numSamplesAfterWrap = numSamples - numSamplesBeforeWrap;
		const int offsetAfterWrap = offsetInDestination + numSamplesBeforeWrap;
		const int offsetBeforeWrap = offsetInDestination - midiReadIndex;

		if (numSamplesAfterWrap > 0)
		{
			destination.addEvents(internalMidiBuffer, 0, numSamplesAfterWrap, offsetAfterWrap);
			internalMidiBuffer.clear(0, numSamplesAfterWrap);
		}


		if (numSamplesBeforeWrap > 0)
		{
			destination.addEvents(internalMidiBuffer, midiReadIndex, numSamplesBeforeWrap, offsetBeforeWrap);
			internalMidiBuffer.clear(midiReadIndex, numSamplesBeforeWrap);
		}
		
		
		midiReadIndex = numSamplesAfterWrap;
	}
	else
	{
		destination.addEvents(internalMidiBuffer, midiReadIndex, numSamples, offsetInDestination - midiReadIndex);
		internalMidiBuffer.clear(midiReadIndex, numSamples);

		midiReadIndex += numSamples;
	}

	const bool ok = numAvailable >= 0;
	jassert(ok);
	return ok;
}

void DelayedRenderer::processWrapped(AudioSampleBuffer& buffer, MidiBuffer& midiMessages)
{
	if (shouldDelayRendering())
	{
	
		const bool ok = circularInputBuffer.writeSamples(buffer, 0, buffer.getNumSamples());

		jassert(ok);

		INSTRUMENT_ONLY(circularInputBuffer.writeMidiEvents(midiMessages, 0, buffer.getNumSamples()));
		INSTRUMENT_ONLY(buffer.clear());

		while (circularInputBuffer.getNumAvailableSamples() >= fullBlockSize)
		{
			delayedMidiBuffer.clear();

			circularInputBuffer.readSamples(processBuffer, 0, fullBlockSize);

			INSTRUMENT_ONLY(circularInputBuffer.readMidiEvents(delayedMidiBuffer, 0, fullBlockSize));
			
			mc->processBlockCommon(processBuffer, delayedMidiBuffer);

			circularOutputBuffer.writeSamples(processBuffer, 0, fullBlockSize);
		}

		circularOutputBuffer.readSamples(buffer, 0, buffer.getNumSamples());

#if 0
		const int thisNumSamples = buffer.getNumSamples();
		const int blockSize = writeBuffer->getNumSamples();
		
		//String s;
		//s << "thisNumSamples: " << thisNumSamples << ", sampleIndex: " << sampleIndex;
		//DBG(s);


		int numSamplesTodo = thisNumSamples;

		while (numSamplesTodo > 0)
		{
			jassert(sampleIndexInternal < blockSize);
			jassert(sampleIndexInternal >= 0);
			jassert(sampleIndexExternal < thisNumSamples);
			jassert(sampleIndexExternal >= 0);

			const bool wrapInternalBuffer = (sampleIndexInternal + numSamplesTodo) >= blockSize;

			if (wrapInternalBuffer)
			{
				

				const int numSamplesToCopyBeforeProcessing = jmin<int>(thisNumSamples - sampleIndexExternal, numSamplesTodo - sampleIndexInternal, blockSize - sampleIndexInternal);

				if (numSamplesToCopyBeforeProcessing > 0)
				{

#if FRONTEND_IS_PLUGIN
					FloatVectorOperations::copy(writeBuffer->getWritePointer(0, sampleIndexInternal), buffer.getReadPointer(0, sampleIndexExternal), numSamplesToCopyBeforeProcessing);
					FloatVectorOperations::copy(writeBuffer->getWritePointer(1, sampleIndexInternal), buffer.getReadPointer(1, sampleIndexExternal), numSamplesToCopyBeforeProcessing);
#else
					delayedMidiBuffer.addEvents(midiMessages, 0, numSamplesToCopyBeforeProcessing, sampleIndexInternal);
#endif

					FloatVectorOperations::copy(buffer.getWritePointer(0, sampleIndexExternal), readBuffer->getReadPointer(0, sampleIndexInternal), numSamplesToCopyBeforeProcessing);
					FloatVectorOperations::copy(buffer.getWritePointer(1, sampleIndexExternal), readBuffer->getReadPointer(1, sampleIndexInternal), numSamplesToCopyBeforeProcessing);
					
				}

				sampleIndexExternal += numSamplesToCopyBeforeProcessing;

				

				jassert(sampleIndexExternal + numSamplesToCopyBeforeProcessing == thisNumSamples);

				mc->processBlockCommon(*writeBuffer, delayedMidiBuffer);

				AudioSampleBuffer* temp = readBuffer;
				readBuffer = writeBuffer;
				writeBuffer = temp;
				delayedMidiBuffer.clear();
				sampleIndexInternal = 0;

				const int numSamplesToCopyAfterProcessing = jmin<int>(blockSize- indexInOutputBuffer, numSamplesTodo- indexInOutputBuffer);

				numSamplesTodo -= remainingSamples;

				if (numSamplesToCopyAfterProcessing > 0)
				{
					FloatVectorOperations::copy(buffer.getWritePointer(0, indexInOutputBuffer), readBuffer->getReadPointer(0, 0), numSamplesToCopyAfterProcessing);
					FloatVectorOperations::copy(buffer.getWritePointer(1, indexInOutputBuffer), readBuffer->getReadPointer(1, 0), numSamplesToCopyAfterProcessing);
					numSamplesTodo -= numSamplesToCopyAfterProcessing;
				}

				

				sampleIndexInternal = (sampleIndexInternal + numSamplesToCopyAfterProcessing) % blockSize;

				
			}
			else
			{

				int numToCopy = jmin<int>(blockSize - sampleIndexInternal, numSamplesTodo);

				jassert(numToCopy > 0);

#if FRONTEND_IS_PLUGIN
				FloatVectorOperations::copy(writeBuffer->getWritePointer(0, sampleIndexInternal), buffer.getReadPointer(0, 0), numToCopy);
				FloatVectorOperations::copy(writeBuffer->getWritePointer(1, sampleIndexInternal), buffer.getReadPointer(1, 0), numToCopy);
#else
				delayedMidiBuffer.addEvents(midiMessages, 0, numToCopy, sampleIndexInternal);
#endif

				FloatVectorOperations::copy(buffer.getWritePointer(0, 0), readBuffer->getReadPointer(0, sampleIndexInternal), numToCopy);
				FloatVectorOperations::copy(buffer.getWritePointer(1, 0), readBuffer->getReadPointer(1, sampleIndexInternal), numToCopy);
				numSamplesTodo -= numToCopy;

				sampleIndexInternal = (sampleIndexInternal + numToCopy) % blockSize;
			}

			

			}
#endif

	}
	else
	{
		mc->processBlockCommon(buffer, midiMessages);
	}
}

void DelayedRenderer::prepareToPlayWrapped(double sampleRate, int samplesPerBlock)
{
	if (shouldDelayRendering())
	{
		if (samplesPerBlock > lastBlockSize)
		{
			lastBlockSize = samplesPerBlock;

#if FRONTEND_IS_PLUGIN
			fullBlockSize = samplesPerBlock;
#else
			fullBlockSize = jmin<int>(256, samplesPerBlock);
#endif

			circularInputBuffer = CircularAudioSampleBuffer(2, 3 * samplesPerBlock);



			circularOutputBuffer = CircularAudioSampleBuffer(2, 3 * samplesPerBlock);

			circularOutputBuffer.setReadDelta(fullBlockSize);

			processBuffer.setSize(2, fullBlockSize);

			delayedMidiBuffer.ensureSize(1024);

			dynamic_cast<AudioProcessor*>(mc)->setLatencySamples(fullBlockSize);

			mc->prepareToPlay(sampleRate, fullBlockSize);
		}

	}
	else
	{
		mc->prepareToPlay(sampleRate, samplesPerBlock);
	}
}


void OverlayMessageBroadcaster::sendOverlayMessage(int newState, const String& newCustomMessage/*=String()*/)
{
	if (currentState == DeactiveOverlay::State::CriticalCustomErrorMessage)
		return;

#if USE_BACKEND

	ignoreUnused(newState);

	// Just print it on the console
	Logger::getCurrentLogger()->writeToLog("!" + newCustomMessage);
#else

	currentState = newState;
	customMessage = newCustomMessage;

	internalUpdater.triggerAsyncUpdate();
#endif
}

} // namespace hise
