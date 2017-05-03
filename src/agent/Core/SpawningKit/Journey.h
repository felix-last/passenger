/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2017 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SPAWNING_KIT_HANDSHAKE_JOURNEY_H_
#define _PASSENGER_SPAWNING_KIT_HANDSHAKE_JOURNEY_H_

#include <map>
#include <utility>

#include <oxt/macros.hpp>
#include <oxt/backtrace.hpp>

#include <jsoncpp/json.h>

#include <Logging.h>
#include <StaticString.h>
#include <Utils/StrIntUtils.h>
#include <Utils/SystemTime.h>

namespace Passenger {
namespace SpawningKit {

using namespace std;


enum JourneyType {
	SPAWN_DIRECTLY,
	START_PRELOADER,
	SPAWN_THROUGH_PRELOADER
};

enum JourneyStep {
	// Steps in Passenger Core / SpawningKit
	SPAWNING_KIT_PREPARATION,
	SPAWNING_KIT_FORK_SUBPROCESS,
	SPAWNING_KIT_CONNECT_TO_PRELOADER,
	SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER,
	SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER,
	SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER,
	SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER,
	SPAWNING_KIT_HANDSHAKE_PERFORM,
	SPAWNING_KIT_FINISH,

	// Steps in preloader (when spawning a worker process)
	PRELOADER_PREPARATION,
	PRELOADER_FORK_SUBPROCESS,
	PRELOADER_SEND_RESPONSE,
	PRELOADER_FINISH,

	// Steps in subprocess
	SUBPROCESS_BEFORE_FIRST_EXEC,
	SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL,
	SUBPROCESS_OS_SHELL,
	SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL,
	SUBPROCESS_EXEC_WRAPPER,
	SUBPROCESS_WRAPPER_PREPARATION,
	SUBPROCESS_APP_LOAD_OR_EXEC,
	SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER,
	SUBPROCESS_LISTEN,
	SUBPROCESS_FINISH,

	// Other
	UNKNOWN_JOURNEY_STEP
};

enum JourneyStepState {
	/**
	 * This step has not started yet. Will be visualized with an empty
	 * placeholder.
	 */
	STEP_NOT_STARTED,

	/**
	 * This step is currently in progress. Will be visualized with a spinner.
	 */
	STEP_IN_PROGRESS,

	/**
	 * This step has already been performed successfully. Will be
	 * visualized with a green tick.
	 */
	STEP_PERFORMED,

	/**
	 * This step has failed. Will be visualized with a red mark.
	 */
	STEP_ERRORED,

	UNKNOWN_JOURNEY_STEP_STATE
};


inline OXT_PURE StaticString journeyTypeToString(JourneyType type);
inline OXT_PURE StaticString journeyStepToString(JourneyStep step);
inline OXT_PURE string journeyStepToStringLowerCase(JourneyStep step);
inline OXT_PURE StaticString journeyStepStateToString(JourneyStepState state);
inline OXT_PURE JourneyStepState stringToJourneyStepState(const StaticString &value);

inline OXT_PURE JourneyStep getFirstSubprocessJourneyStep() { return SUBPROCESS_BEFORE_FIRST_EXEC; }
inline OXT_PURE JourneyStep getLastSubprocessJourneyStep() { return SUBPROCESS_FINISH; }


struct JourneyStepInfo {
	JourneyStepState state;
	MonotonicTimeUsec startTime;
	MonotonicTimeUsec endTime;

	JourneyStepInfo(JourneyStepState _state = STEP_NOT_STARTED)
		: state(_state),
		  startTime(0),
		  endTime(0)
		{ }

	unsigned long long usecDuration() const {
		return endTime - startTime;
	}

	Json::Value inspectAsJson(JourneyStep step) const {
		Json::Value doc;
		doc["state"] = journeyStepStateToString(state).toString();
		doc["usec_duration"] = (Json::UInt64) usecDuration();
		return doc;
	}
};


class Journey {
public:
	typedef map<JourneyStep, JourneyStepInfo> Map;

private:
	JourneyType type;
	bool usingWrapper;
	Map steps;

	void insertStep(JourneyStep step) {
		steps.insert(make_pair(step, JourneyStepInfo()));
	}

	void fillInStepsForSpawnDirectlyJourney() {
		insertStep(SPAWNING_KIT_PREPARATION);
		insertStep(SPAWNING_KIT_FORK_SUBPROCESS);
		insertStep(SPAWNING_KIT_HANDSHAKE_PERFORM);
		insertStep(SPAWNING_KIT_FINISH);

		insertStep(SUBPROCESS_BEFORE_FIRST_EXEC);
		insertStep(SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL);
		insertStep(SUBPROCESS_OS_SHELL);
		insertStep(SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL);
		if (usingWrapper) {
			insertStep(SUBPROCESS_EXEC_WRAPPER);
			insertStep(SUBPROCESS_WRAPPER_PREPARATION);
		}
		insertStep(SUBPROCESS_APP_LOAD_OR_EXEC);
		insertStep(SUBPROCESS_LISTEN);
		insertStep(SUBPROCESS_FINISH);
	}

	void fillInStepsForPreloaderStartJourney() {
		insertStep(SPAWNING_KIT_PREPARATION);
		insertStep(SPAWNING_KIT_FORK_SUBPROCESS);
		insertStep(SPAWNING_KIT_HANDSHAKE_PERFORM);
		insertStep(SPAWNING_KIT_FINISH);

		insertStep(SUBPROCESS_BEFORE_FIRST_EXEC);
		insertStep(SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL);
		insertStep(SUBPROCESS_OS_SHELL);
		insertStep(SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL);
		if (usingWrapper) {
			insertStep(SUBPROCESS_EXEC_WRAPPER);
			insertStep(SUBPROCESS_WRAPPER_PREPARATION);
		}
		insertStep(SUBPROCESS_APP_LOAD_OR_EXEC);
		insertStep(SUBPROCESS_LISTEN);
		insertStep(SUBPROCESS_FINISH);
	}

	void fillInStepsForSpawnThroughPreloaderJourney() {
		insertStep(SPAWNING_KIT_PREPARATION);
		insertStep(SPAWNING_KIT_CONNECT_TO_PRELOADER);
		insertStep(SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER);
		insertStep(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER);
		insertStep(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER);
		insertStep(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);
		insertStep(SPAWNING_KIT_HANDSHAKE_PERFORM);
		insertStep(SPAWNING_KIT_FINISH);

		insertStep(PRELOADER_PREPARATION);
		insertStep(PRELOADER_FORK_SUBPROCESS);
		insertStep(PRELOADER_SEND_RESPONSE);
		insertStep(PRELOADER_FINISH);

		insertStep(SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER);
		insertStep(SUBPROCESS_LISTEN);
		insertStep(SUBPROCESS_FINISH);
	}

	JourneyStepInfo &getStepInfoMutable(JourneyStep step) {
		Map::iterator it = steps.find(step);
		if (it == steps.end()) {
			throw RuntimeException("Invalid step " + journeyStepToString(step));
		}

		return it->second;
	}

public:
	Journey(JourneyType _type, bool _usingWrapper)
		: type(_type),
		  usingWrapper(_usingWrapper)
	{
		switch (_type) {
		case SPAWN_DIRECTLY:
			fillInStepsForSpawnDirectlyJourney();
			break;
		case START_PRELOADER:
			fillInStepsForPreloaderStartJourney();
			break;
		case SPAWN_THROUGH_PRELOADER:
			fillInStepsForSpawnThroughPreloaderJourney();
			break;
		default:
			P_BUG("Unknown journey type " << toString((int) _type));
			break;
		}
	}

	JourneyType getType() const {
		return type;
	}

	bool hasStep(JourneyStep step) const {
		Map::const_iterator it = steps.find(step);
		return it != steps.end();
	}

	const JourneyStepInfo &getStepInfo(JourneyStep step) const {
		Map::const_iterator it = steps.find(step);
		if (it == steps.end()) {
			throw RuntimeException("Invalid step " + journeyStepToString(step));
		}

		return it->second;
	}

	JourneyStep getFirstFailedStep() const {
		Map::const_iterator it, end = steps.end();
		for (it = steps.begin(); it != end; it++) {
			if (it->second.state == STEP_ERRORED) {
				return it->first;
			}
		}

		return UNKNOWN_JOURNEY_STEP;
	}

	void setStepNotStarted(JourneyStep step, bool force = false) {
		JourneyStepInfo &info = getStepInfoMutable(step);
		if (info.state == STEP_NOT_STARTED || info.state == STEP_IN_PROGRESS || force) {
			info.state = STEP_NOT_STARTED;
			info.startTime = 0;
		} else {
			throw RuntimeException("Unable to change state for journey step "
				+ journeyStepToString(step) + " because it wasn't already in progress");
		}
	}

	void setStepInProgress(JourneyStep step, bool force = false) {
		JourneyStepInfo &info = getStepInfoMutable(step);
		if (info.state == STEP_IN_PROGRESS) {
			return;
		} else if (info.state == STEP_NOT_STARTED || force) {
			info.state = STEP_IN_PROGRESS;
			// When `force` is true, we don't want to overwrite the previous endTime.
			if (info.endTime == 0) {
				info.startTime =
					SystemTime::getMonotonicUsecWithGranularity<SystemTime::GRAN_10MSEC>();
			}
		} else {
			throw RuntimeException("Unable to change state for journey step "
				+ journeyStepToString(step)
				+ " because it was already in progress or completed");
		}
	}

	void setStepPerformed(JourneyStep step, bool force = false) {
		JourneyStepInfo &info = getStepInfoMutable(step);
		if (info.state == STEP_PERFORMED) {
			return;
		} else if (info.state == STEP_IN_PROGRESS || true) {
			info.state = STEP_PERFORMED;
			// When `force` is true, we don't want to overwrite the previous endTime.
			if (info.endTime == 0) {
				info.endTime =
					SystemTime::getMonotonicUsecWithGranularity<SystemTime::GRAN_10MSEC>();
			}
		} else {
			throw RuntimeException("Unable to change state for journey step "
				+ journeyStepToString(step) + " because it wasn't already in progress");
		}
	}

	void setStepErrored(JourneyStep step, bool force = false) {
		JourneyStepInfo &info = getStepInfoMutable(step);
		if (info.state == STEP_ERRORED) {
			return;
		} else if (info.state == STEP_IN_PROGRESS || force) {
			info.state = STEP_ERRORED;
			// When `force` is true, we don't want to overwrite the previous endTime.
			if (info.endTime == 0) {
				info.endTime =
					SystemTime::getMonotonicUsecWithGranularity<SystemTime::GRAN_10MSEC>();
			}
		} else {
			throw RuntimeException("Unable to change state for journey step "
				+ journeyStepToString(step) + " because it wasn't already in progress");
		}
	}

	void setStepExecutionDuration(JourneyStep step, unsigned long long usecDuration) {
		JourneyStepInfo &info = getStepInfoMutable(step);
		info.startTime = 0;
		info.endTime = usecDuration;
	}

	Json::Value inspectAsJson() const {
		Json::Value doc, steps;
		doc["type"] = journeyTypeToString(type).toString();

		Map::const_iterator it, end = this->steps.end();
		for (it = this->steps.begin(); it != end; it++) {
			const JourneyStep step = it->first;
			const JourneyStepInfo &info = it->second;
			steps[journeyStepToString(step).toString()] = info.inspectAsJson(step);
		}
		doc["steps"] = steps;

		return doc;
	}
};


inline OXT_PURE StaticString
journeyTypeToString(JourneyType type) {
	switch (type) {
	case SPAWN_DIRECTLY:
		return P_STATIC_STRING("SPAWN_DIRECTLY");
	case START_PRELOADER:
		return P_STATIC_STRING("START_PRELOADER");
	case SPAWN_THROUGH_PRELOADER:
		return P_STATIC_STRING("SPAWN_THROUGH_PRELOADER");
	default:
		return P_STATIC_STRING("UNKNOWN_JOURNEY_TYPE");
	}
}

inline OXT_PURE StaticString
journeyStepToString(JourneyStep step) {
	switch (step) {
	case SPAWNING_KIT_PREPARATION:
		return P_STATIC_STRING("SPAWNING_KIT_PREPARATION");
	case SPAWNING_KIT_FORK_SUBPROCESS:
		return P_STATIC_STRING("SPAWNING_KIT_FORK_SUBPROCESS");
	case SPAWNING_KIT_CONNECT_TO_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_CONNECT_TO_PRELOADER");
	case SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER");
	case SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER");
	case SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER");
	case SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER");
	case SPAWNING_KIT_HANDSHAKE_PERFORM:
		return P_STATIC_STRING("SPAWNING_KIT_HANDSHAKE_PERFORM");
	case SPAWNING_KIT_FINISH:
		return P_STATIC_STRING("SPAWNING_KIT_FINISH");

	case PRELOADER_PREPARATION:
		return P_STATIC_STRING("PRELOADER_PREPARATION");
	case PRELOADER_FORK_SUBPROCESS:
		return P_STATIC_STRING("PRELOADER_FORK_SUBPROCESS");
	case PRELOADER_SEND_RESPONSE:
		return P_STATIC_STRING("PRELOADER_SEND_RESPONSE");
	case PRELOADER_FINISH:
		return P_STATIC_STRING("PRELOADER_FINISH");

	case SUBPROCESS_BEFORE_FIRST_EXEC:
		return P_STATIC_STRING("SUBPROCESS_BEFORE_FIRST_EXEC");
	case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
		return P_STATIC_STRING("SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL");
	case SUBPROCESS_OS_SHELL:
		return P_STATIC_STRING("SUBPROCESS_OS_SHELL");
	case SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL:
		return P_STATIC_STRING("SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL");
	case SUBPROCESS_EXEC_WRAPPER:
		return P_STATIC_STRING("SUBPROCESS_EXEC_WRAPPER");
	case SUBPROCESS_WRAPPER_PREPARATION:
		return P_STATIC_STRING("SUBPROCESS_WRAPPER_PREPARATION");
	case SUBPROCESS_APP_LOAD_OR_EXEC:
		return P_STATIC_STRING("SUBPROCESS_APP_LOAD_OR_EXEC");
	case SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER:
		return P_STATIC_STRING("SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER");
	case SUBPROCESS_LISTEN:
		return P_STATIC_STRING("SUBPROCESS_LISTEN");
	case SUBPROCESS_FINISH:
		return P_STATIC_STRING("SUBPROCESS_FINISH");

	default:
		return P_STATIC_STRING("UNKNOWN_JOURNEY_STEP");
	}
}

inline OXT_PURE string
journeyStepToStringLowerCase(JourneyStep step) {
	StaticString stepString = journeyStepToString(step);
	DynamicBuffer stepStringLcBuffer(stepString.size());
	convertLowerCase((const unsigned char *) stepString.data(),
		(unsigned char *) stepStringLcBuffer.data, stepString.size());
	return string(stepStringLcBuffer.data, stepString.size());
}

inline OXT_PURE StaticString
journeyStepStateToString(JourneyStepState state) {
	switch (state) {
	case STEP_NOT_STARTED:
		return P_STATIC_STRING("STEP_NOT_STARTED");
	case STEP_IN_PROGRESS:
		return P_STATIC_STRING("STEP_IN_PROGRESS");
	case STEP_PERFORMED:
		return P_STATIC_STRING("STEP_PERFORMED");
	case STEP_ERRORED:
		return P_STATIC_STRING("STEP_ERRORED");
	default:
		return P_STATIC_STRING("UNKNOWN_JOURNEY_STEP_STATE");
	}
}

inline OXT_PURE JourneyStepState
stringToJourneyStepState(const StaticString &value) {
	if (value == P_STATIC_STRING("STEP_NOT_STARTED")) {
		return STEP_NOT_STARTED;
	} else if (value == P_STATIC_STRING("STEP_IN_PROGRESS")) {
		return STEP_IN_PROGRESS;
	} else if (value == P_STATIC_STRING("STEP_PERFORMED")) {
		return STEP_PERFORMED;
	} else if (value == P_STATIC_STRING("STEP_ERRORED")) {
		return STEP_ERRORED;
	} else {
		return UNKNOWN_JOURNEY_STEP_STATE;
	}
}


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_HANDSHAKE_JOURNEY_H_ */
