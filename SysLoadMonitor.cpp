// SysLoadMonitor.cpp
#include "SysLoadMonitor.h"

static std::mutex g_cpuEmaMutex;
static double g_cpuEmaValue = -1.0; // negative => not yet initialized
static double g_cpuEmaValuePrev = -1.0; // previous EMA value
static std::chrono::steady_clock::time_point g_cpuEmaLastTs = std::chrono::steady_clock::now();
// Time constant (tau) in seconds: larger => slower smoothing. Tune as needed.
static constexpr double kCpuEmaTimeConstantSec = 1.5;

void SysLoadMonitor::monitorLoop() {
    SYSMON_ALOGI("SysLoadMonitor: Thread started");

    samplerRunning_.store(true);
    while (samplerRunning_.load()) {
        // If paused, wait until restarted or stopped
        {
            std::unique_lock<std::mutex> lk(pauseMutex_);
            pauseCv_.wait(lk, [this] {
                return !samplerPaused_.load() || !samplerRunning_.load();
            });
            if (!samplerRunning_.load())
                break;
        }

        // Perform a detailed /proc/stat read and update samples
        SYSMON_ALOGI("SysLoadMonitor: Periodic CPU load check");

        if (getSysCpuLoad() > kSysloadHighThreshold) {
            SYSMON_ALOGI("SysLoadMonitor: High CPU load detected more than %.1f", kSysloadHighThreshold);
            onValueChanged(g_cpuEmaValuePrev, g_cpuEmaValue);
        }

        // Sleep for up to 1s but wake early if paused/stopped
        {
            std::unique_lock<std::mutex> lk(pauseMutex_);
            pauseCv_.wait_for(lk, samplerInterval_, [this]() {
                return samplerPaused_.load() || !samplerRunning_.load();
            });
        }
    }
    samplerRunning_.store(false);
    SYSMON_ALOGI("SysLoadMonitor: sampler thread exiting");
}

double SysLoadMonitor::getSysCpuLoadOld() {
    // Read /proc/stat once and compute both system-wide and per-CPU utilizations.
    // Backup previous sample
    lastSample_ = currentSample_;
    SYSMON_ALOGD("SysLoadMonitor: backed up last sample: total=%llu idle=%llu",
                 lastSample_.totalTime, lastSample_.idleTime);

    FILE* f = fopen("/proc/stat", "r");
    if (!f) {
        SYSMON_ALOGE("SysLoadMonitor: failed to open /proc/stat");
        return -1.0;
    }

    char line[1024];
    unsigned long long aggTotal = 0;
    unsigned long long aggIdle = 0;
    std::vector<unsigned long long> curTotals;
    std::vector<unsigned long long> curIdles;

    while (fgets(line, sizeof(line), f)) {
        // Aggregate line: starts with "cpu " (cpu followed by space)
        if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u' && line[3] == ' ') {
            // parse numeric tokens after "cpu "
            unsigned long long vals[16] = {0};
            int count = 0;
            char* p = line + 4;
            char* tok = strtok(p, " \t\n");
            while (tok && count < 16) {
                unsigned long long v = 0;
                if (sscanf(tok, "%llu", &v) == 1)
                    vals[count++] = v;
                tok = strtok(nullptr, " \t\n");
            }
            aggTotal = 0;
            aggIdle = 0;
            for (int i = 0; i < count; ++i) {
                aggTotal += vals[i];
                if (i == 3 || i == 4)
                    aggIdle += vals[i]; // idle + iowait if present
            }
            continue;
        }

        // Per-CPU lines: "cpuN ..." where N is digit(s)
        if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u' &&
            std::isdigit(static_cast<unsigned char>(line[3]))) {
            // extract index
            unsigned int idx = 0;
            char* p = line + 3;
            // read index
            if (sscanf(p, "%u", &idx) != 1) {
                SYSMON_ALOGE("SysLoadMonitor: failed to parse cpu index in line: %s", line);
                continue;
            }
            // find the first space after index to get to values
            while (*p && *p != ' ' && *p != '\t')
                ++p;
            // tokenize values
            unsigned long long vals[16] = {0};
            int count = 0;
            char* tok = strtok(p, " \t\n");
            while (tok && count < 16) {
                unsigned long long v = 0;
                if (sscanf(tok, "%llu", &v) == 1)
                    vals[count++] = v;
                tok = strtok(nullptr, " \t\n");
            }
            unsigned long long total = 0;
            unsigned long long idle = 0;
            for (int i = 0; i < count; ++i) {
                total += vals[i];
                if (i == 3 || i == 4)
                    idle += vals[i];
            }
            if (idx >= curTotals.size()) {
                curTotals.resize(idx + 1, 0ULL);
                curIdles.resize(idx + 1, 0ULL);
            }
            curTotals[idx] = total;
            curIdles[idx] = idle;
        }
    }
    fclose(f);

    // Update aggregate sample
    currentSample_.totalTime = aggTotal;
    currentSample_.idleTime = aggIdle;
    SYSMON_ALOGD("SysLoadMonitor: updated sample: total=%llu idle=%llu", aggTotal, aggIdle);

    // Compute system-wide utilization
    unsigned long long deltaTotal = 0;
    unsigned long long deltaIdle = 0;
    if (currentSample_.totalTime >= lastSample_.totalTime) {
        deltaTotal = currentSample_.totalTime - lastSample_.totalTime;
    } else {
        SYSMON_ALOGD("SysLoadMonitor: totalTime wrapped or reset");
        deltaTotal = 0;
    }
    if (currentSample_.idleTime >= lastSample_.idleTime) {
        deltaIdle = currentSample_.idleTime - lastSample_.idleTime;
    } else {
        SYSMON_ALOGD("SysLoadMonitor: idleTime wrapped or reset");
        deltaIdle = 0;
    }

    double systemUtil = -1.0;
    if (deltaTotal > 0) {
        unsigned long long busy = (deltaTotal > deltaIdle) ? (deltaTotal - deltaIdle) : 0ULL;
        systemUtil = (double)busy * 100.0 / (double)deltaTotal;
        SYSMON_ALOGI("SysLoadMonitor: CPU util = %.2f%% (busy=%llu totalDelta=%llu idleDelta=%llu)",
                      systemUtil, busy, deltaTotal, deltaIdle);
    } else {
        SYSMON_ALOGD("SysLoadMonitor: not enough data to compute utilization");
    }

    // Compute per-CPU utilizations using persisted last totals/idle
    static std::vector<unsigned long long> lastTotals;
    static std::vector<unsigned long long> lastIdles;
    static std::mutex perCpuMutex;

    {
        std::lock_guard<std::mutex> lk(perCpuMutex);
        if (lastTotals.size() < curTotals.size()) {
            lastTotals.resize(curTotals.size(), 0ULL);
            lastIdles.resize(curIdles.size(), 0ULL);
        }

        for (size_t cpu = 0; cpu < curTotals.size(); ++cpu) {
            unsigned long long curTotal = curTotals[cpu];
            unsigned long long curIdle = curIdles[cpu];
            unsigned long long prevTotal = lastTotals[cpu];
            unsigned long long prevIdle = lastIdles[cpu];

            unsigned long long dt = 0;
            unsigned long long di = 0;
            if (curTotal >= prevTotal)
                dt = curTotal - prevTotal;
            else {
                SYSMON_ALOGD("SysLoadMonitor: cpu%zu total wrapped or reset", cpu);
                dt = 0;
            }
            if (curIdle >= prevIdle)
                di = curIdle - prevIdle;
            else {
                SYSMON_ALOGD("SysLoadMonitor: cpu%zu idle wrapped or reset", cpu);
                di = 0;
            }

            double util = -1.0;
            if (dt > 0) {
                unsigned long long busy = (dt > di) ? (dt - di) : 0ULL;
                util = (double)busy * 100.0 / (double)dt;
                SYSMON_ALOGI("SysLoadMonitor: cpu%zu util = %.2f%% (busy=%llu totalDelta=%llu idleDelta=%llu)",
                              cpu, util, busy, dt, di);
            } else {
                SYSMON_ALOGD("SysLoadMonitor: not enough data to compute util for cpu%zu", cpu);
            }

            // update last samples for this CPU
            lastTotals[cpu] = curTotal;
            lastIdles[cpu] = curIdle;
        }
    }

    return systemUtil;
}

std::vector<double> SysLoadMonitor::getEachCpuLoad() {
    // Returns a vector of per-CPU utilizations (cpu0..cpuN). Missing/insufficient samples -> -1.0
    static std::vector<unsigned long long> lastTotals;
    static std::vector<unsigned long long> lastIdles;
    static std::mutex perCpuMutex;

    std::vector<double> utilizations;

    FILE* f = fopen("/proc/stat", "r");
    if (!f) {
        SYSMON_ALOGE("SysLoadMonitor: failed to open /proc/stat for per-CPU read");
        return utilizations;
    }

    char line[1024];
    // Temporarily store parsed values by cpu index so we can return a dense vector
    std::vector<unsigned long long> curTotals;
    std::vector<unsigned long long> curIdles;
    size_t maxCpuIndex = 0;

    while (fgets(line, sizeof(line), f)) {
        // only interested in lines that start with "cpu" followed by a digit (cpu0, cpu1, ...)
        if (line[0] != 'c' || line[1] != 'p' || line[2] != 'u')
            continue;
        if (!(line[3] >= '0' && line[3] <= '9'))
            continue; // skip the aggregate "cpu" line

        unsigned int idx = 0;
        unsigned long long vals[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        // Try to parse up to 10 fields after cpuN. sscanf returns number of conversions.
        int conv = sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                          &idx,
                          &vals[0], &vals[1], &vals[2], &vals[3], &vals[4],
                          &vals[5], &vals[6], &vals[7], &vals[8], &vals[9]);
        if (conv < 2) {
            SYSMON_ALOGE("SysLoadMonitor: failed to parse per-cpu line: %s", line);
            continue;
        }
        // conv includes the cpu index, so number of value fields parsed is conv-1
        int valueCount = conv - 1;
        unsigned long long total = 0;
        for (int i = 0; i < valueCount && i < 10; ++i)
            total += vals[i];
        unsigned long long idle = 0;
        // field 4 (index 3) = idle, field 5 (index 4) = iowait if present
        if (valueCount > 3)
            idle += vals[3];
        if (valueCount > 4)
            idle += vals[4];

        if (idx >= curTotals.size()) {
            curTotals.resize(idx + 1, 0);
            curIdles.resize(idx + 1, 0);
        }
        curTotals[idx] = total;
        curIdles[idx] = idle;
        if (idx > maxCpuIndex)
            maxCpuIndex = idx;
    }
    fclose(f);

    if (curTotals.empty()) {
        SYSMON_ALOGD("SysLoadMonitor: no per-CPU lines found in /proc/stat");
        return utilizations;
    }

    // Compute utilizations using previous samples stored in static vectors
    std::lock_guard<std::mutex> lk(perCpuMutex);
    if (lastTotals.size() < curTotals.size()) {
        lastTotals.resize(curTotals.size(), 0);
        lastIdles.resize(curIdles.size(), 0);
    }

    utilizations.assign(curTotals.size(), -1.0);
    for (size_t cpu = 0; cpu < curTotals.size(); ++cpu) {
        unsigned long long curTotal = curTotals[cpu];
        unsigned long long curIdle = curIdles[cpu];
        unsigned long long prevTotal = lastTotals[cpu];
        unsigned long long prevIdle = lastIdles[cpu];

        unsigned long long deltaTotal = 0;
        unsigned long long deltaIdle = 0;
        if (curTotal >= prevTotal)
            deltaTotal = curTotal - prevTotal;
        else {
            SYSMON_ALOGD("SysLoadMonitor: cpu%zu total wrapped or reset", cpu);
            deltaTotal = 0;
        }
        if (curIdle >= prevIdle)
            deltaIdle = curIdle - prevIdle;
        else {
            SYSMON_ALOGD("SysLoadMonitor: cpu%zu idle wrapped or reset", cpu);
            deltaIdle = 0;
        }

        double util = -1.0;
        if (deltaTotal > 0) {
            unsigned long long busy = (deltaTotal > deltaIdle) ? (deltaTotal - deltaIdle) : 0ULL;
            util = (double)busy * 100.0 / (double)deltaTotal;
            SYSMON_ALOGI("SysLoadMonitor: cpu%zu util = %.2f%% (busy=%llu totalDelta=%llu idleDelta=%llu)",
                          cpu, util, busy, deltaTotal, deltaIdle);
        } else {
            SYSMON_ALOGD("SysLoadMonitor: not enough data to compute util for cpu%zu", cpu);
        }

        utilizations[cpu] = util;

        // update last samples for this CPU
        lastTotals[cpu] = curTotal;
        lastIdles[cpu] = curIdle;
    }

    return utilizations;
}

static double applyEmaIrregularSample(double rawPercent) {
    using namespace std::chrono;
    std::lock_guard<std::mutex> lg(g_cpuEmaMutex);
    auto now = steady_clock::now();
    double dt = duration_cast<duration<double>>(now - g_cpuEmaLastTs).count();
    if (dt < 0.0)
        dt = 0.0;

    // If we don't have a raw sample, keep last EMA (if any) but refresh timestamp
    if (rawPercent < 0.0) {
        g_cpuEmaLastTs = now;
        return (g_cpuEmaValue >= 0.0) ? g_cpuEmaValue : -1.0;
    }

    // Initialize EMA if necessary
    if (g_cpuEmaValue < 0.0) {
        g_cpuEmaValue = rawPercent;
        g_cpuEmaLastTs = now;
        return g_cpuEmaValue;
    }

    // alpha = 1 - exp(-dt / tau) for irregular intervals
    double alpha = 1.0 - std::exp(-dt / kCpuEmaTimeConstantSec);
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;

    // update EMA
    g_cpuEmaValuePrev = g_cpuEmaValue;
    g_cpuEmaValue = g_cpuEmaValue * (1.0 - alpha) + rawPercent * alpha;
    g_cpuEmaLastTs = now;
    SYSMON_ALOGI("SysLoadMonitor: EMA alpha=%.4f raw=%.2f newEMA=%.2f", alpha, rawPercent, g_cpuEmaValue);
    return g_cpuEmaValue;
}

double SysLoadMonitor::getLatestSysCpuLoad() const {
    std::lock_guard<std::mutex> lg(g_cpuEmaMutex);
    return g_cpuEmaValue;
}

double SysLoadMonitor::getSysCpuLoad() {
    // Read the aggregate "cpu ..." line from /proc/stat and compute raw utilization
    std::ifstream fs("/proc/stat");
    if (!fs.is_open()) {
        SYSMON_ALOGE("SysLoadMonitor: failed to open /proc/stat");
        return applyEmaIrregularSample(-1.0);
    }

    std::string line;
    if (!std::getline(fs, line)) {
        SYSMON_ALOGE("SysLoadMonitor: failed to read /proc/stat first line");
        return applyEmaIrregularSample(-1.0);
    }

    std::istringstream iss(line);
    std::string label;
    if (!(iss >> label) || label != "cpu") {
        SYSMON_ALOGE("SysLoadMonitor: unexpected /proc/stat first token");
        return applyEmaIrregularSample(-1.0);
    }

    // parse numeric fields; fields: user nice system idle iowait irq softirq steal guest guest_nice ...
    unsigned long long total = 0;
    unsigned long long idle = 0;
    unsigned long long value;
    int fieldIndex = 0;
    while (iss >> value) {
        total += value;
        // fieldIndex: 0=user,1=nice,2=system,3=idle,4=iowait, ...
        if (fieldIndex == 3 || fieldIndex == 4)
            idle += value;
        ++fieldIndex;
    }

    // update stored samples (last/current) used elsewhere
    lastSample_ = currentSample_;
    currentSample_.totalTime = total;
    currentSample_.idleTime = idle;

    // compute raw utilization percentage
    unsigned long long deltaTotal = 0;
    unsigned long long deltaIdle = 0;
    if (currentSample_.totalTime >= lastSample_.totalTime)
        deltaTotal = currentSample_.totalTime - lastSample_.totalTime;
    if (currentSample_.idleTime >= lastSample_.idleTime)
        deltaIdle = currentSample_.idleTime - lastSample_.idleTime;

    double rawUtil = -1.0;
    if (deltaTotal > 0) {
        unsigned long long busy = (deltaTotal > deltaIdle) ? (deltaTotal - deltaIdle) : 0ULL;
        rawUtil = (double)busy * 100.0 / (double)deltaTotal;
    } else {
        SYSMON_ALOGD("SysLoadMonitor: not enough data to compute utilization");
    }

    // Return EMA-smoothed utilization (handles irregular intervals)
    double ema = applyEmaIrregularSample(rawUtil);
    return ema;
}
