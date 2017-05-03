/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2012-2017 Phusion Holding B.V.
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

/*
 * Sets given environment variables, dumps the entire environment to
 * a given file (for diagnostics purposes), then execs the given command.
 *
 * This is a separate executable because it does quite
 * some non-async-signal-safe stuff that we can't do after
 * fork()ing from the Spawner and before exec()ing.
 */

#include <oxt/initialize.hpp>
#include <oxt/backtrace.hpp>
#include <boost/scoped_array.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>

#include <jsoncpp/json.h>
#include <adhoc_lve.h>

#include <Logging.h>
#include <Utils.h>
#include <Utils/IOUtils.h>
#include <Utils/StrIntUtils.h>
#include <Core/SpawningKit/Exceptions.h>

using namespace std;
using namespace Passenger;

extern "C" {
	extern char **environ;
}


namespace Passenger {
namespace SpawnEnvSetupper {

	enum Mode {
		BEFORE_MODE,
		AFTER_MODE
	};

	struct Context {
		string workDir;
		Mode mode;
		Json::Value args;
		SpawningKit::JourneyStep step;
		MonotonicTimeUsec startTime;
	};

} // namespace SpawnEnvSetupper
} // namespace Passenger

using namespace Passenger::SpawnEnvSetupper;


static Json::Value
readArgsJson(const string &workDir) {
	Json::Reader reader;
	Json::Value result;
	string contents = readAll(workDir + "/args.json");
	if (reader.parse(contents, result)) {
		return result;
	} else {
		P_CRITICAL("Cannot parse " << workDir << "/args.json: "
			<< reader.getFormattedErrorMessages());
		exit(1);
		// Never reached
		return Json::Value();
	}
}

static void
initializeLogLevel(const Json::Value &args) {
	if (args.isMember("log_level")) {
		setLogLevel(args["log_level"].asInt());
	}
}

static void
recordJourneyStepInProgress(const Context &context,
	SpawningKit::JourneyStep step)
{
	string stepString = journeyStepToStringLowerCase(step);
	string path = context.workDir + "/response/steps/" + stepString;
	try {
		createFile((path + "/state").c_str(),
			SpawningKit::journeyStepStateToString(SpawningKit::STEP_IN_PROGRESS));
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
}

static void
recordJourneyStepComplete(const Context &context, SpawningKit::JourneyStep step,
	SpawningKit::JourneyStepState state, MonotonicTimeUsec startTime)
{
	MonotonicTimeUsec now =
		SystemTime::getMonotonicUsecWithGranularity<
			SystemTime::GRAN_10MSEC>();
	string stepString = journeyStepToStringLowerCase(step);
	string path = context.workDir + "/response/steps/" + stepString;
	try {
		makeDirTree(path);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
		return;
	}
	try {
		createFile((path + "/state").c_str(),
			SpawningKit::journeyStepStateToString(state));
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
		return;
	}
	try {
		createFile((path + "/duration").c_str(),
			toString((now - startTime) / 1000000));
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
}

static void
recordJourneyStepPerformed(const Context &context) {
	recordJourneyStepComplete(context, context.step, SpawningKit::STEP_PERFORMED,
		context.startTime);
}

static void
recordJourneyStepErrored(const Context &context) {
	recordJourneyStepComplete(context, context.step, SpawningKit::STEP_ERRORED,
		context.startTime);
}

static void
recordErrorCategory(const string &workDir, SpawningKit::ErrorCategory category) {
	string path = workDir + "/response/error/category";
	try {
		createFile(path.c_str(), errorCategoryToString(category));
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
}

static void
recordAdvancedProblemDetails(const string &workDir, const string &message) {
	string path = workDir + "/response/error/advanced_problem_details";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
}

static void
recordErrorSummary(const string &workDir, const string &message,
	bool isAlsoAdvancedProblemDetails)
{
	string path = workDir + "/response/error/summary";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
	if (isAlsoAdvancedProblemDetails) {
		recordAdvancedProblemDetails(workDir, message);
	}
}

static void
recordAndPrintErrorSummary(const string &workDir, const string &message,
	bool isAlsoAdvancedProblemDetails)
{
	fprintf(stderr, "Error: %s\n", message.c_str());
	recordErrorSummary(workDir, message, isAlsoAdvancedProblemDetails);
}

static void
recordProblemDescriptionHTML(const string &workDir, const string &message) {
	string path = workDir + "/response/error/problem_description.html";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
}

static void
recordSolutionDescriptionHTML(const string &workDir, const string &message) {
	string path = workDir + "/response/error/solution_description.html";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: %s\n", e.what());
	}
}

static void
dumpEnvvars(const string &workDir) {
	FILE *f = fopen((workDir + "/envdump/envvars").c_str(), "w");
	if (f != NULL) {
		unsigned int i = 0;
		while (environ[i] != NULL) {
			fputs(environ[i], f);
			putc('\n', f);
			i++;
		}
		fclose(f);
	}
}

static void
dumpUserInfo(const string &workDir) {
	FILE *f = fopen((workDir + "/envdump/user_info").c_str(), "w");
	if (f != NULL) {
		pid_t pid = fork();
		if (pid == 0) {
			dup2(fileno(f), 1);
			execlp("id", "id", (char *) 0);
			_exit(1);
		} else if (pid == -1) {
			int e = errno;
			fprintf(stderr, "Error: cannot fork a new process: %s (errno=%d)\n",
				strerror(e), e);
		} else {
			waitpid(pid, NULL, 0);
		}
		fclose(f);
	}
}

static void
dumpUlimits(const string &workDir) {
	FILE *f = fopen((workDir + "/envdump/ulimits").c_str(), "w");
	if (f != NULL) {
		pid_t pid = fork();
		if (pid == 0) {
			dup2(fileno(f), 1);
			execlp("ulimit", "ulimit", "-a", (char *) 0);
			_exit(1);
		} else if (pid == -1) {
			int e = errno;
			fprintf(stderr, "Error: cannot fork a new process: %s (errno=%d)\n",
			strerror(e), e);
		} else {
			waitpid(pid, NULL, 0);
		}
		fclose(f);
	}
}

static void
dumpAllEnvironmentInfo(const string &workDir) {
	dumpEnvvars(workDir);
	dumpUserInfo(workDir);
	dumpUlimits(workDir);
}

static bool
setUlimits(const Json::Value &args) {
	if (!args.isMember("file_descriptor_ulimit")) {
		return false;
	}

	rlim_t fdLimit = (rlim_t) args["file_descriptor_ulimit"].asUInt();
	struct rlimit limit;
	int ret;

	limit.rlim_cur = fdLimit;
	limit.rlim_max = fdLimit;
	do {
		ret = setrlimit(RLIMIT_NOFILE, &limit);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		int e = errno;
		fprintf(stderr, "Error: unable to set file descriptor ulimit to %u: %s (errno=%d)",
			(unsigned int) fdLimit, strerror(e), e);
	}

	return ret != -1;
}

static bool
canSwitchUser(const Json::Value &args) {
	return args.isMember("user") && geteuid() == 0;
}

static void
lookupUserGroup(const Context &context, uid_t *uid, struct passwd **userInfo,
	gid_t *gid)
{
	const Json::Value &args = context.args;
	errno = 0;
	*userInfo = getpwnam(args["user"].asCString());
	if (*userInfo == NULL) {
		int e = errno;
		if (looksLikePositiveNumber(args["user"].asString())) {
			fprintf(stderr,
				"Warning: error looking up system user database"
				" entry for user '%s': %s (errno=%d)\n",
				args["user"].asCString(), strerror(e), e);
			*uid = (uid_t) atoi(args["user"].asString());
		} else {
			recordJourneyStepErrored(context);
			recordErrorCategory(context.workDir,
				SpawningKit::OPERATING_SYSTEM_ERROR);
			recordAndPrintErrorSummary(context.workDir,
				"Cannot lookup up system user database entry for user '"
				+ args["user"].asString() + "': " + strerror(e)
				+ " (errno=" + toString(e) + ")",
				true);
			exit(1);
		}
	} else {
		*uid = (*userInfo)->pw_uid;
	}

	errno = 0;
	struct group *groupInfo = getgrnam(args["group"].asCString());
	if (groupInfo == NULL) {
		int e = errno;
		if (looksLikePositiveNumber(args["group"].asString())) {
			fprintf(stderr,
				"Warning: error looking up system group database entry for group '%s':"
				" %s (errno=%d)\n",
				args["group"].asCString(), strerror(e), e);
			*gid = (gid_t) atoi(args["group"].asString());
		} else {
			recordJourneyStepErrored(context);
			recordErrorCategory(context.workDir,
				SpawningKit::OPERATING_SYSTEM_ERROR);
			recordAndPrintErrorSummary(context.workDir,
				"Cannot lookup up system group database entry for group '"
				+ args["group"].asString() + "': " + strerror(e)
				+ " (errno=" + toString(e) + ")",
				true);
			exit(1);
		}
	} else {
		*gid = groupInfo->gr_gid;
	}
}

static void
enterLveJail(const Context &context, const struct passwd *userInfo) {
	string lveInitErr;
	adhoc_lve::LibLve &liblve = adhoc_lve::LveInitSignleton::getInstance(&lveInitErr);

	if (liblve.is_error()) {
		if (!lveInitErr.empty()) {
			lveInitErr = ": " + lveInitErr;
		}
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::INTERNAL_ERROR);
		recordAndPrintErrorSummary(context.workDir,
			"Failed to initialize LVE library: " + lveInitErr,
			true);
		exit(1);
	}

	if (!liblve.is_lve_available()) {
		return;
	}

	string jailErr;
	int ret = liblve.jail(userInfo, jailErr);
	if (ret < 0) {
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::INTERNAL_ERROR);
		recordAndPrintErrorSummary(context.workDir,
			"enterLve() failed: " + jailErr,
			true);
		exit(1);
	}
}

static void
switchGroup(const Context &context, uid_t uid, const struct passwd *userInfo, gid_t gid) {
	if (userInfo != NULL) {
		bool setgroupsCalled = false;

		#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
			#ifdef __APPLE__
				int groups[1024];
				int ngroups = sizeof(groups) / sizeof(int);
			#else
				gid_t groups[1024];
				int ngroups = sizeof(groups) / sizeof(gid_t);
			#endif
			boost::scoped_array<gid_t> gidset;

			int ret = getgrouplist(userInfo->pw_name, gid,
				groups, &ngroups);
			if (ret == -1) {
				int e = errno;
				recordJourneyStepErrored(context);
				recordErrorCategory(context.workDir,
					SpawningKit::OPERATING_SYSTEM_ERROR);
				recordAndPrintErrorSummary(context.workDir,
					"getgrouplist(" + string(userInfo->pw_name) + ", "
					+ toString(gid) + ") failed: " + strerror(e)
					+ " (errno=" + toString(e) + ")",
					true);
				exit(1);
			}

			if (ngroups <= NGROUPS_MAX) {
				setgroupsCalled = true;
				gidset.reset(new gid_t[ngroups]);
				if (setgroups(ngroups, gidset.get()) == -1) {
					int e = errno;
					recordJourneyStepErrored(context);
					recordErrorCategory(context.workDir,
						SpawningKit::OPERATING_SYSTEM_ERROR);
					recordAndPrintErrorSummary(context.workDir,
						"setgroups(" + toString(ngroups)
						+ ", ...) failed: " + strerror(e) + " (errno="
						+ toString(e) + ")",
						true);
					exit(1);
				}
			}
		#endif

		if (!setgroupsCalled && initgroups(userInfo->pw_name, gid) == -1) {
			int e = errno;
			recordJourneyStepErrored(context);
			recordErrorCategory(context.workDir,
				SpawningKit::OPERATING_SYSTEM_ERROR);
			recordAndPrintErrorSummary(context.workDir,
				"initgroups(" + string(userInfo->pw_name)
				+ ", " + toString(gid) + ") failed: " + strerror(e)
				+ " (errno=" + toString(e) + ")",
				true);
			exit(1);
		}
	}

	if (setgid(gid) == -1) {
		int e = errno;
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::OPERATING_SYSTEM_ERROR);
		recordAndPrintErrorSummary(context.workDir,
			"setgid(" + toString(gid) + ") failed: "
			+ strerror(e) + " (errno=" + toString(e) + ")",
			true);
		exit(1);
	}
}

static void
switchUser(const Context &context, uid_t uid, const struct passwd *userInfo) {
	if (setuid(uid) == -1) {
		int e = errno;
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::OPERATING_SYSTEM_ERROR);
		recordAndPrintErrorSummary(context.workDir,
			"setuid(" + toString(uid) + ") failed: " + strerror(e)
			+ " (errno=" + toString(e) + ")",
			true);
		exit(1);
	}
	if (userInfo != NULL) {
		setenv("USER", userInfo->pw_name, 1);
		setenv("LOGNAME", userInfo->pw_name, 1);
		setenv("SHELL", userInfo->pw_shell, 1);
		setenv("HOME", userInfo->pw_dir, 1);
	} else {
		unsetenv("USER");
		unsetenv("LOGNAME");
		unsetenv("SHELL");
		unsetenv("HOME");
	}
}

static string
lookupCurrentUserShell() {
	struct passwd *userInfo = getpwuid(getuid());
	if (userInfo == NULL) {
		int e = errno;
		fprintf(stderr, "Warning: cannot lookup system user database"
			" entry for UID %d: %s (errno=%d)\n",
			(int) getuid(), strerror(e), e);
		return "/bin/sh";
	} else {
		return userInfo->pw_shell;
	}
}

static vector<string>
inferAllParentDirectories(const string &path) {
	vector<string> components, result;

	split(path, '/', components);
	P_ASSERT_EQ(components.front(), "");
	components.erase(components.begin());

	for (unsigned int i = 0; i < components.size(); i++) {
		string path2;
		for (unsigned int j = 0; j <= i; j++) {
			path2.append("/");
			path2.append(components[j]);
		}
		if (path2.empty()) {
			path2 = "/";
		}
		result.push_back(path2);
	}

	P_ASSERT_EQ(result.back(), path);
	return result;
}

static void
setCurrentWorkingDirectory(const Context &context) {
	string appRoot = absolutizePath(context.args["app_root"].asString());
	vector<string> appRootAndParentDirs = inferAllParentDirectories(appRoot);
	vector<string>::const_iterator it;
	int ret;

	for (it = appRootAndParentDirs.begin(); it != appRootAndParentDirs.end(); it++) {
		struct stat buf;
		ret = stat(it->c_str(), &buf);
		if (ret == -1 && errno == EACCES) {
			char parent[PATH_MAX];
			const char *end = strrchr(it->c_str(), '/');
			memcpy(parent, it->c_str(), end - it->c_str());
			parent[end - it->c_str()] = '\0';

			recordJourneyStepErrored(context);
			recordErrorCategory(context.workDir,
				SpawningKit::OPERATING_SYSTEM_ERROR);
			recordAndPrintErrorSummary(context.workDir,
				"Directory '" + string(parent) + "' is inaccessible because of a"
				" filesystem permission error.",
				false);
			recordProblemDescriptionHTML(context.workDir,
				"<p>"
				"The " PROGRAM_NAME " application server tried to start the"
				" web application as user '" + escapeHTML(getProcessUsername())
				+ "' and group '" + escapeHTML(getGroupName(getgid()))
				+ "'. During this process, " SHORT_PROGRAM_NAME
				" must be able to access its application root directory '"
				+ escapeHTML(appRoot) + "'. However, the parent directory '"
				+ escapeHTML(parent) + "' has wrong permissions, thereby preventing this"
				" process from accessing its application root directory."
				"</p>");
			recordSolutionDescriptionHTML(context.workDir,
				"<p class=\"sole-solution\">"
				"Please fix the permissions of the directory '" + escapeHTML(appRoot)
				+ "' in such a way that the directory is accessible by user '"
				+ escapeHTML(getProcessUsername()) + "' and group '"
				+ escapeHTML(getGroupName(getgid())) + "'."
				"</p>");
			exit(1);
		} else if (ret == -1) {
			int e = errno;
			recordJourneyStepErrored(context);
			recordErrorCategory(context.workDir,
				SpawningKit::OPERATING_SYSTEM_ERROR);
			recordAndPrintErrorSummary(context.workDir,
				"Unable to stat() directory '" + *it + "': "
				+ strerror(e) + " (errno=" + toString(e) + ")",
				true);
			exit(1);
		}
	}

	ret = chdir(appRoot.c_str());
	if (ret != 0) {
		int e = errno;
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::OPERATING_SYSTEM_ERROR);
		recordAndPrintErrorSummary(context.workDir,
			"Unable to change working directory to '" + appRoot + "': "
			+ strerror(e) + " (errno=" + toString(e) + ")",
			true);
		if (e == EPERM || e == EACCES) {
			recordProblemDescriptionHTML(context.workDir,
				"<p>The " PROGRAM_NAME " application server tried to start the"
				" web application as user " + escapeHTML(getProcessUsername())
				+ " and group " + escapeHTML(getGroupName(getgid()))
				+ ", with a working directory of "
				+ escapeHTML(appRoot) + ". However, it encountered a filesystem"
				" permission error while doing this.</p>");
		} else {
			recordProblemDescriptionHTML(context.workDir,
				"<p>The " PROGRAM_NAME " application server tried to start the"
				" web application as user " + escapeHTML(getProcessUsername())
				+ " and group " + escapeHTML(getGroupName(getgid()))
				+ ", with a working directory of "
				+ escapeHTML(appRoot) + ". However, it encountered a filesystem"
				" error while doing this.</p>");
		}
		exit(1);
	}

	// The application root may contain one or more symlinks
	// in its path. If the application calls getcwd(), it will
	// get the resolved path.
	//
	// It turns out that there is no such thing as a path without
	// unresolved symlinks. The shell presents a working directory with
	// unresolved symlinks (which it calls the "logical working directory"),
	// but that is an illusion provided by the shell. The shell reports
	// the logical working directory though the PWD environment variable.
	//
	// See also:
	// https://github.com/phusion/passenger/issues/1596#issuecomment-138154045
	// http://git.savannah.gnu.org/cgit/coreutils.git/tree/src/pwd.c
	// http://www.opensource.apple.com/source/shell_cmds/shell_cmds-170/pwd/pwd.c
	setenv("PWD", appRoot.c_str(), 1);
}

static void
setDefaultEnvvars(const Json::Value &args) {
	setenv("PYTHONUNBUFFERED", "1", 1);

	setenv("NODE_PATH", args["node_libdir"].asCString(), 1);

	setenv("RAILS_ENV", args["app_env"].asCString(), 1);
	setenv("RACK_ENV", args["app_env"].asCString(), 1);
	setenv("WSGI_ENV", args["app_env"].asCString(), 1);
	setenv("NODE_ENV", args["app_env"].asCString(), 1);
	setenv("PASSENGER_APP_ENV", args["app_env"].asCString(), 1);

	if (args.isMember("expected_start_port")) {
		setenv("PORT", toString(args["expected_start_port"].asInt()).c_str(), 1);
	}

	if (args["base_uri"].asString() != "/") {
		setenv("RAILS_RELATIVE_URL_ROOT", args["base_uri"].asCString(), 1);
		setenv("RACK_BASE_URI", args["base_uri"].asCString(), 1);
		setenv("PASSENGER_BASE_URI", args["base_uri"].asCString(), 1);
	} else {
		unsetenv("RAILS_RELATIVE_URL_ROOT");
		unsetenv("RACK_BASE_URI");
		unsetenv("PASSENGER_BASE_URI");
	}
}

static void
setGivenEnvVars(const Json::Value &args) {
	const Json::Value &envvars = args["environment_variables"];
	Json::Value::const_iterator it, end = envvars.end();

	for (it = envvars.begin(); it != end; it++) {
		string key = it.name();
		setenv(key.c_str(), it->asCString(), 1);
	}
}

static bool
shouldLoadShellEnvvars(const Json::Value &args, const string &shell) {
	if (args["load_shell_envvars"].asBool()) {
		string shellName = extractBaseName(shell);
		return shellName == "bash" || shellName == "zsh" || shellName == "ksh";
	} else {
		return false;
	}
}

static string
commandArgsToString(const vector<const char *> &commandArgs) {
	vector<const char *>::const_iterator it;
	string result;

	for (it = commandArgs.begin(); it != commandArgs.end(); it++) {
		if (*it != NULL) {
			result.append(*it);
			result.append(1, ' ');
		}
	}

	return strip(result);
}

static void
execNextCommand(const Context &context, const string &shell)
{
	vector<const char *> commandArgs;
	SpawningKit::JourneyStep nextJourneyStep;

	// Note: do not try to set a process title in this function by messing with argv[0].
	// https://code.google.com/p/phusion-passenger/issues/detail?id=855

	if (context.mode == BEFORE_MODE) {
		assert(!shell.empty());
		if (shouldLoadShellEnvvars(context.args, shell)) {
			nextJourneyStep = SpawningKit::SUBPROCESS_OS_SHELL;
			commandArgs.push_back(shell.c_str());
			commandArgs.push_back("-lc");
			commandArgs.push_back("exec \"$@\"");
			commandArgs.push_back("SpawnEnvSetupperShell");
		} else {
			nextJourneyStep = SpawningKit::SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL;
		}
		commandArgs.push_back(context.args["passenger_agent_path"].asCString());
		commandArgs.push_back("spawn-env-setupper");
		commandArgs.push_back(context.workDir.c_str());
		commandArgs.push_back("--after");
	} else {
		if (context.args["starts_using_wrapper"].asBool()) {
			nextJourneyStep = SpawningKit::SUBPROCESS_EXEC_WRAPPER;
		} else {
			nextJourneyStep = SpawningKit::SUBPROCESS_APP_LOAD_OR_EXEC;
		}
		commandArgs.push_back("/bin/sh");
		commandArgs.push_back("-c");
		commandArgs.push_back(context.args["start_command"].asCString());
	}
	commandArgs.push_back(NULL);

	MonotonicTimeUsec nextStepStartTime =
		SystemTime::getMonotonicUsecWithGranularity<
			SystemTime::GRAN_10MSEC>();
	recordJourneyStepPerformed(context);
	recordJourneyStepInProgress(context, nextJourneyStep);

	execvp(commandArgs[0], (char * const *) &commandArgs[0]);

	int e = errno;
	recordJourneyStepComplete(context, nextJourneyStep,
		SpawningKit::STEP_ERRORED, nextStepStartTime);
	recordErrorCategory(context.workDir, SpawningKit::OPERATING_SYSTEM_ERROR);
	recordAndPrintErrorSummary(context.workDir,
		"Unable to execute command '" + commandArgsToString(commandArgs)
		+ "': " + strerror(e) + " (errno=" + toString(e) + ")",
		true);
	exit(1);
}

int
spawnEnvSetupperMain(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc != 4) {
		fprintf(stderr, "Usage: PassengerAgent spawn-env-setupper <workdir> <--before|--after>\n");
		exit(1);
	}

	oxt::initialize();
	oxt::setup_syscall_interruption_support();

	Context context;
	context.workDir = argv[2];
	context.mode =
		(strcmp(argv[3], "--before") == 0)
		? BEFORE_MODE
		: AFTER_MODE;
	context.step =
		(context.mode == BEFORE_MODE)
		? SpawningKit::SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL
		: SpawningKit::SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL;
	context.startTime = SystemTime::getMonotonicUsecWithGranularity<
		SystemTime::GRAN_10MSEC>();

	setenv("IN_PASSENGER", "1", 1);
	setenv("PASSENGER_SPAWN_WORK_DIR", context.workDir.c_str(), 1);
	recordJourneyStepComplete(context, SpawningKit::SUBPROCESS_BEFORE_FIRST_EXEC,
		SpawningKit::STEP_PERFORMED, context.startTime);
	recordJourneyStepInProgress(context, context.step);

	try {
		context.args = readArgsJson(context.workDir);
		bool shouldTrySwitchUser = canSwitchUser(context.args);
		string shell;

		initializeLogLevel(context.args);
		dumpAllEnvironmentInfo(context.workDir);

		if (context.mode == BEFORE_MODE) {
			struct passwd *userInfo = NULL;
			uid_t uid;
			gid_t gid;

			setDefaultEnvvars(context.args);
			dumpEnvvars(context.workDir);

			if (shouldTrySwitchUser) {
				lookupUserGroup(context, &uid, &userInfo, &gid);
				shell = userInfo->pw_shell;
			} else {
				shell = lookupCurrentUserShell();
			}
			if (setUlimits(context.args)) {
				dumpUlimits(context.workDir);
			}
			if (shouldTrySwitchUser) {
				enterLveJail(context, userInfo);
				switchGroup(context, uid, userInfo, gid);
				dumpUserInfo(context.workDir);

				switchUser(context, uid, userInfo);
				dumpEnvvars(context.workDir);
				dumpUserInfo(context.workDir);
			}
		}

		setCurrentWorkingDirectory(context);
		dumpEnvvars(context.workDir);

		if (context.mode == AFTER_MODE) {
			setDefaultEnvvars(context.args);
			setGivenEnvVars(context.args);
			dumpEnvvars(context.workDir);
		}

		execNextCommand(context, shell);
	} catch (const oxt::tracable_exception &e) {
		fprintf(stderr, "Error: %s\n%s\n",
			e.what(), e.backtrace().c_str());
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::inferErrorCategoryFromAnotherException(
				e, context.step));
		recordErrorSummary(context.workDir, e.what(), true);
		return 1;
	} catch (const std::exception &e) {
		fprintf(stderr, "Error: %s\n", e.what());
		recordJourneyStepErrored(context);
		recordErrorCategory(context.workDir,
			SpawningKit::inferErrorCategoryFromAnotherException(
				e, context.step));
		recordErrorSummary(context.workDir, e.what(), true);
		return 1;
	}

	// Should never be reached
	recordJourneyStepErrored(context);
	recordAndPrintErrorSummary(context.workDir,
		"*** BUG IN SpawnEnvSetupper ***: end of main() reached",
		true);
	return 1;
}
