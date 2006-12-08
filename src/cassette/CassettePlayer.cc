// $Id$

//TODO:
// - specify prefix for auto file name generation when recording (setting?)
// - append to existing wav files when recording (record command), but this is
//   basically a special case (pointer at the end) of:
// - (partly) overwrite an existing wav file from any given time index
// - seek in cassette images for the next and previous file (using empty space?)
// - (partly) overwrite existing wav files with new tape data (not very hi prio)
// - handle read-only cassette images (e.g.: CAS images or WAV files with a RO
//   flag): refuse to go to record mode when those are selected
// - CLEAN UP! It's a bit messy now.
// - smartly auto-set the position of tapes: if you insert an existing WAV
//   file, it will have the position at the start, assuming PLAY mode by
//   default.  When specifiying record mode at insert (somehow), it should be
//   at the back.
//   Alternatively, we could remember the index in tape images by storing the
//   index in some persistent data file with its SHA1 sum as it was as we last
//   saw it. When there are write actions to the tape, the hash has to be
//   recalculated and replaced in the data file. An optimization would be to
//   first simply check on the length of the file and fall back to SHA1 if that
//   results in multiple matches.

#include "CassettePlayer.hh"
#include "BooleanSetting.hh"
#include "Connector.hh"
#include "CassettePort.hh"
#include "MSXCommandController.hh"
#include "RecordedCommand.hh"
#include "GlobalSettings.hh"
#include "XMLElement.hh"
#include "FileContext.hh"
#include "WavImage.hh"
#include "CasImage.hh"
#include "MSXCliComm.hh"
#include "CommandException.hh"
#include "Scheduler.hh"
#include "MSXEventDistributor.hh"
#include "EventDistributor.hh"
#include "FileOperations.hh"
#include "WavWriter.hh"
#include "ThrottleManager.hh"
#include "TclObject.hh"
#include <algorithm>
#include <cassert>

using std::auto_ptr;
using std::string;
using std::vector;
using std::set;

namespace openmsx {

static const unsigned RECORD_FREQ = 44100;
static const double OUTPUT_AMP = 60.0;

class TapeCommand : public RecordedCommand
{
public:
	TapeCommand(MSXCommandController& msxCommandController,
	            MSXEventDistributor& msxEventDistributor,
	            Scheduler& scheduler,
	            CassettePlayer& cassettePlayer);
	virtual string execute(const vector<string>& tokens, const EmuTime& time);
	virtual string help(const vector<string>& tokens) const;
	virtual void tabCompletion(vector<string>& tokens) const;
private:
	CassettePlayer& cassettePlayer;
};


CassettePlayer::CassettePlayer(
		MSXCommandController& msxCommandController_,
		MSXMixer& mixer, Scheduler& scheduler_,
		MSXEventDistributor& msxEventDistributor,
		EventDistributor& eventDistributor_,
		MSXCliComm& cliComm_)
	: SoundDevice(mixer, getName(), getDescription())
	, state(STOP)
	, motor(false), motorControl(true)
	, tapeTime(EmuTime::zero)
	, recTime(EmuTime::zero)
	, prevTime(EmuTime::zero)
	, lastOutput(false)
	, sampcnt(0)
	, msxCommandController(msxCommandController_)
	, scheduler(scheduler_)
	, tapeCommand(new TapeCommand(msxCommandController, msxEventDistributor,
	                              scheduler, *this))
	, playTapeTime(EmuTime::zero)
	, cliComm(cliComm_)
	, eventDistributor(eventDistributor_)
	, loadingIndicator(new LoadingIndicator(
	       msxCommandController.getGlobalSettings().getThrottleManager()))
{
	autoRunSetting.reset(new BooleanSetting(msxCommandController,
		"autoruncassettes", "automatically try to run cassettes", false));
	removeTape(EmuTime::zero);

	static XMLElement cassettePlayerConfig("cassetteplayer");
	static bool init = false;
	if (!init) {
		init = true;
		auto_ptr<XMLElement> sound(new XMLElement("sound"));
		sound->addChild(auto_ptr<XMLElement>(new XMLElement("volume", "5000")));
		cassettePlayerConfig.addChild(sound);
	}
	registerSound(cassettePlayerConfig);
	eventDistributor.registerEventListener(OPENMSX_BOOT_EVENT, *this);
	cliComm.update(CliComm::HARDWARE, getName(), "add");
}

CassettePlayer::~CassettePlayer()
{
	unregisterSound();
	if (Connector* connector = getConnector()) {
		connector->unplug(scheduler.getCurrentTime());
	}
	eventDistributor.unregisterEventListener(OPENMSX_BOOT_EVENT, *this);
	cliComm.update(CliComm::HARDWARE, getName(), "remove");
}

void CassettePlayer::autoRun()
{
	// try to automatically run the tape, if that's set
	CassetteImage::FileType type = playImage->getFirstFileType();
	if (!autoRunSetting->getValue() || type == CassetteImage::UNKNOWN) {
		return;
	}
	string loadingInstruction;
	switch (type) {
		case CassetteImage::ASCII:
			loadingInstruction = "RUN\\\"CAS:\\\"";
			break;
		case CassetteImage::BINARY:
			loadingInstruction = "BLOAD\\\"CAS:\\\",R";
			break;
		case CassetteImage::BASIC:
			loadingInstruction = "CLOAD\\\\rRUN";
			break;
		default:
			assert(false); // Shouldn't be possible
	}
	string var = "::auto_run_cas_counter";
	string command =
		"if ![info exists " + var + "] { set " + var + " 0 }\n"
		"incr " + var + "\n"
		"after time 2 \"if $" + var + "==\\$" + var + " { "
		"type " + loadingInstruction + "\\\\r }\"";
	try {
		msxCommandController.executeCommand(command);
	} catch (CommandException& e) {
		cliComm.printWarning(
			"Error executing loading instruction for AutoRun: " +
			e.getMessage() + "\n Please report a bug.");
	}
}

CassettePlayer::State CassettePlayer::getState() const
{
	return state;
}

bool CassettePlayer::isRolling() const
{
	// TODO implement end-of-tape
	// Is the tape 'rolling'?
	// is true when:
	//  not in stop mode (there is a tape inserted and not at end-of-tape)
	//  AND [ user forced playing (motorcontrol=off) OR motor enabled by
	//        software (motor=on) ]
	return (getState() != STOP) && (motor || !motorControl);
}

string CassettePlayer::getStateString(State state)
{
	switch (state) {
		case PLAY:   return "play";
		case RECORD: return "record";
		case STOP:   return "stop";
	}
	assert(false);
	return "";
}

void CassettePlayer::checkInvariants() const
{
	switch (getState()) {
	case STOP:
		assert(isMuted());
		assert(!recordImage.get());
		break;
	case PLAY:
		assert(!getImageName().empty());
		assert(!recordImage.get());
		assert(playImage.get());
		break;
	case RECORD:
		assert(isMuted());
		assert(!getImageName().empty());
		assert(recordImage.get());
		assert(!playImage.get());
		break;
	default:
		assert(false);
	}
}

void CassettePlayer::setState(State newState, const EmuTime& time)
{
	if (getImageName().empty()) {
		// no image, always STOP state
		newState = STOP;
	}

	// set new state if different from old state
	State oldState = getState();
	if (oldState == newState) return;
	state = newState;

	// stuff for leaving the old state
	if (oldState == RECORD) {
		setSignal(lastOutput, time);
		flushOutput();
		recordImage.reset();
	}

	// stuff for entering the new state
	if (newState == RECORD) {
		partialOut = 0.0;
		partialInterval = 0.0;
		lastX = lastOutput ? OUTPUT_AMP : -OUTPUT_AMP;
		lastY = 0.0;
	}
	cliComm.update(CliComm::STATUS, "cassetteplayer",
	               getStateString(newState));

	updateLoadingState();

	checkInvariants();
}

void CassettePlayer::updateLoadingState()
{
	// TODO also set loadingIndicator for RECORD?
	// note: we don't use isRolling()
	loadingIndicator->update(motor && (getState() == PLAY)); 

	setMute((getState() != PLAY) || !isRolling());
}

void CassettePlayer::setImageName(const string& newImage)
{
	casImage = newImage;
	cliComm.update(CliComm::MEDIA, "cassetteplayer", casImage);
}

const string& CassettePlayer::getImageName() const
{
	return casImage;
}

void CassettePlayer::playTape(const string& filename, const EmuTime& time)
{
	try {
		// first try WAV
		playImage.reset(new WavImage(filename));
	} catch (MSXException& e) {
		// if that fails use CAS
		playImage.reset(new CasImage(filename, cliComm));
	}
	setImageName(filename);
	rewind(time); // sets PLAY mode
	autoRun();
}

void CassettePlayer::rewind(const EmuTime& time)
{
	tapeTime = EmuTime::zero;
	playTapeTime = EmuTime::zero;
	setState(PLAY, time);
}

void CassettePlayer::recordTape(const string& filename, const EmuTime& time)
{
	removeTape(time); // flush (possible) previous recording
	recordImage.reset(new WavWriter(filename, 1, 8, RECORD_FREQ));
	setImageName(filename);
	setState(RECORD, time);
	recTime = time;
}

void CassettePlayer::removeTape(const EmuTime& time)
{
	playImage.reset();
	setImageName("");
	setState(STOP, time);
}

void CassettePlayer::setMotor(bool status, const EmuTime& time)
{
	if (status != motor) {
		updateAll(time);
		motor = status;
		updateLoadingState();
	}
}

void CassettePlayer::setMotorControl(bool status, const EmuTime& time)
{
	if (status != motorControl) {
		updateAll(time);
		motorControl = status;
		updateLoadingState();
	}
}

short CassettePlayer::getSample(const EmuTime& time)
{
	assert(getState() == PLAY);
	return isRolling() ? playImage->getSampleAt(time) : 0;
}

short CassettePlayer::readSample(const EmuTime& time)
{
	if (getState() == PLAY) {
		// playing
		updatePlayPosition(time);
		return getSample(tapeTime);
	} else {
		// record or stop
		return 0;
	}
}

void CassettePlayer::updatePlayPosition(const EmuTime& time)
{
	assert(getState() == PLAY);
	if (isRolling()) {
		tapeTime += (time - prevTime);
		playTapeTime = tapeTime;
	}
	prevTime = time;
}

void CassettePlayer::updateAll(const EmuTime& time)
{
	switch (getState()) {
	case PLAY:
		updatePlayPosition(time);
		break;
	case RECORD:
		if (isRolling()) {
			// was already recording, update output
			setSignal(lastOutput, time);
		} else {
			// (possibly) restart recording, reset parameters
			recTime = time;
		}
		flushOutput();
		break;
	default:
		// nothing
		break;
	}
}

void CassettePlayer::setSignal(bool output, const EmuTime& time)
{
	if (recordImage.get() && isRolling()) {
		double out = output ? OUTPUT_AMP : -OUTPUT_AMP;
		double samples = (time - recTime).toDouble() * RECORD_FREQ;
		double rest = 1.0 - partialInterval;
		if (rest <= samples) {
			// enough to fill next interval
			partialOut += out * rest;
			fillBuf(1, (int)partialOut);
			samples -= rest;

			// fill complete intervals
			int count = (int)samples;
			if (count > 0) {
				fillBuf(count, (int)out);
			}
			samples -= count;

			// partial last interval
			partialOut = samples * out;
			partialInterval = 0.0;
		} else {
			partialOut += samples * out;
			partialInterval += samples;
		}
	}
	recTime = time;
	lastOutput = output;
}

void CassettePlayer::fillBuf(size_t length, double x)
{
	assert(recordImage.get());
	static const double A = 252.0 / 256.0;

	double y = lastY + (x - lastX);

	while (length) {
		int len = std::min(length, BUF_SIZE - sampcnt);
		for (int j = 0; j < len; ++j) {
			buf[sampcnt++] = (int)y + 128;
			y *= A;
		}
		length -= len;
		assert(sampcnt <= BUF_SIZE);
		if (BUF_SIZE == sampcnt) {
			flushOutput();
		}
	}
	lastY = y;
	lastX = x;
}

void CassettePlayer::flushOutput()
{
	recordImage->write8mono(buf, sampcnt);
	sampcnt = 0;
	recordImage->flush(); // update wav header
}


const string& CassettePlayer::getName() const
{
	static const string name("cassetteplayer");
	return name;
}

const string& CassettePlayer::getDescription() const
{
	// TODO: this description is not entirely accurate, but it is used
	// as an identifier for this device in e.g. Catapult. We should use
	// another way to identify devices A.S.A.P.!
	static const string desc(
		"Cassetteplayer, use to read .cas or .wav files.");
	return desc;
}

void CassettePlayer::plugHelper(Connector& connector, const EmuTime& time)
{
	lastOutput = static_cast<CassettePortInterface&>(connector).lastOut();
	updateAll(time);
}

void CassettePlayer::unplugHelper(const EmuTime& time)
{
	setState(STOP, time);
}


void CassettePlayer::setVolume(int newVolume)
{
	volume = newVolume;
}

void CassettePlayer::setSampleRate(int sampleRate)
{
	delta = EmuDuration(1.0 / sampleRate);
}

void CassettePlayer::updateBuffer(unsigned length, int* buffer,
     const EmuTime& /*time*/, const EmuDuration& /*sampDur*/)
{
	assert(getState() == PLAY);
	while (length--) {
		*(buffer++) = (((int)getSample(playTapeTime)) * volume) >> 15;
		playTapeTime += delta;
	}
}


bool CassettePlayer::signalEvent(shared_ptr<const Event> event)
{
	if (event->getType() == OPENMSX_BOOT_EVENT) {
		if (!getImageName().empty()) {
			// Reinsert tape to make sure everything is reset.
			try {
				playTape(getImageName(), scheduler.getCurrentTime());
			} catch (MSXException& e) {
				cliComm.printWarning(
					"Failed to insert tape: " + e.getMessage());
			}
		}
	}
	return true;
}


// class TapeCommand

TapeCommand::TapeCommand(MSXCommandController& msxCommandController,
                         MSXEventDistributor& msxEventDistributor,
                         Scheduler& scheduler,
                         CassettePlayer& cassettePlayer_)
	: RecordedCommand(msxCommandController, msxEventDistributor,
	                  scheduler, "cassetteplayer")
	, cassettePlayer(cassettePlayer_)
{
}

string TapeCommand::execute(const vector<string>& tokens, const EmuTime& time)
{
	string result;
	if (tokens.size() == 1) {
		// Returning TCL lists here, similar to the disk commands in
		// DiskChanger
		TclObject tmp(getCommandController().getInterpreter());
		tmp.addListElement(getName() + ':');
		tmp.addListElement(cassettePlayer.getImageName());

		TclObject options(getCommandController().getInterpreter());
		options.addListElement(cassettePlayer.getStateString(
		                           cassettePlayer.getState()));
		tmp.addListElement(options);
		result += tmp.getString();

	} else if (tokens[1] == "new") {
		string filename = (tokens.size() == 3)
		                ? tokens[2]
		                : FileOperations::getNextNumberedFileName(
		                         "taperecordings", "openmsx", ".wav");
		cassettePlayer.recordTape(filename, time);
		result += "Created new cassette image file: " + filename
		       + ", inserted it and set recording mode.";

	} else if (tokens[1] == "insert" && tokens.size() == 3) {
		try {
			result += "Changing tape";
			UserFileContext context(getCommandController());
			cassettePlayer.playTape(context.resolve(tokens[2]), time);
		} catch (MSXException& e) {
			throw CommandException(e.getMessage());
		}

	} else if (tokens[1] == "motorcontrol" && tokens.size() == 3) {
		if (tokens[2] == "on") {
			cassettePlayer.setMotorControl(true, time);
			result += "Motor control enabled.";
		} else if (tokens[2] == "off") {
			cassettePlayer.setMotorControl(false, time);
			result += "Motor control disabled.";
		} else {
			throw SyntaxError();
		}

	} else if (tokens.size() != 2) {
		throw SyntaxError();

	} else if (tokens[1] == "motorcontrol") {
		result += "Motor control is ";
		result += cassettePlayer.motorControl ? "on" : "off";

	} else if (tokens[1] == "record") {
			result += "TODO: implement this... (sorry)";

	} else if (tokens[1] == "play") {
		if (cassettePlayer.getState() == CassettePlayer::RECORD) {
			try {
				result += "Play mode set, rewinding tape.";
				cassettePlayer.playTape(
					cassettePlayer.getImageName(), time);
			} catch (MSXException& e) {
				throw CommandException(e.getMessage());
			}
		} else {
			// both PLAY and STOP
			result += "Already in play mode.";
		}

	} else if (tokens[1] == "eject") {
		result += "Tape ejected";
		cassettePlayer.removeTape(time);

	} else if (tokens[1] == "rewind") {
		if (cassettePlayer.getState() == CassettePlayer::RECORD) {
			try {
				result += "First stopping recording... ";
				cassettePlayer.playTape(
					cassettePlayer.getImageName(), time);
			} catch (MSXException& e) {
				throw CommandException(e.getMessage());
			}
		}
		cassettePlayer.rewind(time);
		result += "Tape rewound";

	} else {
		try {
			result += "Changing tape";
			UserFileContext context(getCommandController());
			cassettePlayer.playTape(context.resolve(tokens[1]), time);
		} catch (MSXException& e) {
			throw CommandException(e.getMessage());
		}
	}
	if (!cassettePlayer.getConnector()) {
		cassettePlayer.cliComm.printWarning("Cassetteplayer not plugged in.");
	}
	return result;
}

string TapeCommand::help(const vector<string>& tokens) const
{
	string helptext;
	if (tokens.size() >= 2) {
		if (tokens[1] == "eject") {
			helptext =
			    "Well, just eject the cassette from the cassette "
			    "player/recorder!";
		} else if (tokens[1] == "rewind") {
			helptext =
			    "Indeed, rewind the tape that is currently in the "
			    "cassette player/recorder...";
		} else if (tokens[1] == "motorcontrol") {
			helptext =
			    "Setting this to 'off' is equivalent to "
			    "disconnecting the black remote plug from the "
			    "cassette player: it makes the cassette player "
			    "run (if in play mode); the motor signal from the "
			    "MSX will be ignored. Normally this is set to "
			    "'on': the cassetteplayer obeys the motor control "
			    "signal from the MSX.";
		} else if (tokens[1] == "play") {
			helptext =
			    "Go to play mode. Only useful if you were in "
			    "record mode (which is currently the only other "
			    "mode available).";
		} else if (tokens[1] == "new") {
			helptext =
			    "Create a new cassette image. If the file name is "
			    "omitted, one will be generated in the default "
			    "directory for tape recordings. Implies going to "
			    "record mode (why else do you want a new cassette "
			    "image?).";
		} else if (tokens[1] == "insert") {
			helptext =
			    "Inserts the specified cassette image into the "
			    "cassette player, rewinds it and switches to play "
			    "mode.";
		} else if (tokens[1] == "record") {
			helptext =
			    "Go to record mode. NOT IMPLEMENTED YET. Will be "
			    "used to be able to resume recording to an "
			    "existing cassette image, previously inserted with "
			    "the insert command.";
		}
	} else {
		helptext =
		    "cassetteplayer eject             "
		    ": remove tape from virtual player\n"
		    "cassetteplayer rewind            "
		    ": rewind tape in virtual player\n"
		    "cassetteplayer motorcontrol      "
		    ": enables or disables motor control (remote)\n"
		    "cassetteplayer play              "
		    ": change to play mode (default)\n"
		    "cassetteplayer record            "
		    ": change to record mode (NOT IMPLEMENTED YET)\n"
		    "cassetteplayer new [<filename>]  "
		    ": create and insert new tape image file and go to record mode\n"
		    "cassetteplayer insert <filename> "
		    ": insert (a different) tape file\n"
		    "cassetteplayer <filename>        "
		    ": insert (a different) tape file\n";
	}
	return helptext;
}

void TapeCommand::tabCompletion(vector<string>& tokens) const
{
	if (tokens.size() == 2) {
		set<string> extra;
		extra.insert("eject");
		extra.insert("rewind");
		extra.insert("motorcontrol");
		extra.insert("insert");
		extra.insert("new");
		extra.insert("play");
	//	extra.insert("record");
		UserFileContext context(getCommandController());
		completeFileName(tokens, context, extra);
	} else if ((tokens.size() == 3) && (tokens[1] == "insert")) {
		UserFileContext context(getCommandController());
		completeFileName(tokens, context);
	} else if ((tokens.size() == 3) && (tokens[1] == "motorcontrol")) {
		set<string> extra;
		extra.insert("on");
		extra.insert("off");
		completeString(tokens, extra);
	}
}

} // namespace openmsx
