/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2017 Phusion Holding B.V.
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
#ifndef _PASSENGER_SPAWNING_KIT_SMART_SPAWNER_H_
#define _PASSENGER_SPAWNING_KIT_SMART_SPAWNER_H_

#include <oxt/thread.hpp>
#include <oxt/system_calls.hpp>
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdio>
#include <cstring>
#include <cassert>

#include <adhoc_lve.h>

#include <Logging.h>
#include <Constants.h>
#include <Exceptions.h>
#include <DataStructures/StringKeyTable.h>
#include <Utils/SystemTime.h>
#include <Utils/IOUtils.h>
#include <Utils/BufferedIO.h>
#include <Utils/JsonUtils.h>
#include <Utils/ScopeGuard.h>
#include <Utils/ProcessMetricsCollector.h>
#include <LveLoggingDecorator.h>
#include <Core/SpawningKit/Spawner.h>
#include <Core/SpawningKit/Exceptions.h>
#include <Core/SpawningKit/PipeWatcher.h>
#include <Core/SpawningKit/Handshake/Session.h>
#include <Core/SpawningKit/Handshake/Prepare.h>
#include <Core/SpawningKit/Handshake/Perform.h>
#include <Core/SpawningKit/Handshake/BackgroundIOCapturer.h>

namespace Passenger {
namespace SpawningKit {

using namespace std;
using namespace boost;
using namespace oxt;


class SmartSpawner: public Spawner {
private:
	const vector<string> preloaderCommand;
	StringKeyTable<string> preloaderAnnotations;
	AppPoolOptions options;

	// Protects m_lastUsed and pid.
	mutable boost::mutex simpleFieldSyncher;
	// Protects everything else.
	mutable boost::mutex syncher;

	// Preloader information.
	pid_t pid;
	FileDescriptor preloaderStdin;
	string socketAddress;
	unsigned long long m_lastUsed;


	/**
	 * Behaves like <tt>waitpid(pid, status, WNOHANG)</tt>, but waits at most
	 * <em>timeout</em> miliseconds for the process to exit.
	 */
	static int timedWaitpid(pid_t pid, int *status, unsigned long long timeout) {
		Timer<SystemTime::GRAN_10MSEC> timer;
		int ret;

		do {
			ret = syscalls::waitpid(pid, status, WNOHANG);
			if (ret > 0 || ret == -1) {
				return ret;
			} else {
				syscalls::usleep(10000);
			}
		} while (timer.elapsed() < timeout);
		return 0; // timed out
	}

	static bool osProcessExists(pid_t pid) {
		if (syscalls::kill(pid, 0) == 0) {
			/* On some environments, e.g. Heroku, the init process does
			 * not properly reap adopted zombie processes, which can interfere
			 * with our process existance check. To work around this, we
			 * explicitly check whether or not the process has become a zombie.
			 */
			return !isZombie(pid);
		} else {
			return errno != ESRCH;
		}
	}

	static bool isZombie(pid_t pid) {
		string filename = "/proc/" + toString(pid) + "/status";
		FILE *f = fopen(filename.c_str(), "r");
		if (f == NULL) {
			// Don't know.
			return false;
		}

		bool result = false;
		while (!feof(f)) {
			char buf[512];
			const char *line;

			line = fgets(buf, sizeof(buf), f);
			if (line == NULL) {
				break;
			}
			if (strcmp(line, "State:	Z (zombie)\n") == 0) {
				// Is a zombie.
				result = true;
				break;
			}
		}
		fclose(f);
		return result;
	}

	void setConfigFromAppPoolOptions(Config *config, Json::Value &extraArgs,
		const AppPoolOptions &options)
	{
		Spawner::setConfigFromAppPoolOptions(config, extraArgs, options);
		config->spawnMethod = P_STATIC_STRING("smart");
	}

	bool preloaderStarted() const {
		return pid != -1;
	}

	void startPreloader() {
		TRACE_POINT();
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;
		assert(!preloaderStarted());
		P_DEBUG("Spawning new preloader: appRoot=" << options.appRoot);

		Config config;
		Json::Value extraArgs;
		try {
			setConfigFromAppPoolOptions(&config, extraArgs, options);
		} catch (const std::exception &originalException) {
			Journey journey(SPAWN_THROUGH_PRELOADER, true);
			journey.setStepErrored(SPAWNING_KIT_PREPARATION, true);
			throw SpawnException(originalException, journey,
				&config).finalize();
		}

		HandshakeSession session(*context, config, START_PRELOADER);
		session.journey.setStepInProgress(SPAWNING_KIT_PREPARATION);

		try {
			internalStartPreloader(config, session, extraArgs);
		} catch (const SpawnException &) {
			throw;
		} catch (const std::exception &originalException) {
			session.journey.setStepErrored(SPAWNING_KIT_PREPARATION);
			throw SpawnException(originalException, session.journey,
				&config).finalize();
		}
	}

	void internalStartPreloader(Config &config, HandshakeSession &session,
		const Json::Value &extraArgs)
	{
		TRACE_POINT();
		HandshakePrepare(session, extraArgs).execute();
		Pipe stdinChannel = createPipe(__FILE__, __LINE__);
		Pipe stdoutAndErrChannel = createPipe(__FILE__, __LINE__);
		adhoc_lve::LveEnter scopedLveEnter(LveLoggingDecorator::lveInitOnce(),
			session.uid,
			config.lveMinUid,
			LveLoggingDecorator::lveExitCallback);
		LveLoggingDecorator::logLveEnter(scopedLveEnter,
			session.uid,
			config.lveMinUid);
		string agentFilename = context->resourceLocator
			->findSupportBinary(AGENT_EXE);

		session.journey.setStepPerformed(SPAWNING_KIT_PREPARATION);
		session.journey.setStepInProgress(SPAWNING_KIT_FORK_SUBPROCESS);
		session.journey.setStepInProgress(SUBPROCESS_BEFORE_FIRST_EXEC);

		pid_t pid = syscalls::fork();
		if (pid == 0) {
			purgeStdio(stdout);
			purgeStdio(stderr);
			resetSignalHandlersAndMask();
			disableMallocDebugging();
			int stdinCopy = dup2(stdinChannel.first, 3);
			int stdoutAndErrCopy = dup2(stdoutAndErrChannel.second, 4);
			dup2(stdinCopy, 0);
			dup2(stdoutAndErrCopy, 1);
			dup2(stdoutAndErrCopy, 2);
			closeAllFileDescriptors(2);
			execlp(agentFilename.c_str(),
				agentFilename.c_str(),
				"spawn-env-setupper",
				session.workDir->getPath().c_str(),
				"--before",
				NULL);

			int e = errno;
			fprintf(stderr, "Cannot execute \"%s\": %s (errno=%d)\n",
				agentFilename.c_str(), strerror(e), e);
			fflush(stderr);
			_exit(1);

		} else if (pid == -1) {
			int e = errno;
			UPDATE_TRACE_POINT();
			session.journey.setStepErrored(SPAWNING_KIT_FORK_SUBPROCESS);
			SpawnException ex(OPERATING_SYSTEM_ERROR, session.journey, &config);
			ex.setSummary(StaticString("Cannot fork a new process: ") + strerror(e)
				+ " (errno=" + toString(e) + ")");
			ex.setAdvancedProblemDetails(StaticString("Cannot fork a new process: ")
				+ strerror(e) + " (errno=" + toString(e) + ")");
			throw ex.finalize();

		} else {
			UPDATE_TRACE_POINT();
			session.journey.setStepPerformed(SPAWNING_KIT_FORK_SUBPROCESS);
			session.journey.setStepInProgress(SPAWNING_KIT_HANDSHAKE_PERFORM);

			scopedLveEnter.exit();

			P_LOG_FILE_DESCRIPTOR_PURPOSE(stdinChannel.second,
				"Preloader " << pid << " (" << options.appRoot << ") stdin");
			P_LOG_FILE_DESCRIPTOR_PURPOSE(stdoutAndErrChannel.first,
				"Preloader " << pid << " (" << options.appRoot << ") stdoutAndErr");

			UPDATE_TRACE_POINT();
			ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, pid));
			P_DEBUG("Preloader process forked for appRoot=" << options.appRoot
				<< ": PID " << pid);
			stdinChannel.first.close();
			stdoutAndErrChannel.second.close();

			HandshakePerform(session, pid, stdinChannel.second,
				stdoutAndErrChannel.first).execute();
			string socketAddress = findPreloaderCommandSocketAddress(session);
			{
				boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
				this->pid = pid;
				this->socketAddress = socketAddress;
				this->preloaderStdin = stdinChannel.second;
				this->preloaderAnnotations = loadAnnotationsFromEnvDumpDir(
					session.envDumpDir);
			}

			PipeWatcherPtr watcher = boost::make_shared<PipeWatcher>(
				stdoutAndErrChannel.first, "output", pid);
			watcher->initialize();
			watcher->start();

			UPDATE_TRACE_POINT();
			guard.clear();
			session.journey.setStepPerformed(SPAWNING_KIT_HANDSHAKE_PERFORM);
			P_INFO("Preloader for " << options.appRoot <<
				" started on PID " << pid <<
				", listening on " << socketAddress);
		}
	}

	void stopPreloader() {
		TRACE_POINT();
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;

		if (!preloaderStarted()) {
			return;
		}

		safelyClose(preloaderStdin);
		if (timedWaitpid(pid, NULL, 5000) == 0) {
			P_DEBUG("Preloader did not exit in time, killing it...");
			syscalls::kill(pid, SIGKILL);
			syscalls::waitpid(pid, NULL, 0);
		}

		// Delete socket after the process has exited so that it
		// doesn't crash upon deleting a nonexistant file.
		if (getSocketAddressType(socketAddress) == SAT_UNIX) {
			string filename = parseUnixSocketAddress(socketAddress);
			syscalls::unlink(filename.c_str());
		}

		{
			boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
			pid = -1;
			socketAddress.clear();
			preloaderStdin.close(false);
			preloaderAnnotations.clear();
		}
	}

	FileDescriptor connectToPreloader(HandshakeSession &session) {
		TRACE_POINT();
		FileDescriptor fd(connectToServer(socketAddress, __FILE__, __LINE__), NULL, 0);
		P_LOG_FILE_DESCRIPTOR_PURPOSE(fd, "Preloader " << pid
			<< " (" << session.config->appRoot << ") connection");
		return fd;
	}

	struct ForkResult {
		pid_t pid;
		FileDescriptor stdinFd;
		FileDescriptor stdoutAndErrFd;

		ForkResult()
			: pid(-1)
			{ }

		ForkResult(pid_t _pid, const FileDescriptor &_stdinFd,
			const FileDescriptor &_stdoutAndErrFd)
			: pid(_pid),
			  stdinFd(_stdinFd),
			  stdoutAndErrFd(_stdoutAndErrFd)
			{ }
	};

	struct PreloaderCrashed {
		SystemException *systemException;
		IOException *ioException;

		PreloaderCrashed(const SystemException &e)
			: systemException(new SystemException(e)),
			  ioException(NULL)
			{ }

		PreloaderCrashed(const IOException &e)
			: systemException(NULL),
			  ioException(new IOException(e))
			{ }

		~PreloaderCrashed() {
			delete systemException;
			delete ioException;
		}

		const std::exception &getException() const {
			if (systemException != NULL) {
				return *systemException;
			} else {
				return *ioException;
			}
		}
	};

	ForkResult invokeForkCommand(HandshakeSession &session) {
		TRACE_POINT();
		try {
			return internalInvokeForkCommand(session);
		} catch (const PreloaderCrashed &crashException1) {
			P_WARN("An error occurred while spawning an application process: "
				<< crashException1.getException().what());
			P_WARN("The application preloader seems to have crashed,"
				" restarting it and trying again...");

			session.journey.setStepNotStarted(SPAWNING_KIT_CONNECT_TO_PRELOADER);
			session.journey.setStepNotStarted(SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER);
			session.journey.setStepNotStarted(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER);

			try {
				stopPreloader();
			} catch (const SpawnException &) {
				throw;
			} catch (const std::exception &originalException) {
				session.journey.setStepErrored(SPAWNING_KIT_PREPARATION);

				SpawnException e(originalException, session.journey, session.config);
				e.setSummary(StaticString("Error stopping a crashed preloader: ")
					+ originalException.what());
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However,"
					" this helper process crashed unexpectedly. "
					SHORT_PROGRAM_NAME " then tried to restart it, but"
					" encountered the following error while trying to"
					" stop the preloader:</p>"
					"<pre>" + escapeHTML(originalException.what()) + "</pre>");
				throw e.finalize();
			}

			startPreloader();

			try {
				return internalInvokeForkCommand(session);
			} catch (const PreloaderCrashed &crashException2) {
				try {
					stopPreloader();
				} catch (const SpawnException &) {
					throw;
				} catch (const std::exception &originalException) {
					session.journey.setStepErrored(SPAWNING_KIT_PREPARATION);
					session.journey.setStepNotStarted(SPAWNING_KIT_CONNECT_TO_PRELOADER);
					session.journey.setStepNotStarted(SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER);
					session.journey.setStepNotStarted(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER);

					SpawnException e(originalException, session.journey, session.config);
					e.setSummary(StaticString("Error stopping a crashed preloader: ")
						+ originalException.what());
					e.setProblemDescriptionHTML(
						"<p>The " PROGRAM_NAME " application server tried"
						" to start the web application by communicating with a"
						" helper process that we call a \"preloader\". However,"
						" this helper process crashed unexpectedly. "
						SHORT_PROGRAM_NAME " then tried to restart it, but"
						" encountered the following error while trying to"
						" stop the preloader:</p>"
						"<pre>" + escapeHTML(originalException.what()) + "</pre>");
					throw e.finalize();
				}

				session.journey.setStepErrored(SPAWNING_KIT_PREPARATION);

				SpawnException e(crashException2.getException(),
					session.journey, session.config);
				e.setSummary(StaticString("An application preloader crashed: ") +
					crashException2.getException().what());
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However,"
					" this helper process crashed unexpectedly:</p>"
					"<pre>" + escapeHTML(crashException2.getException().what())
					+ "</pre>");
				throw e.finalize();
			}
		}
	}

	ForkResult internalInvokeForkCommand(HandshakeSession &session) {
		TRACE_POINT();

		session.journey.setStepInProgress(SPAWNING_KIT_CONNECT_TO_PRELOADER);
		FileDescriptor fd;
		string line;
		Json::Value doc;
		try {
			fd = connectToPreloader(session);
		} catch (const SystemException &e) {
			throw PreloaderCrashed(e);
		} catch (const IOException &e) {
			throw PreloaderCrashed(e);
		}

		session.journey.setStepPerformed(SPAWNING_KIT_CONNECT_TO_PRELOADER);
		session.journey.setStepInProgress(SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER);
		try {
			sendForkCommand(session, fd);
		} catch (const SystemException &e) {
			throw PreloaderCrashed(e);
		} catch (const IOException &e) {
			throw PreloaderCrashed(e);
		}

		session.journey.setStepPerformed(SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER);
		session.journey.setStepInProgress(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER);
		try {
			line = readForkCommandResponse(session, fd);
		} catch (const SystemException &e) {
			throw PreloaderCrashed(e);
		} catch (const IOException &e) {
			throw PreloaderCrashed(e);
		}

		session.journey.setStepPerformed(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER);
		session.journey.setStepInProgress(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER);
		try {
			doc = parseForkCommandResponse(session, line);
		} catch (...) {
			session.journey.setStepErrored(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER);
			throw;
		}

		session.journey.setStepPerformed(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER);
		session.journey.setStepInProgress(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);
		try {
			return handleForkCommandResponse(session, doc);
		} catch (...) {
			session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);
			throw;
		}
	}

	void sendForkCommand(HandshakeSession &session, const FileDescriptor &fd) {
		TRACE_POINT();
		Json::Value doc;

		doc["command"] = "spawn";
		doc["work_dir"] = session.workDir->getPath();

		writeExact(fd, Json::FastWriter().write(doc), &session.timeoutUsec);
	}

	string readForkCommandResponse(HandshakeSession &session, const FileDescriptor &fd) {
		TRACE_POINT();
		BufferedIO io(fd);

		try {
			return io.readLine(10240, &session.timeoutUsec);
		} catch (const SecurityException &) {
			session.journey.setStepErrored(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER);

			SpawnException e(INTERNAL_ERROR, session.journey, session.config);
			addPreloaderAnnotations(e);
			e.setSummary("The preloader process sent a response that exceeds the maximum size limit.");
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" this helper process sent a response that exceeded the"
				" internally-defined maximum size limit.</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"" SUPPORT_URL "\">"
				"report this bug</a>."
				"</p>");
			throw e.finalize();
		}
	}

	Json::Value parseForkCommandResponse(HandshakeSession &session, const string &data) {
		TRACE_POINT();
		Json::Value doc;
		Json::Reader reader;

		if (!reader.parse(data, doc)) {
			session.journey.setStepErrored(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER);

			SpawnException e(INTERNAL_ERROR, session.journey, session.config);
			addPreloaderAnnotations(e);
			e.setSummary("The preloader process sent an unparseable response: " + data);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" this helper process sent a response that looks like"
				" gibberish.</p>"
				"<p>The response is as follows:</p>"
				"<pre>" + escapeHTML(data) + "</pre>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"" SUPPORT_URL "\">"
				"report this bug</a>."
				"</p>");
			throw e.finalize();
		}

		UPDATE_TRACE_POINT();
		if (!validateForkCommandResponse(doc)) {
			session.journey.setStepErrored(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER);

			SpawnException e(INTERNAL_ERROR, session.journey, session.config);
			addPreloaderAnnotations(e);
			e.setSummary("The preloader process sent a response that does not"
				" match the expected structure: " + stringifyJson(doc));
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" this helper process sent a response that does not match"
				" the structure that " SHORT_PROGRAM_NAME " expects.</p>"
				"<p>The response is as follows:</p>"
				"<pre>" + escapeHTML(doc.toStyledString()) + "</pre>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"" SUPPORT_URL "\">"
				"report this bug</a>."
				"</p>");
			throw e.finalize();
		}

		return doc;
	}

	bool validateForkCommandResponse(const Json::Value &doc) const {
		if (!doc.isObject()) {
			return false;
		}
		if (!doc.isMember("result") || !doc["result"].isString()) {
			return false;
		}
		if (doc["result"].asString() == "ok") {
			if (!doc.isMember("pid") || !doc["pid"].isInt()) {
				return false;
			}
			return true;
		} else if (doc["result"].asString() == "error") {
			if (!doc.isMember("message") || !doc["message"].isString()) {
				return false;
			}
			return true;
		} else {
			return false;
		}
	}

	ForkResult handleForkCommandResponse(HandshakeSession &session, const Json::Value &doc) {
		TRACE_POINT();
		if (doc["result"].asString() == "ok") {
			return handleForkCommandResponseSuccess(session, doc);
		} else {
			P_ASSERT_EQ(doc["result"].asString(), "error");
			return handleForkCommandResponseError(session ,doc);
		}
	}

	ForkResult handleForkCommandResponseSuccess(HandshakeSession &session,
		const Json::Value &doc)
	{
		TRACE_POINT();
		pid_t spawnedPid = doc["pid"].asInt();
		ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, spawnedPid));

		FileDescriptor spawnedStdin, spawnedStdoutAndErr;
		BackgroundIOCapturerPtr stdoutAndErrCapturer;

		if (fileExists(session.responseDir + "/stdin")) {
			spawnedStdin = openFifoWithTimeout(
				session.responseDir + "/stdin",
				session.timeoutUsec);
			P_LOG_FILE_DESCRIPTOR_PURPOSE(spawnedStdin,
				"App " << spawnedPid << " (" << options.appRoot
				<< ") stdin");
		}

		if (fileExists(session.responseDir + "/stdout_and_err")) {
			spawnedStdoutAndErr = openFifoWithTimeout(
				session.responseDir + "/stdout_and_err",
				session.timeoutUsec);
			P_LOG_FILE_DESCRIPTOR_PURPOSE(spawnedStdoutAndErr,
				"App " << spawnedPid << " (" << options.appRoot
				<< ") stdoutAndErr");
			stdoutAndErrCapturer = boost::make_shared<BackgroundIOCapturer>(
				spawnedStdoutAndErr, spawnedPid);
			stdoutAndErrCapturer->start();
		}

		// How do we know the preloader actually forked a process
		// instead of reporting the PID of a random other existing process?
		// For security reasons we perform a UID check.
		uid_t spawnedUid = getProcessUid(session, spawnedPid, stdoutAndErrCapturer);
		if (spawnedUid != session.uid) {
			session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);

			SpawnException e(INTERNAL_ERROR, session.journey, session.config);
			addPreloaderAnnotations(e);
			e.setSummary("The process that the preloader said it spawned, PID "
				+ toString(spawnedPid) + ", has UID " + toString(spawnedUid)
				+ ", but the expected UID is " + toString(session.uid));
			e.setStdoutAndErrData(getBackgroundIOCapturerData(stdoutAndErrCapturer));
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" the web application process that the preloader started"
				" belongs to the wrong user. The UID of the web"
				" application process should be " + toString(session.uid)
				+ ", but is actually " + toString(session.uid) + ".</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"" SUPPORT_URL "\">"
				"report this bug</a>."
				"</p>");
			throw e.finalize();
		}

		stdoutAndErrCapturer->stop();
		guard.clear();
		return ForkResult(spawnedPid, spawnedStdin, spawnedStdoutAndErr);
	}

	ForkResult handleForkCommandResponseError(HandshakeSession &session,
		const Json::Value &doc)
	{
		session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);

		SpawnException e(INTERNAL_ERROR, session.journey, session.config);
		addPreloaderAnnotations(e);
		e.setSummary("An error occured while starting the web application: "
			+ doc["message"].asString());
		e.setProblemDescriptionHTML(
			"<p>The " PROGRAM_NAME " application server tried to"
			" start the web application by communicating with a"
			" helper process that we call a \"preloader\". However, "
			" this helper process reported an error:</p>"
			"<pre>" + escapeHTML(doc["message"].asString()) + "</pre>");
		e.setSolutionDescriptionHTML(
			"<p class=\"sole-solution\">"
			"Please try troubleshooting the problem by studying the"
			" <strong>error message</strong> and the"
			" <strong>diagnostics</strong> reports. You can also"
			" consult <a href=\"" SUPPORT_URL "\">the " SHORT_PROGRAM_NAME
			" support resources</a> for help.</p>");
		throw e.finalize();
	}

	string getBackgroundIOCapturerData(const BackgroundIOCapturerPtr &capturer) const {
		if (capturer != NULL) {
			// Sleep shortly to allow the child process to finish writing logs.
			syscalls::usleep(50000);
			return capturer->getData();
		} else {
			return string();
		}
	}

	uid_t getProcessUid(HandshakeSession &session, pid_t pid,
		const BackgroundIOCapturerPtr &stdoutAndErrCapturer)
	{
		uid_t uid = (uid_t) -1;

		try {
			vector<pid_t> pids;
			pids.push_back(pid);
			ProcessMetricMap result = ProcessMetricsCollector().collect(pids);
			uid = result[pid].uid;
		} catch (const ParseException &) {
			session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);

			SpawnException e(INTERNAL_ERROR, session.journey, session.config);
			addPreloaderAnnotations(e);
			e.setSummary("Unable to query the UID of spawned application process "
				+ toString(pid) + ": error parsing 'ps' output");
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application. As part of the starting"
				" sequence, " SHORT_PROGRAM_NAME " also tried to query"
				" the system user ID of the web application process"
				" using the operating system's \"ps\" tool. However,"
				" this tool returned output that " SHORT_PROGRAM_NAME
				" could not understand.</p>");
			e.setSolutionDescriptionHTML(
				createSolutionDescriptionForProcessMetricsCollectionError());
			throw e.finalize();
		} catch (const SystemException &originalException) {
			session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);

			SpawnException e(OPERATING_SYSTEM_ERROR, session.journey, session.config);
			addPreloaderAnnotations(e);
			e.setSummary("Unable to query the UID of spawned application process "
				+ toString(pid) + "; error capturing 'ps' output: "
				+ originalException.what());
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application. As part of the starting"
				" sequence, " SHORT_PROGRAM_NAME " also tried to query"
				" the system user ID of the web application process."
				" This is done by using the operating system's \"ps\""
				" tool and by querying operating system APIs and special"
				" files. However, an error was encountered while doing"
				" one of those things.</p>"
				"<p>The error returned by the operating system is as follows:</p>"
				"<pre>" + escapeHTML(originalException.what()) + "</pre>");
			e.setSolutionDescriptionHTML(
				createSolutionDescriptionForProcessMetricsCollectionError());
			throw e.finalize();
		}

		if (uid == (uid_t) -1) {
			if (osProcessExists(pid)) {
				session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);

				SpawnException e(INTERNAL_ERROR, session.journey, session.config);
				addPreloaderAnnotations(e);
				e.setSummary("Unable to query the UID of spawned application process "
					+ toString(pid) + ": 'ps' did not report information"
					" about this process");
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application. As part of the starting"
					" sequence, " SHORT_PROGRAM_NAME " also tried to query"
					" the system user ID of the web application process"
					" using the operating system's \"ps\" tool. However,"
					" this tool did not return any information about"
					" the web application process.</p>");
				e.setSolutionDescriptionHTML(
					createSolutionDescriptionForProcessMetricsCollectionError());
				throw e.finalize();
			} else {
				session.journey.setStepErrored(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER);

				SpawnException e(INTERNAL_ERROR, session.journey, session.config);
				addPreloaderAnnotations(e);
				e.setSummary("The application process spawned from the preloader"
					" seems to have exited prematurely");
				e.setStdoutAndErrData(getBackgroundIOCapturerData(stdoutAndErrCapturer));
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application. As part of the starting"
					" sequence, " SHORT_PROGRAM_NAME " also tried to query"
					" the system user ID of the web application process"
					" using the operating system's \"ps\" tool. However,"
					" this tool did not return any information about"
					" the web application process.</p>");
				e.setSolutionDescriptionHTML(
					createSolutionDescriptionForProcessMetricsCollectionError());
				throw e.finalize();
			}
		} else {
			return uid;
		}
	}

	static string createSolutionDescriptionForProcessMetricsCollectionError() {
		const char *path = getenv("PATH");
		if (path == NULL || path[0] == '\0') {
			path = "(empty)";
		}
		return "<div class=\"multiple-solutions\">"

			"<h3>Check whether the \"ps\" tool is installed and accessible by "
			SHORT_PROGRAM_NAME "</h3>"
			"<p>Maybe \"ps\" is not installed. Or maybe it is installed, but "
			SHORT_PROGRAM_NAME " cannot find it inside its PATH. Or"
			" maybe filesystem permissions disallow " SHORT_PROGRAM_NAME
			" from accessing \"ps\". Please check all these factors and"
			" fix them if necessary.</p>"
			"<p>" SHORT_PROGRAM_NAME "'s PATH is:</p>"
			"<pre>" + escapeHTML(path) + "</pre>"

			"<h3>Check whether the server is low on resources</h3>"
			"<p>Maybe the server is currently low on resources. This would"
			" cause the \"ps\" tool to encounter errors. Please study the"
			" <em>error message</em> and the <em>diagnostics reports</em> to"
			" verify whether this is the case. Key things to check for:</p>"
			"<ul>"
			"<li>Excessive CPU usage</li>"
			"<li>Memory and swap</li>"
			"<li>Ulimits</li>"
			"</ul>"
			"<p>If the server is indeed low on resources, find a way to"
			" free up some resources.</p>"

			"<h3>Check whether /proc is mounted</h3>"
			"<p>On many operating systems including Linux and FreeBSD, \"ps\""
			" only works if /proc is mounted. Please check this.</p>"

			"<h3>Still no luck?</h3>"
			"<p>Please try troubleshooting the problem by studying the"
			" <em>diagnostics</em> reports.</p>"

			"</div>";
	}

	static FileDescriptor openFifoWithTimeout(const string &path,
		unsigned long long &timeout)
	{
		TRACE_POINT();
		FileDescriptor fd;
		int errcode;
		oxt::thread thr(
			boost::bind(openFifoWithTimeoutThreadMain, path, &fd, &errcode),
			"FIFO opener: " + path, 1024 * 128);

		MonotonicTimeUsec startTime = SystemTime::getMonotonicUsec();
		ScopeGuard guard(boost::bind(adjustTimeout, startTime, &timeout));

		try {
			UPDATE_TRACE_POINT();
			if (thr.try_join_for(boost::chrono::microseconds(timeout))) {
				if (fd == -1) {
					throw SystemException("Cannot open FIFO " + path, errcode);
				} else {
					return fd;
				}
			} else {
				boost::this_thread::disable_interruption di;
				boost::this_thread::disable_syscall_interruption dsi;
				thr.interrupt_and_join();
				throw TimeoutException("Timeout opening FIFO " + path);
			}
		} catch (const boost::thread_interrupted &) {
			boost::this_thread::disable_interruption di;
			boost::this_thread::disable_syscall_interruption dsi;
			UPDATE_TRACE_POINT();
			thr.interrupt_and_join();
			throw;
		} catch (const boost::system::system_error &e) {
			throw SystemException(e.what(), e.code().value());
		}
	}

	static void openFifoWithTimeoutThreadMain(const string path,
		FileDescriptor *fd, int *errcode)
	{
		TRACE_POINT();
		fd->assign(syscalls::open(path.c_str(), O_RDONLY), __FILE__, __LINE__);
		*errcode = errno;
	}

	static void adjustTimeout(MonotonicTimeUsec startTime, unsigned long long *timeout) {
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;
		MonotonicTimeUsec now = SystemTime::getMonotonicUsec();
		assert(now >= startTime);
		MonotonicTimeUsec diff = startTime - now;
		if (*timeout >= diff) {
			*timeout -= diff;
		} else {
			*timeout = 0;
		}
	}

	static void doClosedir(DIR *dir) {
		closedir(dir);
	}

	static string findPreloaderCommandSocketAddress(const HandshakeSession &session) {
		const vector<Result::Socket> &sockets = session.result.sockets;
		vector<Result::Socket>::const_iterator it, end = sockets.end();
		for (it = sockets.begin(); it != end; it++) {
			if (it->protocol == "preloader") {
				return it->address;
			}
		}
		return string();
	}

	static StringKeyTable<string> loadAnnotationsFromEnvDumpDir(const string &envDumpDir) {
		string path = envDumpDir + "/annotations";
		DIR *dir = opendir(path.c_str());
		if (dir == NULL) {
			return StringKeyTable<string>();
		}

		ScopeGuard guard(boost::bind(doClosedir, dir));
		StringKeyTable<string> result;
		struct dirent *ent;
		while ((ent = readdir(dir)) != NULL) {
			if (ent->d_name[0] != '.') {
				result.insert(ent->d_name, strip(
					Passenger::readAll(path + "/" + ent->d_name)),
					true);
			}
		}

		result.compact();

		return result;
	}

	void addPreloaderAnnotations(SpawnException &e) const {
		StringKeyTable<string>::ConstIterator it(preloaderAnnotations);
		while (*it != NULL) {
			e.setAnnotation(it.getKey(), it.getValue(), false);
			it.next();
		}
	}

public:
	SmartSpawner(Context *context,
		const vector<string> &_preloaderCommand,
		const AppPoolOptions &_options)
		: Spawner(context),
		  preloaderCommand(_preloaderCommand)
	{
		if (preloaderCommand.size() < 2) {
			throw ArgumentException("preloaderCommand must have at least 2 elements");
		}

		options    = _options.copyAndPersist().detachFromUnionStationTransaction();
		pid        = -1;
		m_lastUsed = SystemTime::getUsec();
	}

	virtual ~SmartSpawner() {
		boost::lock_guard<boost::mutex> l(syncher);
		stopPreloader();
	}

	virtual Result spawn(const AppPoolOptions &options) {
		TRACE_POINT();
		P_ASSERT_EQ(options.appType, this->options.appType);
		P_ASSERT_EQ(options.appRoot, this->options.appRoot);

		P_DEBUG("Spawning new process: appRoot=" << options.appRoot);
		possiblyRaiseInternalError(options);

		{
			boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
			m_lastUsed = SystemTime::getUsec();
		}
		UPDATE_TRACE_POINT();
		boost::lock_guard<boost::mutex> l(syncher);
		if (!preloaderStarted()) {
			UPDATE_TRACE_POINT();
			startPreloader();
		}

		UPDATE_TRACE_POINT();
		Config config;
		Json::Value extraArgs;
		try {
			setConfigFromAppPoolOptions(&config, extraArgs, options);
		} catch (const std::exception &originalException) {
			Journey journey(SPAWN_THROUGH_PRELOADER, true);
			journey.setStepErrored(SPAWNING_KIT_PREPARATION, true);
			SpawnException e(originalException, journey, &config);
			addPreloaderAnnotations(e);
			throw e.finalize();
		}

		UPDATE_TRACE_POINT();
		Result result;
		HandshakeSession session(*context, config, SPAWN_THROUGH_PRELOADER);
		session.journey.setStepInProgress(SPAWNING_KIT_PREPARATION);

		try {
			HandshakePrepare(session, extraArgs).execute();

			ForkResult forkResult = invokeForkCommand(session);
			ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, forkResult.pid));
			P_DEBUG("Process forked for appRoot=" << options.appRoot << ": PID " << forkResult.pid);
			HandshakePerform(session, forkResult.pid, forkResult.stdinFd,
				forkResult.stdoutAndErrFd).execute();
			guard.clear();
			session.journey.setStepPerformed(SPAWNING_KIT_HANDSHAKE_PERFORM);
			P_DEBUG("Process spawning done: appRoot=" << options.appRoot <<
				", pid=" << forkResult.pid);
			return result;
		} catch (SpawnException &e) {
			addPreloaderAnnotations(e);
			throw e;
		} catch (const std::exception &originalException) {
			session.journey.setStepErrored(SPAWNING_KIT_PREPARATION);
			SpawnException e(originalException, session.journey,
				&config);
			addPreloaderAnnotations(e);
			throw e.finalize();
		}
	}

	virtual bool cleanable() const {
		return true;
	}

	virtual void cleanup() {
		TRACE_POINT();
		{
			boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
			m_lastUsed = SystemTime::getUsec();
		}
		boost::lock_guard<boost::mutex> lock(syncher);
		stopPreloader();
	}

	virtual unsigned long long lastUsed() const {
		boost::lock_guard<boost::mutex> lock(simpleFieldSyncher);
		return m_lastUsed;
	}

	pid_t getPreloaderPid() const {
		boost::lock_guard<boost::mutex> lock(simpleFieldSyncher);
		return pid;
	}
};


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_SMART_SPAWNER_H_ */
