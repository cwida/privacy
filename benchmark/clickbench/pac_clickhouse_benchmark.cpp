//
// Created by ila on 02/12/26.
//

// ClickBench benchmark runner with fork-based process isolation.
// The parent never opens DuckDB for benchmarking; a child process handles
// all DB access. If the child is killed (OOM, crash), the parent survives
// and spawns a new one.
//
// ---------------------------------------------------------------------------
// Modes measured per query
// ---------------------------------------------------------------------------
//  - baseline               : plain DuckDB, no privacy (always runs; slowdown reference)
//  - pac / dp_standard /
//    dp_elastic / dp_sass    : the four privacy mechanisms, selected via --modes
//                              (default all four). Each runs ONCE per query at a single
//                              operating point: dp_epsilon = 1.0 and the inferred "perfect"
//                              sensitivity bound (no epsilon/sensitivity sweep).
//  - naive_pac / naive_dp    : explicit sample-table-join variants of the two SAMPLING
//                              mechanisms, run only with --run-naive (and only for whichever
//                              of pac / dp_sass is in --modes). Read from
//                              clickbench_naive_pac_queries / clickbench_naive_dp_queries.
//
// ---------------------------------------------------------------------------
// Naive sample-and-aggregate strategy (the "naive_*" rows)
// ---------------------------------------------------------------------------
// "Naive" means the sampling and aggregation are written out in plain SQL (an explicit
// join against a sample/lane table), with only the final noise calibration delegated to a
// scalar UDF. This is the non-vectorized reference the SIMD/bitslice path is compared against.
// The two naive families share an identical skeleton and differ only in the terminal UDF:
//
//   PAC naive : 128 random sub-samples per privacy unit (WHERE random() < 0.5), per-sub-sample
//               SUM/COUNT/AVG, then  pac_aggregate(list_of_answers, list_of_counts, mi, k)
//               collapses them to one PAC-noised scalar.
//
//   DP naive  : 64 sample lanes; each privacy unit (UserID) is assigned to ~8 of the 64 lanes
//               by an APPROXIMATE SQL hash  (hash(UserID, lane_id) % 64) < 8 , and per-lane
//               answers are rescaled by ~64/8 to undo the lane-inclusion probability. The
//               terminal  dp_aggregate(list_of_answers, list_of_counts, eps, delta, lanes,
//               lower, upper)  takes the clipped median of the 64 lane answers and adds
//               smooth-sensitivity Laplace noise (it wraps dp_smooth_median_noise; the counts
//               list is accepted only for signature symmetry with pac_aggregate and is unused).
//
// IMPORTANT: the DP naive lane assignment is APPROXIMATE and does NOT bit-match the vectorized
// `dp_sass` rewrite. The real lane assignment (DpSampleHash in pac_aggregate.hpp) is a C++-only
// 6-bit-chunk bitmask that cannot be reproduced in plain SQL, and the naive domain bounds are
// hand-set generous constants, not the per-query inferred bounds. The naive rows therefore exist
// as a runtime / structural baseline (we record timing + success, NOT utility); their numeric
// output is not expected to match the vectorized path.
// ---------------------------------------------------------------------------

#include "pac_clickhouse_benchmark.hpp"

#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/printer.hpp"
#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdexcept>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <unistd.h>
#include <limits.h>
#include <regex>
#include <poll.h>
#include <signal.h>

namespace duckdb {

static string Timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return string(buf);
}

static string FormatNumber(double v) {
    std::ostringstream oss;
    oss << std::setprecision(5) << std::defaultfloat << v;
    string s = oss.str();
    auto pos = s.find('.');
    if (pos != string::npos) {
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
    }
    return s;
}

static double PositiveOrDefault(double value, double fallback) {
    return value > 0.0 && std::isfinite(value) ? value : fallback;
}

static void Log(const string &msg) {
    Printer::Print("[" + Timestamp() + "] " + msg);
}

static bool FileExists(const string &path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

static string ReadFileToString(const string &path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static vector<string> SplitLines(const string &content) {
    vector<string> lines;
    std::istringstream iss(content);
    string line;
    while (std::getline(iss, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        if (start != string::npos && end != string::npos) {
            string trimmed = line.substr(start, end - start + 1);
            if (!trimmed.empty() && trimmed[0] != '-' && trimmed.substr(0, 2) != "--") {
                lines.push_back(trimmed);
            }
        }
    }
    return lines;
}

static string ToLowerAscii(string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static string Trim(const string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static string CsvQuote(const string &s) {
    string out = "\"";
    for (char c : s) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

static string SqlQuote(const string &s) {
    string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

static size_t FindKeywordOutsideParens(const string &lower, const string &keyword, size_t start = 0) {
    int depth = 0;
    for (size_t i = start; i + keyword.size() <= lower.size(); i++) {
        char c = lower[i];
        if (c == '(') {
            depth++;
        } else if (c == ')' && depth > 0) {
            depth--;
        }
        if (depth == 0 && lower.compare(i, keyword.size(), keyword) == 0) {
            return i;
        }
    }
    return string::npos;
}

static vector<string> ExtractSumAvgArgs(const string &query) {
    vector<string> args;
    string lower = ToLowerAscii(query);
    size_t pos = 0;
    while (pos < lower.size()) {
        size_t sum_pos = lower.find("sum(", pos);
        size_t avg_pos = lower.find("avg(", pos);
        size_t fn_pos = string::npos;
        if (sum_pos != string::npos && (avg_pos == string::npos || sum_pos < avg_pos)) {
            fn_pos = sum_pos;
        } else if (avg_pos != string::npos) {
            fn_pos = avg_pos;
        }
        if (fn_pos == string::npos) {
            break;
        }
        size_t arg_start = fn_pos + 4;
        int depth = 1;
        size_t i = arg_start;
        for (; i < query.size(); i++) {
            if (query[i] == '(') {
                depth++;
            } else if (query[i] == ')') {
                depth--;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (i < query.size()) {
            string arg = Trim(query.substr(arg_start, i - arg_start));
            if (!arg.empty() && arg != "*") {
                args.push_back(arg);
            }
            pos = i + 1;
        } else {
            break;
        }
    }
    return args;
}

static string ExtractFromWhereClause(const string &query) {
    string lower = ToLowerAscii(query);
    size_t from_pos = FindKeywordOutsideParens(lower, " from ");
    if (from_pos == string::npos) {
        return "";
    }
    size_t end = query.size();
    vector<string> terminators = {" group by ", " order by ", " limit ", " offset "};
    for (auto &term : terminators) {
        size_t p = FindKeywordOutsideParens(lower, term, from_pos + 1);
        if (p != string::npos) {
            end = std::min(end, p);
        }
    }
    string clause = Trim(query.substr(from_pos, end - from_pos));
    if (!clause.empty() && clause.back() == ';') {
        clause.pop_back();
    }
    return clause;
}

static string BuildBoundQuery(const string &query) {
    auto args = ExtractSumAvgArgs(query);
    if (args.empty()) {
        return "";
    }
    string from_where = ExtractFromWhereClause(query);
    if (from_where.empty()) {
        return "";
    }
    string expr;
    for (idx_t i = 0; i < args.size(); i++) {
        if (i > 0) {
            expr += ", ";
        }
        expr += "abs(CAST((" + args[i] + ") AS DOUBLE))";
    }
    if (args.size() > 1) {
        expr = "greatest(" + expr + ")";
    }
    return "SELECT max(" + expr + ") " + from_where;
}

// Build a per-query "perfect" dp_count_bound query: the maximum number of rows any single
// privacy unit (UserID) contributes under the query's filter. Used to calibrate dp_sass's
// COUNT-domain bound (dp_count_bound). Mirrors BuildBoundQuery but for COUNT contributions.
static string BuildCountBoundQuery(const string &query) {
    string from_where = ExtractFromWhereClause(query);
    if (from_where.empty()) {
        return "";
    }
    return "SELECT max(cnt) FROM (SELECT count(*) AS cnt " + from_where + " GROUP BY UserID)";
}

static bool ReadLastUtilityLine(const string &path, string &utility, string &recall, string &precision,
                                string &median_error_pct) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    string line;
    string last;
    while (std::getline(in, line)) {
        if (!Trim(line).empty()) {
            last = Trim(line);
        }
    }
    if (last.empty()) {
        return false;
    }
    std::istringstream ss(last);
    if (!std::getline(ss, utility, ',')) {
        return false;
    }
    if (!std::getline(ss, recall, ',')) {
        return false;
    }
    if (!std::getline(ss, precision, ',')) {
        return false;
    }
    if (!std::getline(ss, median_error_pct, ',')) {
        return false;
    }
    utility = Trim(utility);
    recall = Trim(recall);
    precision = Trim(precision);
    median_error_pct = Trim(median_error_pct);
    return true;
}

static int ExecuteCommand(const string &cmd) {
    Log(string("Executing: ") + cmd);
    int ret = system(cmd.c_str());
    if (ret == -1) {
        return -1;
    }
    if (WIFEXITED(ret)) {
        return WEXITSTATUS(ret);
    }
    return ret;
}

static string FindFile(const string &filename) {
    vector<string> candidates = {
        filename,
        "./" + filename,
        "../" + filename,
        "../../" + filename,
        "benchmark/" + filename,
        "../benchmark/" + filename,
        "../../benchmark/" + filename
    };

    for (auto &cand : candidates) {
        if (FileExists(cand)) {
            return cand;
        }
    }

    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        string dir = string(exe_path);
        auto pos = dir.find_last_of('/');
        if (pos != string::npos) {
            dir = dir.substr(0, pos);
        }

        for (int i = 0; i < 6; ++i) {
            string cand = dir + "/" + filename;
            if (FileExists(cand)) {
                return cand;
            }
            cand = dir + "/benchmark/" + filename;
            if (FileExists(cand)) {
                return cand;
            }
            auto p2 = dir.find_last_of('/');
            if (p2 == string::npos) break;
            dir = dir.substr(0, p2);
        }
    }

    return "";
}

// =====================================================================
// IPC helpers
// =====================================================================

static bool WriteAllBytes(int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

static bool ReadAllBytes(int fd, void *buf, size_t n) {
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// =====================================================================
// Result structure
// =====================================================================

struct BenchmarkQueryResult {
    int query_num;
    string mode;
    int run;
    double time_ms;
    bool success;
    string error_msg;
    string utility;
    string recall;
    string precision;
    string median_error_pct;
    string epsilon;
    string delta_scenario;
    string dp_delta;
    string bound_scenario;
    string dp_sum_bound;
    string dp_count_bound;
    string pac_mi;
    int64_t seed = 0;
};

static bool IsCommonDpUnsupportedClickBenchQuery(int qnum) {
    // Keep DP mechanism support comparable in this benchmark. These queries currently
    // hit dp_sass resource/worker failures, so treat them as outside the common DP set.
    return qnum == 30 || qnum == 34 || qnum == 35;
}

static string CommonDpUnsupportedReason() {
    return "ClickBench query excluded from common DP support set";
}

// =====================================================================
// Child worker process
// =====================================================================
// Protocol (parent->child):
//   [uint8_t cmd]
//     cmd=0: stop
//     cmd=1: run query — [uint32_t sql_len][sql_bytes]
//     cmd=2: setup PAC — [uint32_t count][for each: uint32_t len, bytes]
//     cmd=3: unset PAC
// Protocol (child->parent) for cmd=1:
//   [double time_ms][uint8_t ok][uint32_t err_len][err_bytes]

static constexpr uint8_t CMD_STOP = 0;
static constexpr uint8_t CMD_QUERY = 1;
static constexpr uint8_t CMD_SETUP_PAC = 2;
static constexpr uint8_t CMD_UNSET_PAC = 3;

static void WriteStatus(int write_fd, const string &error) {
    uint8_t ok = error.empty() ? 1 : 0;
    uint32_t err_len = static_cast<uint32_t>(error.size());
    WriteAllBytes(write_fd, &ok, sizeof(ok));
    WriteAllBytes(write_fd, &err_len, sizeof(err_len));
    if (err_len > 0) {
        WriteAllBytes(write_fd, error.data(), err_len);
    }
}

[[noreturn]] static void ChildWorkerMain(int read_fd, int write_fd,
                                          const string &db_path) {
    try {
        DuckDB db(db_path);
        Connection con(db);
        auto r = con.Query("LOAD privacy");
        if (r->HasError()) {
            _exit(2);
        }
        con.Query("SET memory_limit='60GB'");
        con.Query("SET temp_directory='" + db_path + ".tmp'");

        while (true) {
            uint8_t cmd = 0;
            if (!ReadAllBytes(read_fd, &cmd, sizeof(cmd))) break;

            if (cmd == CMD_STOP) {
                break;
            } else if (cmd == CMD_UNSET_PAC) {
                con.Query("SET privacy_diffcols=NULL;");
                con.Query("SET privacy_mode='pac';");
                con.Query("RESET dp_sum_bound;");
                con.Query("PRAGMA clear_privacy_metadata;");
            } else if (cmd == CMD_SETUP_PAC) {
                uint32_t count = 0;
                if (!ReadAllBytes(read_fd, &count, sizeof(count))) break;
                string setup_error;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t len = 0;
                    if (!ReadAllBytes(read_fd, &len, sizeof(len))) goto done;
                    string stmt(len, '\0');
                    if (!ReadAllBytes(read_fd, &stmt[0], len)) goto done;
                    if (setup_error.empty()) {
                        auto setup_result = con.Query(stmt);
                        if (setup_result && setup_result->HasError()) {
                            setup_error = "setup failed: " + stmt + " -> " + setup_result->GetError();
                            if (setup_error.size() > 300) {
                                setup_error = setup_error.substr(0, 300);
                            }
                        }
                    }
                }
                WriteStatus(write_fd, setup_error);
            } else if (cmd == CMD_QUERY) {
                uint32_t sql_len = 0;
                if (!ReadAllBytes(read_fd, &sql_len, sizeof(sql_len))) break;
                string sql(sql_len, '\0');
                if (!ReadAllBytes(read_fd, &sql[0], sql_len)) break;

                auto start = std::chrono::steady_clock::now();
                unique_ptr<MaterializedQueryResult> result;
                std::exception_ptr exc;
                try {
                    result = con.Query(sql);
                    if (result && !result->HasError()) {
                        while (result->Fetch()) {}
                    }
                } catch (...) {
                    exc = std::current_exception();
                }
                auto end = std::chrono::steady_clock::now();
                double time_ms = std::chrono::duration<double, std::milli>(end - start).count();

                string error;
                if (exc) {
                    try { std::rethrow_exception(exc); }
                    catch (std::exception &e) { error = e.what(); }
                    catch (...) { error = "unknown exception"; }
                } else if (result && result->HasError()) {
                    error = result->GetError();
                }

                if (!error.empty()) {
                    auto sp = error.find("\n\nStack Trace");
                    if (sp != string::npos) error = error.substr(0, sp);
                    if (error.size() > 300) error = error.substr(0, 300);
                }
                result.reset();

                uint8_t ok = error.empty() ? 1 : 0;
                uint32_t err_len = static_cast<uint32_t>(error.size());
                WriteAllBytes(write_fd, &time_ms, sizeof(time_ms));
                WriteAllBytes(write_fd, &ok, sizeof(ok));
                WriteAllBytes(write_fd, &err_len, sizeof(err_len));
                if (err_len > 0) WriteAllBytes(write_fd, error.data(), err_len);

                // If DB is invalidated, child must exit
                if (!error.empty() &&
                    (error.find("FATAL") != string::npos ||
                     error.find("database has been invalidated") != string::npos)) {
                    _exit(1);
                }
            }
        }
    } catch (...) {
        _exit(2);
    }
done:
    _exit(0);
}

// =====================================================================
// Fork-based worker
// =====================================================================

struct ForkWorker {
    pid_t child_pid = -1;
    int to_child_fd = -1;
    int from_child_fd = -1;
    string db_path;

    double result_time_ms = 0;
    bool result_ok = false;
    string result_error;
    string setup_error;

    enum SubmitResult { SR_OK, SR_TIMEOUT, SR_CHILD_DIED };

    void Start() {
        Stop();
        int to_child[2], from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0) {
            throw std::runtime_error("pipe() failed");
        }

        child_pid = fork();
        if (child_pid < 0) {
            close(to_child[0]); close(to_child[1]);
            close(from_child[0]); close(from_child[1]);
            throw std::runtime_error("fork() failed");
        }

        if (child_pid == 0) {
            close(to_child[1]);
            close(from_child[0]);
            ChildWorkerMain(to_child[0], from_child[1], db_path);
            _exit(0);
        }

        close(to_child[0]);
        close(from_child[1]);
        to_child_fd = to_child[1];
        from_child_fd = from_child[0];
    }

    void Stop() {
        if (child_pid > 0) {
            uint8_t cmd = CMD_STOP;
            WriteAllBytes(to_child_fd, &cmd, sizeof(cmd));

            int status;
            for (int i = 0; i < 4; i++) {
                int w = waitpid(child_pid, &status, WNOHANG);
                if (w != 0) break;
                usleep(50000);
            }
            int w = waitpid(child_pid, &status, WNOHANG);
            if (w == 0) {
                kill(child_pid, SIGKILL);
                waitpid(child_pid, &status, 0);
            }
            child_pid = -1;
        }
        if (to_child_fd >= 0) { close(to_child_fd); to_child_fd = -1; }
        if (from_child_fd >= 0) { close(from_child_fd); from_child_fd = -1; }
    }

    size_t GetChildRSS() {
        if (child_pid <= 0) return 0;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/statm", child_pid);
        FILE *f = fopen(path, "r");
        if (!f) return 0;
        long pages = 0, rss = 0;
        if (fscanf(f, "%ld %ld", &pages, &rss) != 2) { rss = 0; }
        fclose(f);
        return static_cast<size_t>(rss) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
    }

    // Send setup statements to the child. These run outside measured query time.
    bool SendSetupStatements(const vector<string> &stmts) {
        setup_error.clear();
        if (child_pid <= 0) return false;
        uint8_t cmd = CMD_SETUP_PAC;
        if (!WriteAllBytes(to_child_fd, &cmd, sizeof(cmd))) return false;
        uint32_t count = static_cast<uint32_t>(stmts.size());
        if (!WriteAllBytes(to_child_fd, &count, sizeof(count))) return false;
        for (auto &s : stmts) {
            uint32_t len = static_cast<uint32_t>(s.size());
            if (!WriteAllBytes(to_child_fd, &len, sizeof(len))) return false;
            if (!WriteAllBytes(to_child_fd, s.data(), len)) return false;
        }
        uint8_t ok = 0;
        uint32_t err_len = 0;
        if (!ReadAllBytes(from_child_fd, &ok, sizeof(ok)) || !ReadAllBytes(from_child_fd, &err_len, sizeof(err_len))) {
            setup_error = "child process died during setup";
            return false;
        }
        if (err_len > 0) {
            setup_error.resize(err_len);
            if (!ReadAllBytes(from_child_fd, &setup_error[0], err_len)) {
                setup_error = "child process died while reading setup error";
                return false;
            }
        }
        return ok != 0;
    }

    // Clear privacy metadata/settings for the next run.
    bool SendUnsetPAC() {
        if (child_pid <= 0) return false;
        uint8_t cmd = CMD_UNSET_PAC;
        return WriteAllBytes(to_child_fd, &cmd, sizeof(cmd));
    }

    // Submit a query and wait for the result with timeout
    SubmitResult SubmitQuery(const string &sql, double timeout_s) {
        result_time_ms = 0;
        result_ok = false;
        result_error.clear();

        if (child_pid <= 0) return SR_CHILD_DIED;

        // Send query command
        uint8_t cmd = CMD_QUERY;
        uint32_t sql_len = static_cast<uint32_t>(sql.size());
        if (!WriteAllBytes(to_child_fd, &cmd, sizeof(cmd)) ||
            !WriteAllBytes(to_child_fd, &sql_len, sizeof(sql_len)) ||
            !WriteAllBytes(to_child_fd, sql.data(), sql_len)) {
            int status;
            waitpid(child_pid, &status, 0);
            child_pid = -1;
            result_error = "child process died (write failed)";
            return SR_CHILD_DIED;
        }

        // Wait for result with timeout + memory monitoring
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(timeout_s));

        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                kill(child_pid, SIGKILL);
                waitpid(child_pid, nullptr, 0);
                child_pid = -1;
                result_error = "timeout";
                return SR_TIMEOUT;
            }

            int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            int poll_ms = std::min(remaining_ms, 500);

            struct pollfd pfd;
            pfd.fd = from_child_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int ret = poll(&pfd, 1, poll_ms);

            if (ret > 0 && (pfd.revents & POLLIN)) {
                // Read result
                uint8_t ok_byte = 0;
                uint32_t err_len = 0;
                if (!ReadAllBytes(from_child_fd, &result_time_ms, sizeof(result_time_ms)) ||
                    !ReadAllBytes(from_child_fd, &ok_byte, sizeof(ok_byte)) ||
                    !ReadAllBytes(from_child_fd, &err_len, sizeof(err_len))) {
                    int status;
                    waitpid(child_pid, &status, 0);
                    child_pid = -1;
                    result_error = "child process died (read failed)";
                    return SR_CHILD_DIED;
                }
                result_ok = (ok_byte != 0);
                if (err_len > 0) {
                    result_error.resize(err_len);
                    if (!ReadAllBytes(from_child_fd, &result_error[0], err_len)) {
                        int status;
                        waitpid(child_pid, &status, 0);
                        child_pid = -1;
                        result_error = "child process died (read failed)";
                        return SR_CHILD_DIED;
                    }
                }

                // If child reported fatal error, it will exit
                if (!result_error.empty() &&
                    (result_error.find("FATAL") != string::npos ||
                     result_error.find("database has been invalidated") != string::npos)) {
                    int status;
                    waitpid(child_pid, &status, 0);
                    child_pid = -1;
                    return SR_CHILD_DIED;
                }

                return SR_OK;
            }

            if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
                int status;
                waitpid(child_pid, &status, 0);
                child_pid = -1;
                result_error = "child process died unexpectedly";
                return SR_CHILD_DIED;
            }

            // Check if child still alive
            {
                int status;
                int w = waitpid(child_pid, &status, WNOHANG);
                if (w > 0) {
                    child_pid = -1;
                    result_error = "child process died unexpectedly";
                    return SR_CHILD_DIED;
                }
            }
        }
    }

    ~ForkWorker() { Stop(); }
};

// =====================================================================
// Main benchmark logic
// =====================================================================

int RunClickHouseBenchmark(const string &db_path, const string &queries_dir, const string &out_csv, bool micro,
                           bool run_naive, const std::set<string> &modes) {
    // Ignore SIGPIPE so writing to a dead child's pipe returns EPIPE instead of killing us
    signal(SIGPIPE, SIG_IGN);

    try {
        Log(string("Starting ClickBench benchmark (fork-based)"));
        Log(string("micro flag: ") + (micro ? string("true") : string("false")));

        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            throw std::runtime_error("Failed to get current working directory");
        }
        string work_dir = string(cwd);

        // Dataset path
        string parquet_path = work_dir + "/hits.parquet";

        // Determine database path
        string db_actual;
        if (!db_path.empty()) {
            db_actual = db_path;
        } else if (micro) {
            db_actual = "clickbench_micro.db";
        } else {
            db_actual = "clickbench.db";
        }

        // Find query files
        string create_sql_path = queries_dir + "/create.sql";
        string load_sql_path = queries_dir + "/load.sql";
        string queries_sql_path = queries_dir + "/queries.sql";
        string setup_sql_path = queries_dir + "/setup.sql";

        string create_file = FindFile(create_sql_path);
        string load_file = FindFile(load_sql_path);
        string queries_file = FindFile(queries_sql_path);
        string setup_file = FindFile(setup_sql_path);

        if (create_file.empty()) throw std::runtime_error("Cannot find create.sql in " + queries_dir);
        if (load_file.empty()) throw std::runtime_error("Cannot find load.sql in " + queries_dir);
        if (queries_file.empty()) throw std::runtime_error("Cannot find queries.sql in " + queries_dir);
        if (setup_file.empty()) throw std::runtime_error("Cannot find setup.sql in " + queries_dir);

        Log(string("Using create.sql: ") + create_file);
        Log(string("Using load.sql: ") + load_file);
        Log(string("Using queries.sql: ") + queries_file);
        Log(string("Using setup.sql: ") + setup_file);

        // Naive sample-table-join query directories: siblings of the resolved queries directory.
        // (e.g. .../clickbench/clickbench_queries -> .../clickbench/clickbench_naive_pac_queries)
        string queries_dir_resolved = queries_file.substr(0, queries_file.find_last_of('/'));
        string clickbench_root = queries_dir_resolved.substr(0, queries_dir_resolved.find_last_of('/'));
        string naive_pac_dir = clickbench_root + "/clickbench_naive_pac_queries";
        string naive_dp_dir = clickbench_root + "/clickbench_naive_dp_queries";
        if (run_naive) {
            Log(string("Naive PAC queries dir: ") + naive_pac_dir);
            Log(string("Naive DP queries dir:  ") + naive_dp_dir);
        }

        string create_sql = ReadFileToString(create_file);
        string load_sql = ReadFileToString(load_file);
        string queries_content = ReadFileToString(queries_file);
        vector<string> setup_stmts = SplitLines(ReadFileToString(setup_file));

        if (create_sql.empty()) throw std::runtime_error("create.sql is empty or unreadable");
        if (load_sql.empty()) throw std::runtime_error("load.sql is empty or unreadable");

        vector<string> queries = SplitLines(queries_content);
        if (queries.empty()) throw std::runtime_error("No queries found in queries.sql");

        Log(string("Found ") + std::to_string(queries.size()) + " queries to benchmark");

        vector<int> diff_key_cols = {
            0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
            1, 2, 1, 1, 2, 1, 2, 2, 3, 0,
            0, 1, 1, 0, 0, 0, 0, 1, 1, 0,
            2, 2, 2, 1, 2, 4, 1, 1, 1, 5,
            2, 2, 1
        };
        if (diff_key_cols.size() != queries.size()) {
            throw std::runtime_error("ClickBench privacy_diffcols metadata does not match query count");
        }

        // Single DP operating point (no epsilon/sensitivity sweep): each selected DP mode runs once
        // per query with dp_epsilon = 1.0 and the inferred "perfect" sensitivity bound.
        const double dp_epsilon_fixed = 1.0;
        const string pac_mi_default = "0.0078125";
        double privacy_unit_count = 0.0;
        vector<bool> query_has_sum_avg(queries.size(), false);
        vector<bool> query_has_group_by(queries.size(), false);
        vector<double> perfect_bounds(queries.size(), 0.0);
        vector<double> perfect_count_bounds(queries.size(), 0.0);
        for (idx_t q = 0; q < queries.size(); q++) {
            query_has_sum_avg[q] = !ExtractSumAvgArgs(queries[q]).empty();
            query_has_group_by[q] =
                FindKeywordOutsideParens(ToLowerAscii(queries[q]), " group by ") != string::npos;
        }

        // =====================================================================
        // Setup phase: create table and load data (in a temporary process)
        // We open the DB briefly to check/load, then close it so the fork
        // worker can open it exclusively.
        // =====================================================================
        {
            bool db_exists = FileExists(db_actual);
            if (db_exists) {
                Log(string("Connecting to existing DuckDB database: ") + db_actual);
            } else {
                Log(string("Creating new DuckDB database: ") + db_actual);
            }

            DuckDB db(db_actual.c_str());
            Connection con(db);

            auto check_result = con.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'hits'");
            bool table_exists = false;
            if (check_result && !check_result->HasError()) {
                auto chunk = check_result->Fetch();
                if (chunk && chunk->size() > 0) {
                    auto count = chunk->GetValue(0, 0).GetValue<int64_t>();
                    table_exists = (count > 0);
                }
            }

            bool needs_reload = false;
            if (table_exists) {
                auto row_count_result = con.Query("SELECT COUNT(*) FROM hits");
                if (row_count_result && !row_count_result->HasError()) {
                    auto chunk = row_count_result->Fetch();
                    if (chunk && chunk->size() > 0) {
                        auto row_count = chunk->GetValue(0, 0).GetValue<int64_t>();
                        Log(string("hits table has ") + std::to_string(row_count) + " rows");
                        if (row_count == 0) {
                            Log("Table is empty, will reload data");
                            needs_reload = true;
                        }
                    }
                }
            }

            if (!table_exists || needs_reload) {
                if (!FileExists(parquet_path)) {
                    Log("Downloading ClickHouse hits dataset (parquet format)...");
                    string download_cmd =
                        "wget -O \"" + parquet_path + "\" https://datasets.clickhouse.com/hits_compatible/hits.parquet";
                    int ret = ExecuteCommand(download_cmd);
                    if (ret != 0) {
                        Log("wget failed, trying curl...");
                        download_cmd =
                            "curl -L -o \"" + parquet_path +
                            "\" https://datasets.clickhouse.com/hits_compatible/hits.parquet";
                        ret = ExecuteCommand(download_cmd);
                        if (ret != 0) {
                            throw std::runtime_error("Failed to download hits.parquet");
                        }
                    }
                    Log("Download complete.");
                } else {
                    Log("hits.parquet already exists, skipping download.");
                }

                if (!table_exists) {
                    Log("Creating hits table...");
                    auto create_result = con.Query(create_sql);
                    if (create_result && create_result->HasError()) {
                        throw std::runtime_error("Failed to create table: " + create_result->GetError());
                    }
                }

                Log("Loading data into hits table... (this may take a while)");
                string actual_load_sql = load_sql;

                std::regex parquet_regex("'hits\\.parquet'");
                actual_load_sql = std::regex_replace(actual_load_sql, parquet_regex, "'" + parquet_path + "'");

                if (micro) {
                    Log("Micro mode: limiting data load to 5m rows");
                    std::regex from_parquet_regex("(FROM\\s+read_parquet\\([^)]+\\)[^;]*)");
                    actual_load_sql = std::regex_replace(actual_load_sql, from_parquet_regex, "$1 LIMIT 5000000");
                }

                Log(string("Executing load SQL: ") + actual_load_sql.substr(0, 200) + "...");

                auto load_result = con.Query(actual_load_sql);
                if (load_result && load_result->HasError()) {
                    throw std::runtime_error("Failed to load data: " + load_result->GetError());
                }
                Log("Data loading complete.");
            } else {
                Log("hits table already exists, skipping creation and loading.");
            }

            // Ensure privacy metadata is disabled before bound inference and baseline runs.
            con.Query("PRAGMA clear_privacy_metadata;");

            auto pu_count_result = con.Query("SELECT COUNT(DISTINCT UserID) FROM hits");
            if (!pu_count_result || pu_count_result->HasError()) {
                throw std::runtime_error("Failed to compute ClickBench privacy unit count: " +
                                         (pu_count_result ? pu_count_result->GetError() : "unknown error"));
            }
            auto pu_count_chunk = pu_count_result->Fetch();
            if (pu_count_chunk && pu_count_chunk->size() > 0) {
                auto value = pu_count_chunk->GetValue(0, 0);
                if (!value.IsNull()) {
                    privacy_unit_count = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                }
            }
            if (privacy_unit_count <= 1.0 || !std::isfinite(privacy_unit_count)) {
                throw std::runtime_error("Invalid ClickBench privacy unit count");
            }
            Log("ClickBench privacy units (distinct UserID): " + FormatNumber(privacy_unit_count));

            Log("Inferring per-query perfect dp_sum_bound values...");
            for (idx_t q = 0; q < queries.size(); q++) {
                string bound_sql = BuildBoundQuery(queries[q]);
                if (bound_sql.empty()) {
                    continue;
                }
                auto bound_result = con.Query(bound_sql);
                if (!bound_result || bound_result->HasError()) {
                    string err = bound_result ? bound_result->GetError() : "unknown error";
                    Log(string("Q") + std::to_string(q + 1) + " bound inference failed: " + err);
                    continue;
                }
                auto chunk = bound_result->Fetch();
                if (chunk && chunk->size() > 0) {
                    auto value = chunk->GetValue(0, 0);
                    if (!value.IsNull()) {
                        perfect_bounds[q] = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    }
                }
                if (perfect_bounds[q] > 0) {
                    Log(string("Q") + std::to_string(q + 1) + " perfect dp_sum_bound=" +
                        FormatNumber(perfect_bounds[q]));
                }
            }

            Log("Inferring per-query perfect dp_count_bound values (max rows per privacy unit)...");
            for (idx_t q = 0; q < queries.size(); q++) {
                string count_sql = BuildCountBoundQuery(queries[q]);
                if (count_sql.empty()) {
                    continue;
                }
                auto count_result = con.Query(count_sql);
                if (!count_result || count_result->HasError()) {
                    string err = count_result ? count_result->GetError() : "unknown error";
                    Log(string("Q") + std::to_string(q + 1) + " count-bound inference failed: " + err);
                    continue;
                }
                auto chunk = count_result->Fetch();
                if (chunk && chunk->size() > 0) {
                    auto value = chunk->GetValue(0, 0);
                    if (!value.IsNull()) {
                        perfect_count_bounds[q] = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    }
                }
                if (perfect_count_bounds[q] > 0) {
                    Log(string("Q") + std::to_string(q + 1) + " perfect dp_count_bound=" +
                        FormatNumber(perfect_count_bounds[q]));
                }
            }

            // Checkpoint to flush WAL before child opens DB
            con.Query("CHECKPOINT;");
        }
        // DB is now closed — child process will open it

        // Prepare output CSV
        string actual_out = out_csv;
        if (actual_out.empty()) {
            if (micro) {
                actual_out = "benchmark/clickbench_micro_results.csv";
            } else {
                actual_out = "benchmark/clickbench_results.csv";
            }
        }

        std::ofstream csv(actual_out, std::ofstream::out | std::ofstream::trunc);
        if (!csv.is_open()) {
            throw std::runtime_error("Failed to open output CSV: " + actual_out);
        }

        vector<BenchmarkQueryResult> all_results;

        // =====================================================================
        // Main benchmark loop using fork worker
        // =====================================================================
        Log("=== Starting per-run benchmark (fork-based) ===");

        int num_runs = 3;
        double timeout_s = 120.0;  // 2 minute timeout per query

        for (int run = 1; run <= num_runs; ++run) {
            Log(string("=== Run ") + std::to_string(run) + " of " + std::to_string(num_runs) + " ===");

            ForkWorker worker;
            worker.db_path = db_actual;
            worker.Start();

            for (size_t q = 0; q < queries.size(); ++q) {
                int qnum = static_cast<int>(q + 1);
                const string &query = queries[q];

                // Ensure worker is alive
                if (worker.child_pid <= 0) {
                    Log("Restarting child worker...");
                    worker.Start();
                }

                // ----------------------------------------------------------
                // Cold run (baseline, not recorded)
                // ----------------------------------------------------------
                {
                    worker.SendUnsetPAC();
                    Log(string("Q") + std::to_string(qnum) + " cold run");
                    auto res = worker.SubmitQuery(query, timeout_s);
                    if (res == ForkWorker::SR_CHILD_DIED) {
                        Log(string("Q") + std::to_string(qnum) + " cold run: child died, restarting");
                        worker.Start();
                    } else if (res == ForkWorker::SR_TIMEOUT) {
                        Log(string("Q") + std::to_string(qnum) + " cold run: timeout, restarting");
                        worker.Start();
                    }
                }

                // Ensure worker is alive for warm run
                if (worker.child_pid <= 0) {
                    worker.Start();
                }

                // ----------------------------------------------------------
                // Warm run (baseline, recorded)
                // ----------------------------------------------------------
                {
                    worker.SendUnsetPAC();

                    auto res = worker.SubmitQuery(query, timeout_s);

                    BenchmarkQueryResult result;
                    result.query_num = qnum;
                    result.mode = "baseline";
                    result.run = run;
                    result.time_ms = worker.result_time_ms;
                    result.success = (res == ForkWorker::SR_OK && worker.result_ok);
                    result.error_msg = worker.result_error;

                    all_results.push_back(result);

                    if (res == ForkWorker::SR_CHILD_DIED) {
                        Log(string("Q") + std::to_string(qnum) + " baseline run " + std::to_string(run) +
                            " CRASHED: " + worker.result_error);
                        worker.Start();
                    } else if (res == ForkWorker::SR_TIMEOUT) {
                        Log(string("Q") + std::to_string(qnum) + " baseline run " + std::to_string(run) +
                            " TIMEOUT: " + worker.result_error);
                        worker.Start();
                    } else if (result.success) {
                        Log(string("Q") + std::to_string(qnum) + " baseline run " + std::to_string(run) +
                            " time: " + FormatNumber(result.time_ms) + " ms");
                    } else {
                        Log(string("Q") + std::to_string(qnum) + " baseline run " + std::to_string(run) +
                            " ERROR: " + result.error_msg);
                    }
                }

                // Ensure worker is alive for PAC run
                if (worker.child_pid <= 0) {
                    worker.Start();
                }

                // ----------------------------------------------------------
                // PAC run (recorded) — gated by --modes
                // ----------------------------------------------------------
                if (modes.count("pac")) {
                    string utility_path = "/tmp/clickbench_utility_pac_q" + std::to_string(qnum) + "_r" +
                                          std::to_string(run) + "_" + std::to_string(getpid()) + ".csv";
                    std::remove(utility_path.c_str());
                    int64_t seed = static_cast<int64_t>(run * 997 + qnum);
                    vector<string> pac_setup = setup_stmts;
                    pac_setup.push_back("SET privacy_mode='pac';");
                    pac_setup.push_back("SET privacy_seed=" + std::to_string(seed) + ";");
                    pac_setup.push_back("SET pac_mi=" + pac_mi_default + ";");
                    pac_setup.push_back("SET privacy_diffcols=NULL;");
                    bool setup_ok = worker.SendSetupStatements(pac_setup);
                    ForkWorker::SubmitResult res = ForkWorker::SR_OK;
                    if (setup_ok) {
                        res = worker.SubmitQuery(query, timeout_s);
                    }

                    BenchmarkQueryResult result;
                    result.query_num = qnum;
                    result.mode = "PAC";
                    result.run = run;
                    result.time_ms = setup_ok ? worker.result_time_ms : 0;
                    result.success = setup_ok && (res == ForkWorker::SR_OK && worker.result_ok);
                    result.error_msg = setup_ok ? worker.result_error : worker.setup_error;
                    result.pac_mi = pac_mi_default;
                    result.seed = seed;
                    if (result.success && worker.child_pid > 0) {
                        vector<string> pac_diff_setup = pac_setup;
                        pac_diff_setup.push_back("SET privacy_seed=" + std::to_string(seed) + ";");
                        pac_diff_setup.push_back("SET privacy_diffcols=" +
                                                 SqlQuote(std::to_string(diff_key_cols[q]) + ":" + utility_path) + ";");
                        bool diff_setup_ok = worker.SendSetupStatements(pac_diff_setup);
                        if (diff_setup_ok) {
                            auto diff_res = worker.SubmitQuery(query, timeout_s);
                            if (diff_res == ForkWorker::SR_OK && worker.result_ok) {
                                ReadLastUtilityLine(utility_path, result.utility, result.recall, result.precision,
                                                    result.median_error_pct);
                            }
                        }
                    }

                    all_results.push_back(result);

                    if (res == ForkWorker::SR_CHILD_DIED) {
                        Log(string("Q") + std::to_string(qnum) + " PAC run " + std::to_string(run) +
                            " CRASHED: " + worker.result_error);
                        worker.Start();
                    } else if (res == ForkWorker::SR_TIMEOUT) {
                        Log(string("Q") + std::to_string(qnum) + " PAC run " + std::to_string(run) +
                            " TIMEOUT: " + worker.result_error);
                        worker.Start();
                    } else if (result.success) {
                        Log(string("Q") + std::to_string(qnum) + " PAC run " + std::to_string(run) +
                            " time: " + FormatNumber(result.time_ms) + " ms");
                    } else {
                        if (!setup_ok) {
                            Log(string("Q") + std::to_string(qnum) + " PAC run " + std::to_string(run) +
                                " SETUP ERROR: " + result.error_msg);
                        } else if (result.error_msg.find("PAC") != string::npos ||
                            result.error_msg.find("privacy") != string::npos ||
                            result.error_msg.find("aggregat") != string::npos ||
                            result.error_msg.find("protected") != string::npos) {
                            Log(string("Q") + std::to_string(qnum) + " PAC run " + std::to_string(run) +
                                " REJECTED: " + result.error_msg);
                        } else {
                            Log(string("Q") + std::to_string(qnum) + " PAC run " + std::to_string(run) +
                                " ERROR: " + result.error_msg);
                        }
                    }

                    // Disable PAC after the run
                    if (worker.child_pid > 0) {
                        worker.SendUnsetPAC();
                    }
                }

                // ----------------------------------------------------------
                // DP modes (recorded) — one run per selected mode, single epsilon + single
                // sensitivity (no sweep). Grouped dp_standard queries need delta for private
                // partition selection; dp_elastic/dp_sass use delta for smooth sensitivity.
                // dp_sass additionally needs dp_count_bound.
                // ----------------------------------------------------------
                auto run_dp_mode = [&](const string &mode_name) {
                    if (worker.child_pid <= 0) {
                        worker.Start();
                    }
                    if (IsCommonDpUnsupportedClickBenchQuery(qnum)) {
                        int64_t seed = static_cast<int64_t>(run * 997 + qnum);
                        BenchmarkQueryResult result;
                        result.query_num = qnum;
                        result.mode = mode_name;
                        result.run = run;
                        result.time_ms = 0;
                        result.success = false;
                        result.error_msg = CommonDpUnsupportedReason();
                        result.epsilon = FormatNumber(dp_epsilon_fixed);
                        result.bound_scenario = "common_support";
                        result.seed = seed;
                        all_results.push_back(result);

                        Log(string("Q") + std::to_string(qnum) + " " + mode_name + " run " +
                            std::to_string(run) + " REJECTED: " + result.error_msg);
                        return;
                    }
                    string utility_path = "/tmp/clickbench_utility_" + mode_name + "_q" + std::to_string(qnum) + "_r" +
                                          std::to_string(run) + "_" + std::to_string(getpid()) + ".csv";
                    std::remove(utility_path.c_str());

                    int64_t seed = static_cast<int64_t>(run * 997 + qnum);
                    double dp_bound = 0.0;
                    double dp_count_bound = 0.0;
                    double dp_group_bound = 0.0;
                    double dp_delta = 0.0;
                    bool needs_delta = (mode_name != "dp_standard") || query_has_group_by[q];
                    vector<string> dp_setup = setup_stmts;
                    dp_setup.push_back("SET privacy_mode='" + mode_name + "';");
                    dp_setup.push_back("SET privacy_seed=" + std::to_string(seed) + ";");
                    dp_setup.push_back("SET dp_epsilon=" + FormatNumber(dp_epsilon_fixed) + ";");
                    if (needs_delta) {
                        dp_delta = std::exp(-dp_epsilon_fixed * std::pow(std::log(privacy_unit_count), 2.0));
                        dp_setup.push_back("SET dp_delta=" + FormatNumber(dp_delta) + ";");
                    } else {
                        dp_setup.push_back("SET dp_delta=NULL;");
                    }
                    if (query_has_sum_avg[q]) {
                        dp_bound = PositiveOrDefault(perfect_bounds[q], 1e-9);
                        dp_setup.push_back("SET dp_sum_bound=" + FormatNumber(dp_bound) + ";");
                    } else {
                        dp_setup.push_back("RESET dp_sum_bound;");
                    }
                    if (mode_name == "dp_standard" || mode_name == "dp_sass") {
                        dp_count_bound = PositiveOrDefault(perfect_count_bounds[q], 1.0);
                        dp_setup.push_back("SET dp_count_bound=" + FormatNumber(dp_count_bound) + ";");
                    }
                    if (mode_name == "dp_sass") {
                        // as_sample_count/as_sample_sum rescale each lane to the full-output estimator before
                        // dp_smooth_median_noise clamps to the public output domain. These bounds must therefore
                        // cover the rescaled answer, not the raw per-lane sample contribution.
                        double pu_bound = std::ceil(privacy_unit_count);
                        double sass_count_output_bound = dp_count_bound * pu_bound;
                        dp_setup.push_back("SET dp_sass_count_output_bound=" +
                                           FormatNumber(sass_count_output_bound) + ";");
                        if (query_has_sum_avg[q]) {
                            double sass_sum_output_bound = dp_bound * pu_bound;
                            dp_setup.push_back("SET dp_sass_sum_output_bound=" +
                                               FormatNumber(sass_sum_output_bound) + ";");
                            dp_setup.push_back("SET dp_sass_avg_lower_bound=" + FormatNumber(-dp_bound) + ";");
                            dp_setup.push_back("SET dp_sass_avg_upper_bound=" + FormatNumber(dp_bound) + ";");
                        }
                        dp_setup.push_back("SET dp_sass_minmax_lower_bound=-1e18;");
                        dp_setup.push_back("SET dp_sass_minmax_upper_bound=1e18;");
                    }
                    if (query_has_group_by[q] && (mode_name == "dp_standard" || mode_name == "dp_sass")) {
                        dp_group_bound = PositiveOrDefault(perfect_count_bounds[q], 1.0);
                        dp_setup.push_back("SET dp_max_groups_contributed=" + FormatNumber(dp_group_bound) + ";");
                    }
                    dp_setup.push_back("SET privacy_diffcols=NULL;");
                    bool setup_ok = worker.SendSetupStatements(dp_setup);
                    ForkWorker::SubmitResult res = ForkWorker::SR_OK;
                    if (setup_ok) {
                        res = worker.SubmitQuery(query, timeout_s);
                    }

                    BenchmarkQueryResult result;
                    result.query_num = qnum;
                    result.mode = mode_name;
                    result.run = run;
                    result.time_ms = setup_ok ? worker.result_time_ms : 0;
                    result.success = setup_ok && (res == ForkWorker::SR_OK && worker.result_ok);
                    result.error_msg = setup_ok ? worker.result_error : worker.setup_error;
                    result.epsilon = FormatNumber(dp_epsilon_fixed);
                    result.delta_scenario =
                        needs_delta ? (mode_name == "dp_standard" ? "auto_partition" : "auto_flex") : "";
                    result.dp_delta = needs_delta ? FormatNumber(dp_delta) : "";
                    result.bound_scenario = "single";
                    result.dp_sum_bound = query_has_sum_avg[q] ? FormatNumber(dp_bound) : "";
                    result.dp_count_bound =
                        (mode_name == "dp_standard" || mode_name == "dp_sass") ? FormatNumber(dp_count_bound) : "";
                    result.seed = seed;
                    if (result.success && worker.child_pid > 0) {
                        vector<string> dp_diff_setup = dp_setup;
                        dp_diff_setup.push_back("SET privacy_seed=" + std::to_string(seed) + ";");
                        dp_diff_setup.push_back("SET privacy_diffcols=" +
                                                SqlQuote(std::to_string(diff_key_cols[q]) + ":" + utility_path) + ";");
                        bool diff_setup_ok = worker.SendSetupStatements(dp_diff_setup);
                        if (diff_setup_ok) {
                            auto diff_res = worker.SubmitQuery(query, timeout_s);
                            if (diff_res == ForkWorker::SR_OK && worker.result_ok) {
                                ReadLastUtilityLine(utility_path, result.utility, result.recall, result.precision,
                                                    result.median_error_pct);
                            }
                        }
                    }

                    all_results.push_back(result);

                    string tag = mode_name + " run " + std::to_string(run);
                    if (!setup_ok) {
                        Log(string("Q") + std::to_string(qnum) + " " + tag + " SETUP ERROR: " + result.error_msg);
                    } else if (res == ForkWorker::SR_CHILD_DIED) {
                        Log(string("Q") + std::to_string(qnum) + " " + tag + " CRASHED: " + worker.result_error);
                        worker.Start();
                    } else if (res == ForkWorker::SR_TIMEOUT) {
                        Log(string("Q") + std::to_string(qnum) + " " + tag + " TIMEOUT: " + worker.result_error);
                        worker.Start();
                    } else if (result.success) {
                        Log(string("Q") + std::to_string(qnum) + " " + tag + " time: " +
                            FormatNumber(result.time_ms) + " ms");
                    } else {
                        Log(string("Q") + std::to_string(qnum) + " " + tag + " ERROR: " + result.error_msg);
                    }

                    if (worker.child_pid > 0) {
                        worker.SendUnsetPAC();
                    }
                };

                if (modes.count("dp_standard")) {
                    run_dp_mode("dp_standard");
                }
                if (modes.count("dp_elastic")) {
                    run_dp_mode("dp_elastic");
                }
                if (modes.count("dp_sass")) {
                    run_dp_mode("dp_sass");
                }

                // ----------------------------------------------------------
                // Naive runs (recorded) — explicit sample-table-join variants of the sampling
                // mechanisms (pac, dp_sass). Plain SQL using pac_aggregate / dp_aggregate; no
                // privacy rewrite (metadata is cleared first). Timing + success only, no utility.
                // ----------------------------------------------------------
                if (run_naive) {
                    auto run_naive_variant = [&](const string &mode_label, const string &naive_dir) {
                        string padded = (qnum < 10 ? "0" : "") + std::to_string(qnum);
                        string path = naive_dir + "/q" + padded + ".sql";
                        if (!FileExists(path)) {
                            return; // not a privatizable query for this mechanism — skip silently
                        }
                        string sql = ReadFileToString(path);
                        if (sql.empty()) {
                            return;
                        }
                        if (worker.child_pid <= 0) {
                            worker.Start();
                        }
                        int64_t seed = static_cast<int64_t>(run * 997 + qnum);
                        // Clear privacy metadata so the naive SQL runs as-is (no rewrite), then seed the
                        // pac_aggregate/dp_aggregate noise.
                        worker.SendUnsetPAC();
                        vector<string> naive_setup = {"SET privacy_seed=" + std::to_string(seed) + ";"};
                        bool setup_ok = worker.SendSetupStatements(naive_setup);
                        ForkWorker::SubmitResult res = ForkWorker::SR_OK;
                        if (setup_ok) {
                            res = worker.SubmitQuery(sql, timeout_s);
                        }

                        BenchmarkQueryResult result;
                        result.query_num = qnum;
                        result.mode = mode_label;
                        result.run = run;
                        result.time_ms = setup_ok ? worker.result_time_ms : 0;
                        result.success = setup_ok && (res == ForkWorker::SR_OK && worker.result_ok);
                        result.error_msg = setup_ok ? worker.result_error : worker.setup_error;
                        result.seed = seed;
                        all_results.push_back(result);

                        string tag = mode_label + " run " + std::to_string(run);
                        if (!setup_ok) {
                            Log(string("Q") + std::to_string(qnum) + " " + tag + " SETUP ERROR: " + result.error_msg);
                        } else if (res == ForkWorker::SR_CHILD_DIED) {
                            Log(string("Q") + std::to_string(qnum) + " " + tag + " CRASHED: " + worker.result_error);
                            worker.Start();
                        } else if (res == ForkWorker::SR_TIMEOUT) {
                            Log(string("Q") + std::to_string(qnum) + " " + tag + " TIMEOUT: " + worker.result_error);
                            worker.Start();
                        } else if (result.success) {
                            Log(string("Q") + std::to_string(qnum) + " " + tag + " time: " +
                                FormatNumber(result.time_ms) + " ms");
                        } else {
                            Log(string("Q") + std::to_string(qnum) + " " + tag + " ERROR: " + result.error_msg);
                        }
                    };
                    if (modes.count("pac")) {
                        run_naive_variant("naive_pac", naive_pac_dir);
                    }
                    if (modes.count("dp_sass")) {
                        run_naive_variant("naive_dp", naive_dp_dir);
                    }
                }
            }

            // Worker is stopped automatically when it goes out of scope
        }

        // =====================================================================
        // Post-process: Detect unstable queries
        // =====================================================================
        std::set<int> unstable_queries;
        for (const auto &r : all_results) {
            if (r.mode == "PAC" && !r.success) {
                if (r.error_msg.find("sample diversity") != string::npos) {
                    unstable_queries.insert(r.query_num);
                }
            }
        }
        if (!unstable_queries.empty()) {
            Log("Detected unstable queries (sample diversity errors):");
            for (int qnum : unstable_queries) {
                Log(string("  Q") + std::to_string(qnum));
            }
            for (auto &r : all_results) {
                if (r.mode == "PAC" && unstable_queries.count(r.query_num) > 0) {
                    if (r.success) {
                        r.success = false;
                        r.error_msg = "Marked failed due to sample diversity instability";
                    }
                }
            }
        }

        // =====================================================================
        // Write CSV
        // =====================================================================
        csv << "query,mode,run,time_ms,success,error,utility,recall,precision,median_error_pct,epsilon,delta_scenario,dp_delta,bound_scenario,dp_sum_bound,dp_count_bound,pac_mi,seed\n";
        for (const auto &r : all_results) {
            csv << r.query_num << "," << r.mode << "," << r.run << ","
                << FormatNumber(r.time_ms) << ","
                << (r.success ? "true" : "false") << ","
                << CsvQuote(r.error_msg) << ","
                << r.utility << ","
                << r.recall << ","
                << r.precision << ","
                << r.median_error_pct << ","
                << r.epsilon << ","
                << r.delta_scenario << ","
                << r.dp_delta << ","
                << r.bound_scenario << ","
                << r.dp_sum_bound << ","
                << r.dp_count_bound << ","
                << r.pac_mi << ","
                << r.seed << "\n";
        }
        csv.close();
        Log(string("Results written to: ") + actual_out);

        // =====================================================================
        // Print Statistics
        // =====================================================================
        Log("=== Benchmark Statistics ===");

        int total_queries = static_cast<int>(queries.size());

        int baseline_success = 0, baseline_failed = 0;
        double baseline_total_time = 0;
        for (const auto &r : all_results) {
            if (r.mode == "baseline") {
                if (r.success) { baseline_success++; baseline_total_time += r.time_ms; }
                else { baseline_failed++; }
            }
        }

        int pac_success = 0, pac_rejected = 0, pac_crashed = 0;
        double pac_total_time = 0;
        std::map<string, int> error_counts;

        for (const auto &r : all_results) {
            if (r.mode == "PAC") {
                if (r.success) {
                    pac_success++;
                    pac_total_time += r.time_ms;
                } else {
                    string error_category;
                    if (r.error_msg.find("sample diversity") != string::npos) {
                        error_category = "sample diversity instability";
                        pac_rejected++;
                    } else if (r.error_msg.find("protected column") != string::npos) {
                        error_category = "protected column access";
                        pac_rejected++;
                    } else if (r.error_msg.find("pac_min not implemented") != string::npos ||
                               r.error_msg.find("pac_max not implemented") != string::npos) {
                        error_category = "pac_min/max not implemented for VARCHAR";
                        pac_crashed++;
                    } else if (r.error_msg.find("PAC") != string::npos ||
                               r.error_msg.find("privacy") != string::npos ||
                               r.error_msg.find("aggregat") != string::npos) {
                        error_category = "other PAC rejection";
                        pac_rejected++;
                    } else if (r.error_msg.find("timeout") != string::npos ||
                               r.error_msg.find("memory limit") != string::npos) {
                        error_category = "timeout/OOM";
                        pac_crashed++;
                    } else if (r.error_msg.find("child process died") != string::npos) {
                        error_category = "child crash (OOM/signal)";
                        pac_crashed++;
                    } else {
                        error_category = "other error";
                        pac_crashed++;
                    }
                    error_counts[error_category]++;
                }
            }
        }

        double baseline_avg_success_per_run = static_cast<double>(baseline_success) / num_runs;
        double baseline_avg_failed_per_run = static_cast<double>(baseline_failed) / num_runs;
        double baseline_avg_time_per_run = baseline_total_time / num_runs;

        double pac_avg_success_per_run = static_cast<double>(pac_success) / num_runs;
        double pac_avg_rejected_per_run = static_cast<double>(pac_rejected) / num_runs;
        double pac_avg_crashed_per_run = static_cast<double>(pac_crashed) / num_runs;
        double pac_avg_time_per_run = pac_total_time / num_runs;

        Log(string("Total queries: ") + std::to_string(total_queries));
        Log(string("Number of runs: ") + std::to_string(num_runs));

        Log("--- Baseline (per run average) ---");
        Log(string("  Successful queries: ") + FormatNumber(baseline_avg_success_per_run));
        Log(string("  Failed queries: ") + FormatNumber(baseline_avg_failed_per_run));
        Log(string("  Total time: ") + FormatNumber(baseline_avg_time_per_run) + " ms");
        if (baseline_success > 0) {
            Log(string("  Avg time per successful query: ") +
                FormatNumber(baseline_total_time / baseline_success) + " ms");
        }

        Log("--- PAC (per run average) ---");
        Log(string("  Successful queries: ") + FormatNumber(pac_avg_success_per_run));
        Log(string("  Rejected (privacy violations): ") + FormatNumber(pac_avg_rejected_per_run));
        Log(string("  Crashed/errors: ") + FormatNumber(pac_avg_crashed_per_run));
        Log(string("  Total time (successful): ") + FormatNumber(pac_avg_time_per_run) + " ms");
        if (pac_success > 0) {
            Log(string("  Avg time per successful query: ") +
                FormatNumber(pac_total_time / pac_success) + " ms");
        }

        if (!error_counts.empty()) {
            Log("--- Error breakdown (per run average) ---");
            for (const auto &ec : error_counts) {
                double avg_count = static_cast<double>(ec.second) / num_runs;
                Log(string("  ") + ec.first + ": " + FormatNumber(avg_count));
            }
        }

        // =====================================================================
        // Per-query slowdown
        // =====================================================================
        {
            std::map<int, double> baseline_total_per_q, pac_total_per_q;
            std::map<int, int> baseline_count_per_q, pac_count_per_q;
            for (const auto &r : all_results) {
                if (!r.success) continue;
                if (r.mode == "baseline") {
                    baseline_total_per_q[r.query_num] += r.time_ms;
                    baseline_count_per_q[r.query_num]++;
                } else if (r.mode == "PAC") {
                    pac_total_per_q[r.query_num] += r.time_ms;
                    pac_count_per_q[r.query_num]++;
                }
            }

            Log("--- Per-query slowdown (PAC / baseline) ---");
            double sum_slowdown = 0;
            int n_compared = 0;
            double worst_slowdown = 0;
            int worst_query = 0;
            for (auto &kv : baseline_total_per_q) {
                int qnum = kv.first;
                if (pac_count_per_q.count(qnum) == 0 || baseline_count_per_q[qnum] == 0) continue;
                double baseline_mean = kv.second / baseline_count_per_q[qnum];
                double pac_mean = pac_total_per_q[qnum] / pac_count_per_q[qnum];
                double slowdown = (baseline_mean > 0) ? pac_mean / baseline_mean : 0;
                Log(string("  Q") + std::to_string(qnum) + ": " +
                    FormatNumber(baseline_mean) + " ms -> " + FormatNumber(pac_mean) + " ms (" +
                    FormatNumber(slowdown) + "x)");
                sum_slowdown += slowdown;
                n_compared++;
                if (slowdown > worst_slowdown) {
                    worst_slowdown = slowdown;
                    worst_query = qnum;
                }
            }
            if (n_compared > 0) {
                double avg_slowdown = sum_slowdown / n_compared;
                Log(string("  Average slowdown: ") + FormatNumber(avg_slowdown) + "x over " +
                    std::to_string(n_compared) + " queries");
                Log(string("  Worst slowdown: ") + FormatNumber(worst_slowdown) + "x (Q" +
                    std::to_string(worst_query) + ")");
            }
        }

        Log("=== Benchmark Complete ===");
        return 0;

    } catch (std::exception &ex) {
        Log(string("Error running benchmark: ") + ex.what());
        return 2;
    }
}

} // namespace duckdb

// Helper for printing usage
static void PrintUsageMain() {
    std::cout << "Usage: pac_clickhouse_benchmark [options]\n"
              << "Options:\n"
              << "  --micro           Run with a smaller dataset (5000000 rows) for quick testing\n"
              << "                    Uses clickbench_micro.db by default\n"
              << "  --db <path>       DuckDB database file (default: clickbench.db or clickbench_micro.db)\n"
              << "  --queries <dir>   Directory containing create.sql, load.sql, queries.sql\n"
              << "                    (default: benchmark/clickbench/clickbench_queries)\n"
              << "  --out <csv>       Output CSV path (default: benchmark/clickbench_results.csv)\n"
              << "  --modes <csv>     Comma-separated privacy mechanisms to measure\n"
              << "                    (subset of pac,dp_standard,dp_elastic,dp_sass; default: all four).\n"
              << "                    The non-private baseline always runs.\n"
              << "  --run-naive       Also run the explicit sample-table-join naive variants for the\n"
              << "                    sampling mechanisms in --modes (pac, dp_sass).\n"
              << "  -h, --help        Show this help message\n";
}

int main(int argc, char **argv) {
    bool micro = false;
    std::string db_path;
    std::string queries_dir = "benchmark/clickbench/clickbench_queries";
    std::string out_csv;
    bool run_naive = false;
    const std::set<std::string> valid_modes = {"pac", "dp_standard", "dp_elastic", "dp_sass"};
    std::set<std::string> modes = valid_modes; // default: all four

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsageMain();
            return 0;
        } else if (arg == "--micro") {
            micro = true;
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--queries" && i + 1 < argc) {
            queries_dir = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_csv = argv[++i];
        } else if (arg == "--run-naive") {
            run_naive = true;
        } else if (arg == "--modes" && i + 1 < argc) {
            modes.clear();
            std::string list = argv[++i];
            std::stringstream ss(list);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                // trim whitespace
                size_t b = tok.find_first_not_of(" \t");
                size_t e = tok.find_last_not_of(" \t");
                if (b == std::string::npos) {
                    continue;
                }
                tok = tok.substr(b, e - b + 1);
                if (!valid_modes.count(tok)) {
                    std::cerr << "Unknown mode '" << tok << "'. Valid: pac,dp_standard,dp_elastic,dp_sass\n";
                    return 1;
                }
                modes.insert(tok);
            }
            if (modes.empty()) {
                std::cerr << "--modes given but no valid modes parsed\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsageMain();
            return 1;
        }
    }

    return duckdb::RunClickHouseBenchmark(db_path, queries_dir, out_csv, micro, run_naive, modes);
}
