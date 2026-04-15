#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <dirent.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

struct TimeStep {
    std::string name;
    double value;
};

enum class TimeSourceMode {
    Auto,
    Case,
    Processor
};

struct Config {
    std::string funcCmd;
    std::optional<std::string> timeArg;
    int requestedN = 1;
    TimeSourceMode timeSource = TimeSourceMode::Auto;
};

struct TaskResult {
    std::string timeName;
    int exitCode = -1;
    std::size_t peakRssBytes = 0;
    bool success = false;
};

struct RunningTask {
    pid_t pid = -1;
    std::string timeName;
    std::size_t peakRssBytes = 0;
};

static constexpr double MEMORY_USAGE_RATIO = 0.90;
static constexpr double MEMORY_SAFETY_FACTOR = 1.15;
static constexpr int POLL_MS = 300;

std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(begin, end - begin + 1);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string shellEscapeSingleQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

bool looksLikeNumber(const std::string& s, double& v) {
    char* end = nullptr;
    errno = 0;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && end != s.c_str() && errno == 0;
}

void usage() {
    std::cerr
        << "Usage: myParaFunc -func \"COMMAND ...\" [-time VALUE|START:END] [-n N] [-timeSource auto|case|processor]\n"
        << "Examples:\n"
        << "  myParaFunc -func \"postProcess -func Q\" -time \"0.902:1.4\" -n 8\n"
        << "  myParaFunc -func \"reconstructPar\" -timeSource processor -n 4\n";
}

TimeSourceMode parseTimeSource(const std::string& value) {
    const std::string s = toLower(trim(value));
    if (s == "auto") return TimeSourceMode::Auto;
    if (s == "case") return TimeSourceMode::Case;
    if (s == "processor") return TimeSourceMode::Processor;
    throw std::runtime_error("Invalid -timeSource value: " + value + " (expected auto|case|processor)");
}

std::string timeSourceToString(TimeSourceMode mode) {
    switch (mode) {
        case TimeSourceMode::Auto: return "auto";
        case TimeSourceMode::Case: return "case";
        case TimeSourceMode::Processor: return "processor";
    }
    return "auto";
}

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-func") {
            if (i + 1 >= argc) throw std::runtime_error("-func requires a value");
            cfg.funcCmd = argv[++i];
        } else if (arg == "-time") {
            if (i + 1 >= argc) throw std::runtime_error("-time requires a value");
            cfg.timeArg = argv[++i];
        } else if (arg == "-n") {
            if (i + 1 >= argc) throw std::runtime_error("-n requires a value");
            cfg.requestedN = std::stoi(argv[++i]);
            if (cfg.requestedN <= 0) throw std::runtime_error("-n must be >= 1");
        } else if (arg == "-timeSource") {
            if (i + 1 >= argc) throw std::runtime_error("-timeSource requires a value");
            cfg.timeSource = parseTimeSource(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    if (cfg.funcCmd.empty()) throw std::runtime_error("-func is required");

    std::regex timeToken(R"((^|\s)-time(\s|$))");
    if (std::regex_search(cfg.funcCmd, timeToken)) {
        throw std::runtime_error("Do not include -time inside -func; myParaFunc appends it automatically.");
    }
    return cfg;
}

std::string extractPrimaryCommand(const std::string& funcCmd) {
    std::istringstream iss(funcCmd);
    std::string token;
    if (!(iss >> token)) return "";
    try {
        return fs::path(token).filename().string();
    } catch (...) {
        return token;
    }
}

bool shouldUseProcessorTimeSourceAuto(const std::string& funcCmd) {
    const std::string primary = toLower(extractPrimaryCommand(funcCmd));
    return primary == "reconstructpar" || primary == "reconstructparmesh";
}

TimeSourceMode resolveTimeSource(const Config& cfg) {
    if (cfg.timeSource != TimeSourceMode::Auto) return cfg.timeSource;
    return shouldUseProcessorTimeSourceAuto(cfg.funcCmd) ? TimeSourceMode::Processor : TimeSourceMode::Case;
}

std::string runCommandCapture(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("Failed to run command: " + cmd);
    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    const int rc = pclose(pipe);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        throw std::runtime_error("Command failed: " + cmd);
    }
    return output;
}

std::vector<TimeStep> parseFoamListTimesOutput(const std::string& output) {
    std::istringstream iss(output);
    std::vector<TimeStep> steps;
    std::string token;
    while (iss >> token) {
        double v = 0.0;
        if (looksLikeNumber(token, v)) {
            steps.push_back({token, v});
        }
    }
    std::sort(steps.begin(), steps.end(), [](const TimeStep& a, const TimeStep& b) {
        if (a.value == b.value) return a.name < b.name;
        return a.value < b.value;
    });
    steps.erase(std::unique(steps.begin(), steps.end(), [](const TimeStep& a, const TimeStep& b) {
        return a.name == b.name;
    }), steps.end());
    return steps;
}

std::vector<TimeStep> listTimeStepsFromFoam(TimeSourceMode sourceMode) {
    const std::string sourceFlag = (sourceMode == TimeSourceMode::Processor) ? " -processor" : "";
    std::vector<TimeStep> steps;
    try {
        steps = parseFoamListTimesOutput(runCommandCapture("foamListTimes -noZero" + sourceFlag + " 2>/dev/null"));
    } catch (...) {
    }
    if (steps.empty()) {
        steps = parseFoamListTimesOutput(runCommandCapture("foamListTimes" + sourceFlag + " 2>/dev/null"));
    }
    return steps;
}

std::vector<TimeStep> filterTimeSteps(const std::vector<TimeStep>& all, const std::optional<std::string>& timeArg) {
    if (!timeArg.has_value()) return all;
    const std::string s = trim(*timeArg);
    const auto pos = s.find(':');
    if (pos == std::string::npos) {
        double target = 0.0;
        if (!looksLikeNumber(s, target)) throw std::runtime_error("Invalid -time value: " + s);
        std::vector<TimeStep> out;
        for (const auto& t : all) {
            if (std::fabs(t.value - target) < 1e-12 || t.name == s) out.push_back(t);
        }
        if (out.empty()) throw std::runtime_error("No time step matches -time " + s);
        return out;
    }

    const std::string left = trim(s.substr(0, pos));
    const std::string right = trim(s.substr(pos + 1));
    double start = 0.0, end = 0.0;
    if (!looksLikeNumber(left, start) || !looksLikeNumber(right, end)) {
        throw std::runtime_error("Invalid -time range: " + s);
    }
    if (start > end) std::swap(start, end);
    std::vector<TimeStep> out;
    for (const auto& t : all) {
        if (t.value + 1e-12 >= start && t.value - 1e-12 <= end) out.push_back(t);
    }
    if (out.empty()) throw std::runtime_error("No time step is within range " + s);
    return out;
}

std::optional<std::size_t> readUnsignedFromFile(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs) return std::nullopt;
    std::string s;
    ifs >> s;
    if (!ifs || s.empty() || s == "max") return std::nullopt;
    try {
        return static_cast<std::size_t>(std::stoull(s));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> readDoubleFromFile(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs) return std::nullopt;
    double x;
    ifs >> x;
    if (!ifs) return std::nullopt;
    return x;
}

std::size_t getMemAvailableFromProc() {
    std::ifstream ifs("/proc/meminfo");
    std::string key, unit;
    std::size_t value = 0;
    while (ifs >> key >> value >> unit) {
        if (key == "MemAvailable:") return value * 1024ULL;
    }
    throw std::runtime_error("Cannot read MemAvailable from /proc/meminfo");
}

std::size_t getCgroupMemoryAvailable() {
    const fs::path v2Max = "/sys/fs/cgroup/memory.max";
    const fs::path v2Current = "/sys/fs/cgroup/memory.current";
    if (fs::exists(v2Max) && fs::exists(v2Current)) {
        const auto maxOpt = readUnsignedFromFile(v2Max);
        const auto curOpt = readUnsignedFromFile(v2Current);
        if (maxOpt && curOpt && *maxOpt > *curOpt) {
            return *maxOpt - *curOpt;
        }
    }

    const fs::path v1Limit = "/sys/fs/cgroup/memory/memory.limit_in_bytes";
    const fs::path v1Usage = "/sys/fs/cgroup/memory/memory.usage_in_bytes";
    if (fs::exists(v1Limit) && fs::exists(v1Usage)) {
        const auto limitOpt = readUnsignedFromFile(v1Limit);
        const auto usageOpt = readUnsignedFromFile(v1Usage);
        if (limitOpt && usageOpt && *limitOpt > *usageOpt) {
            const std::size_t huge = (1ULL << 60);
            if (*limitOpt < huge) return *limitOpt - *usageOpt;
        }
    }

    return getMemAvailableFromProc();
}

std::size_t getEffectiveAvailableMemory() {
    return std::min(getMemAvailableFromProc(), getCgroupMemoryAvailable());
}

int countAffinityCpus() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        return std::max(1u, std::thread::hardware_concurrency());
    }
    int count = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &mask)) ++count;
    }
    return std::max(1, count);
}

int readCpuQuotaLimit() {
    const fs::path cpuMax = "/sys/fs/cgroup/cpu.max";
    if (fs::exists(cpuMax)) {
        std::ifstream ifs(cpuMax);
        std::string quotaStr, periodStr;
        if (ifs >> quotaStr >> periodStr && quotaStr != "max") {
            try {
                const double quota = std::stod(quotaStr);
                const double period = std::stod(periodStr);
                if (quota > 0 && period > 0) {
                    return std::max(1, static_cast<int>(std::floor(quota / period)));
                }
            } catch (...) {
            }
        }
    }

    const fs::path v1Quota = "/sys/fs/cgroup/cpu/cpu.cfs_quota_us";
    const fs::path v1Period = "/sys/fs/cgroup/cpu/cpu.cfs_period_us";
    if (fs::exists(v1Quota) && fs::exists(v1Period)) {
        const auto quotaOpt = readDoubleFromFile(v1Quota);
        const auto periodOpt = readDoubleFromFile(v1Period);
        if (quotaOpt && periodOpt && *quotaOpt > 0 && *periodOpt > 0) {
            return std::max(1, static_cast<int>(std::floor(*quotaOpt / *periodOpt)));
        }
    }
    return -1;
}

int getCpuHardLimit() {
    const int affinity = countAffinityCpus();
    const int quota = readCpuQuotaLimit();
    if (quota > 0) return std::max(1, std::min(affinity, quota));
    return affinity;
}

double getLoadAvg1() {
    double loads[3] = {0.0, 0.0, 0.0};
    if (getloadavg(loads, 3) == -1) return 0.0;
    return loads[0];
}

int estimateAvailableCpuSlots() {
    const int hardLimit = getCpuHardLimit();
    const double load1 = getLoadAvg1();
    int slots = hardLimit;
    if (load1 > 0.0) {
        slots = static_cast<int>(std::floor(hardLimit - load1 + 0.5));
    }
    return std::max(1, std::min(hardLimit, slots));
}

std::map<pid_t, pid_t> readParentMap() {
    std::map<pid_t, pid_t> parent;
    DIR* dir = opendir("/proc");
    if (!dir) return parent;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (!std::all_of(ent->d_name, ent->d_name + std::strlen(ent->d_name), ::isdigit)) continue;
        const pid_t pid = static_cast<pid_t>(std::atoi(ent->d_name));
        std::ifstream ifs(std::string("/proc/") + ent->d_name + "/status");
        if (!ifs) continue;
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.rfind("PPid:", 0) == 0) {
                std::istringstream iss(line.substr(5));
                pid_t ppid = 0;
                if (iss >> ppid) parent[pid] = ppid;
                break;
            }
        }
    }
    closedir(dir);
    return parent;
}

std::set<pid_t> collectDescendants(pid_t root) {
    const auto parent = readParentMap();
    std::set<pid_t> all{root};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [pid, ppid] : parent) {
            if (!all.count(pid) && all.count(ppid)) {
                all.insert(pid);
                changed = true;
            }
        }
    }
    return all;
}

std::size_t readRssBytes(pid_t pid) {
    std::ifstream ifs("/proc/" + std::to_string(pid) + "/status");
    if (!ifs) return 0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            std::size_t valueKb = 0;
            std::string unit;
            if (iss >> valueKb >> unit) return valueKb * 1024ULL;
        }
    }
    return 0;
}

std::size_t readProcessTreeRssBytes(pid_t root) {
    std::size_t total = 0;
    for (pid_t pid : collectDescendants(root)) total += readRssBytes(pid);
    return total;
}

std::string makeLogDir() {
    const auto t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << "myParaFunc_logs_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
    fs::create_directories(oss.str());
    return oss.str();
}

std::string buildCommand(const std::string& funcCmd, const std::string& timeName, const std::string& logPath) {
    std::ostringstream oss;
    oss << funcCmd << " -time " << shellEscapeSingleQuote(timeName)
        << " > " << shellEscapeSingleQuote(logPath) << " 2>&1";
    return oss.str();
}

pid_t startTask(const std::string& fullCmd) {
    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-lc", fullCmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

TaskResult waitTaskWithMonitoring(pid_t pid, const std::string& timeName) {
    TaskResult result;
    result.timeName = timeName;
    int status = 0;
    while (true) {
        result.peakRssBytes = std::max(result.peakRssBytes, readProcessTreeRssBytes(pid));
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r == -1) throw std::runtime_error("waitpid() failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.success = (result.exitCode == 0);
    return result;
}

bool pollTask(RunningTask& task, TaskResult& result) {
    task.peakRssBytes = std::max(task.peakRssBytes, readProcessTreeRssBytes(task.pid));
    int status = 0;
    const pid_t r = waitpid(task.pid, &status, WNOHANG);
    if (r == 0) return false;
    if (r == -1) throw std::runtime_error("waitpid() failed during polling");
    result.timeName = task.timeName;
    result.peakRssBytes = task.peakRssBytes;
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.success = (result.exitCode == 0);
    return true;
}

std::string bytesToHuman(std::size_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double x = static_cast<double>(bytes);
    int idx = 0;
    while (x >= 1024.0 && idx < 4) {
        x /= 1024.0;
        ++idx;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 2) << x << ' ' << units[idx];
    return oss.str();
}

int computeMemoryParallelism(std::size_t availableBytes, std::size_t perTaskPeakBytes) {
    if (perTaskPeakBytes == 0) return 1;
    const double allowed = static_cast<double>(availableBytes) * MEMORY_USAGE_RATIO;
    const double denom = static_cast<double>(perTaskPeakBytes) * MEMORY_SAFETY_FACTOR;
    return std::max(1, static_cast<int>(std::floor(allowed / denom)));
}

std::string makeFinishedIndent(std::size_t totalTasks) {
    const std::size_t digits = std::to_string(totalTasks).size();
    const std::size_t width = 1 + digits + 1 + digits + 13;
    return std::string(width, ' ');
}

int main(int argc, char* argv[]) {
    try {
        const Config cfg = parseArgs(argc, argv);
        const TimeSourceMode resolvedSource = resolveTimeSource(cfg);
        const auto allTimes = listTimeStepsFromFoam(resolvedSource);
        const auto times = filterTimeSteps(allTimes, cfg.timeArg);
        if (times.empty()) throw std::runtime_error("No time steps found to process.");

        const std::string logDir = makeLogDir();
        const int cpuHardLimit = getCpuHardLimit();
        const int cpuAvailNow = estimateAvailableCpuSlots();
        const std::string finishedIndent = makeFinishedIndent(times.size());

        std::cout << "func: " << cfg.funcCmd << "\n";
        std::cout << "time source: " << timeSourceToString(resolvedSource);
        if (cfg.timeSource == TimeSourceMode::Auto) std::cout << " (auto)";
        std::cout << "\n";
        std::cout << "total time steps: " << times.size() << "\n";
        std::cout << "requested n: " << cfg.requestedN << "\n";
        std::cout << "cpu hard limit: " << cpuHardLimit << "\n";
        std::cout << "cpu available now: " << cpuAvailNow << "\n";
        std::cout << "logs: " << logDir << "\n";

        std::size_t observedPeakPerTask = 0;
        std::vector<std::string> failedTimes;
        bool stopSubmitting = false;
        std::size_t submitted = 0;
        std::size_t completed = 0;

        {
            const auto& t = times.front();
            const std::string logPath = logDir + "/" + t.name + ".log";
            const std::string cmd = buildCommand(cfg.funcCmd, t.name, logPath);
            std::cout << "[1/" << times.size() << "] submitted: time=" << t.name << "\n";
            const pid_t pid = startTask(cmd);
            const TaskResult pilot = waitTaskWithMonitoring(pid, t.name);
            submitted = 1;
            completed = 1;
            observedPeakPerTask = std::max(observedPeakPerTask, pilot.peakRssBytes);
            if (!pilot.success) {
                std::cerr << finishedIndent << "time=" << pilot.timeName << " failed, exitCode=" << pilot.exitCode
                          << ", log=" << logPath << "\n";
                return 1;
            }
            std::cout << finishedIndent << "time=" << pilot.timeName << " finished\n";

            const std::size_t memAvail = getEffectiveAvailableMemory();
            const int memLimit = computeMemoryParallelism(memAvail, observedPeakPerTask);
            int actualNow = std::min({cfg.requestedN, estimateAvailableCpuSlots(), memLimit});
            actualNow = std::max(1, actualNow);
            std::cout << "pilot peak memory: " << bytesToHuman(observedPeakPerTask) << "\n";
            std::cout << "memory-limited parallelism: " << memLimit << "\n";
            std::cout << "actual parallelism now: " << actualNow << "\n";
        }

        std::vector<RunningTask> running;
        std::size_t nextIndex = 1;

        while (completed < times.size()) {
            for (std::size_t i = 0; i < running.size();) {
                TaskResult res;
                if (pollTask(running[i], res)) {
                    ++completed;
                    observedPeakPerTask = std::max(observedPeakPerTask, res.peakRssBytes);
                    if (!res.success) {
                        failedTimes.push_back(res.timeName);
                        stopSubmitting = true;
                        std::cerr << finishedIndent << "time=" << res.timeName << " failed, exitCode=" << res.exitCode << "\n";
                    } else {
                        std::cout << finishedIndent << "time=" << res.timeName << " finished\n";
                    }
                    running.erase(running.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }

            if (!stopSubmitting && nextIndex < times.size()) {
                int cpuAvail = estimateAvailableCpuSlots();
                std::size_t memAvail = getEffectiveAvailableMemory();
                int memLimit = computeMemoryParallelism(memAvail, observedPeakPerTask);
                int allowed = std::min({cfg.requestedN, cpuAvail, memLimit});
                allowed = std::max(1, allowed);

                while (!stopSubmitting && nextIndex < times.size() && static_cast<int>(running.size()) < allowed) {
                    const auto& t = times[nextIndex];
                    const std::string logPath = logDir + "/" + t.name + ".log";
                    const std::string cmd = buildCommand(cfg.funcCmd, t.name, logPath);
                    const pid_t pid = startTask(cmd);
                    running.push_back({pid, t.name, 0});
                    ++submitted;
                    std::cout << "[" << submitted << "/" << times.size() << "] submitted: time=" << t.name << "\n";
                    ++nextIndex;

                    cpuAvail = estimateAvailableCpuSlots();
                    memAvail = getEffectiveAvailableMemory();
                    memLimit = computeMemoryParallelism(memAvail, observedPeakPerTask);
                    allowed = std::min({cfg.requestedN, cpuAvail, memLimit});
                    allowed = std::max(1, allowed);
                }
            }

            if (running.empty()) {
                if (stopSubmitting || nextIndex >= times.size()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        }

        while (!running.empty()) {
            for (std::size_t i = 0; i < running.size();) {
                TaskResult res;
                if (pollTask(running[i], res)) {
                    ++completed;
                    observedPeakPerTask = std::max(observedPeakPerTask, res.peakRssBytes);
                    if (!res.success) {
                        failedTimes.push_back(res.timeName);
                        std::cerr << finishedIndent << "time=" << res.timeName << " failed, exitCode=" << res.exitCode << "\n";
                    } else {
                        std::cout << finishedIndent << "time=" << res.timeName << " finished\n";
                    }
                    running.erase(running.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }
            if (!running.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        }

        if (!failedTimes.empty()) {
            std::cerr << "task stopped due to failure. failed time steps:";
            for (const auto& t : failedTimes) std::cerr << ' ' << t;
            std::cerr << "\n";
            return 2;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        usage();
        return 1;
    }
}
