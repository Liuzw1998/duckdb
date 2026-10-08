#include "duckdb.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace duckdb;
using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

class StartGate {
public:
	explicit StartGate(int expected) : expected(expected) {
	}

	void WaitForReady() {
		std::unique_lock<std::mutex> guard(lock);
		ready.wait(guard, [&] { return arrived == expected; });
	}

	Clock::time_point ArriveAndWait() {
		std::unique_lock<std::mutex> guard(lock);
		arrived++;
		if (arrived == expected) {
			ready.notify_one();
		}
		start.wait(guard, [&] { return started; });
		return deadline;
	}

	void Start(Clock::time_point start_deadline) {
		std::lock_guard<std::mutex> guard(lock);
		deadline = start_deadline;
		started = true;
		start.notify_all();
	}

private:
	const int expected;
	int arrived = 0;
	bool started = false;
	Clock::time_point deadline;
	std::mutex lock;
	std::condition_variable ready;
	std::condition_variable start;
};

static std::string CurrentAffinity() {
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0) {
		return "unknown";
	}

	std::string result;
	int range_start = -1;
	auto append_range = [&](int range_end) {
		if (!result.empty()) {
			result += ',';
		}
		result += std::to_string(range_start);
		if (range_start != range_end) {
			result += '-';
			result += std::to_string(range_end);
		}
		range_start = -1;
	};
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &cpus)) {
			if (range_start < 0) {
				range_start = cpu;
			}
		} else if (range_start >= 0) {
			append_range(cpu - 1);
		}
	}
	if (range_start >= 0) {
		append_range(CPU_SETSIZE - 1);
	}
	return result.empty() ? "none" : result;
}

static double TimevalSeconds(const timeval &value) {
	return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
}

static void Consume(QueryResult &result) {
	if (result.HasError()) {
		throw std::runtime_error(result.GetError());
	}
	while (auto chunk = result.Fetch()) {
		(void)chunk->size();
	}
	if (result.HasError()) {
		throw std::runtime_error(result.GetError());
	}
}

static MaterializedQueryResult &GetMaterialized(QueryResult &result) {
	if (result.HasError()) {
		throw std::runtime_error(result.GetError());
	}
	if (result.GetResultType() != QueryResultType::MATERIALIZED_RESULT) {
		throw std::runtime_error("workload correctness gate expected a materialized result");
	}
	return result.Cast<MaterializedQueryResult>();
}

static std::vector<int64_t> PointIDs() {
	std::vector<int64_t> result;
	for (int64_t i = 12345; i < 12375; i++) {
		result.push_back((i * 48271) % 1000000);
	}
	return result;
}

static duckdb::vector<Value> PointValues(int64_t id) {
	int64_t seed = 0;
	for (int64_t candidate = 12345; candidate < 12375; candidate++) {
		if ((candidate * 48271) % 1000000 == id) {
			seed = candidate;
			break;
		}
	}
	return {Value::BIGINT(id), Value::BIGINT(seed % 100), Value(std::string((seed % 10 < 8) ? "ready" : "closed"))};
}

static duckdb::vector<Value> In32Values() {
	duckdb::vector<Value> values;
	for (int64_t i = 12345; i < 12377; i++) {
		values.push_back(Value::BIGINT((i * 48271) % 1000000));
	}
	values.push_back(Value(std::string("ready")));
	return values;
}

static std::unique_ptr<PreparedStatement> Prepare(Connection &connection, const std::string &sql) {
	auto statement = connection.Prepare(sql);
	if (!statement || statement->HasError()) {
		throw std::runtime_error(statement ? statement->GetError() : "prepare failed");
	}
	return std::move(statement);
}

static void ValidateCount(PreparedStatement &statement, duckdb::vector<Value> values, int64_t expected) {
	auto result = statement.Execute(values, false);
	if (!result) {
		throw std::runtime_error("workload correctness gate returned no result");
	}
	auto &materialized = GetMaterialized(*result);
	if (materialized.RowCount() != 1) {
		throw std::runtime_error("workload correctness gate returned an unexpected row count");
	}
	const auto value = materialized.GetValue(0, 0);
	if (value.IsNull() || value.GetValue<int64_t>() != expected) {
		throw std::runtime_error("workload correctness gate returned an unexpected count");
	}
}

static void ValidatePayload(PreparedStatement &statement) {
	duckdb::vector<Value> no_values;
	auto result = statement.Execute(no_values, false);
	if (!result) {
		throw std::runtime_error("workload correctness gate returned no payload result");
	}
	auto &materialized = GetMaterialized(*result);
	if (materialized.RowCount() != 1 || materialized.GetValue(0, 0).ToString().size() != 4096) {
		throw std::runtime_error("workload correctness gate returned an unexpected payload");
	}
}

static void ValidateWorkload(Connection &connection) {
	const auto point_ids = PointIDs();
	auto single = Prepare(connection, "SELECT count(*) FROM orders WHERE id = ?");
	auto point = Prepare(connection, "SELECT count(*) FROM orders WHERE id = ? AND tenant_id = ? AND status = ?");
	for (auto id : point_ids) {
		ValidateCount(*single, {Value::BIGINT(id)}, 1);
		ValidateCount(*point, PointValues(id), 1);
	}

	std::string placeholders = "?";
	for (int i = 1; i < 32; i++) {
		placeholders += ", ?";
	}
	auto in32 = Prepare(connection, "SELECT count(*) FROM orders WHERE id IN (" + placeholders + ") AND status = ?");
	ValidateCount(*in32, In32Values(), 26);

	auto wide =
	    Prepare(connection, "SELECT payload_wide FROM candidate_orders WHERE lookup_key = 42 AND status_one = 'ready'");
	ValidatePayload(*wide);
	auto medium_plain =
	    Prepare(connection, "SELECT payload FROM candidate_medium_plain WHERE lookup_key = 42 AND residual = 'keep'");
	ValidatePayload(*medium_plain);
	auto medium_zstd =
	    Prepare(connection, "SELECT payload FROM candidate_medium_zstd WHERE lookup_key = 42 AND residual = 'keep'");
	ValidatePayload(*medium_zstd);
}

static int64_t ConfigureThreads(Connection &connection, int requested_threads) {
	auto set_result = connection.Query("SET threads=" + std::to_string(requested_threads));
	if (!set_result || set_result->HasError()) {
		throw std::runtime_error(set_result ? set_result->GetError() : "failed to set threads");
	}
	auto current = connection.Query("SELECT current_setting('threads')");
	if (!current || current->HasError() || current->RowCount() != 1) {
		throw std::runtime_error(current ? current->GetError() : "failed to read threads");
	}
	return current->GetValue(0, 0).GetValue<int64_t>();
}

static void ConfigureWorkload(Connection &connection, const std::string &query, bool sequential) {
	if (sequential) {
		auto result = connection.Query("SET index_scan_max_count=0; SET index_scan_percentage=0;");
		if (!result || result->HasError()) {
			throw std::runtime_error(result ? result->GetError() : "failed to disable index scans");
		}
		return;
	}
	if (query != "medium_plain" && query != "medium_zstd") {
		return;
	}
	auto result = connection.Query("SET index_scan_max_count=2048; SET index_scan_percentage=0;");
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "failed to configure medium workload");
	}
}

static void Execute(PreparedStatement &statement, duckdb::vector<Value> values) {
	auto result = statement.Execute(values, true);
	if (!result) {
		throw std::runtime_error("query execution returned no result");
	}
	Consume(*result);
}

static void Worker(DuckDB &database, const std::string &query, bool sequential, StartGate &gate,
                   std::vector<int64_t> &local) {
	Connection connection(database);
	ConfigureWorkload(connection, query, sequential);
	ValidateWorkload(connection);
	auto single = Prepare(connection, "SELECT count(*) FROM orders WHERE id = ?");
	auto point = Prepare(connection, "SELECT count(*) FROM orders WHERE id = ? AND tenant_id = ? AND status = ?");
	std::string placeholders = "?";
	for (int i = 1; i < 32; i++) {
		placeholders += ", ?";
	}
	auto in32 = Prepare(connection, "SELECT count(*) FROM orders WHERE id IN (" + placeholders + ") AND status = ?");
	auto wide =
	    Prepare(connection, "SELECT payload_wide FROM candidate_orders WHERE lookup_key = 42 AND status_one = 'ready'");
	auto medium_plain =
	    Prepare(connection, "SELECT payload FROM candidate_medium_plain WHERE lookup_key = 42 AND residual = 'keep'");
	auto medium_zstd =
	    Prepare(connection, "SELECT payload FROM candidate_medium_zstd WHERE lookup_key = 42 AND residual = 'keep'");
	const auto point_ids = PointIDs();
	const auto in_values = In32Values();
	auto execute_query = [&](idx_t iteration) {
		if (query == "single") {
			Execute(*single, {Value::BIGINT(point_ids[iteration % point_ids.size()])});
		} else if (query == "point") {
			Execute(*point, PointValues(point_ids[iteration % point_ids.size()]));
		} else if (query == "in32") {
			Execute(*in32, in_values);
		} else if (query == "medium_plain") {
			Execute(*medium_plain, {});
		} else if (query == "medium_zstd") {
			Execute(*medium_zstd, {});
		} else {
			Execute(*wide, {});
		}
	};

	for (idx_t i = 0; i < 5; i++) {
		execute_query(i);
	}

	const auto end = gate.ArriveAndWait();
	idx_t iteration = 0;
	while (Clock::now() < end) {
		auto start = Clock::now();
		execute_query(iteration);
		local.push_back(std::chrono::duration_cast<Nanos>(Clock::now() - start).count());
		iteration++;
	}
}

static int64_t Percentile(const std::vector<int64_t> &values, double percentile) {
	if (values.empty()) {
		return 0;
	}
	const auto index = static_cast<idx_t>(percentile * static_cast<double>(values.size() - 1));
	return values[index];
}

int main(int argc, char **argv) {
	if (argc < 5 || argc > 7) {
		std::cerr << "usage: native_runner DB THREADS CONNECTIONS QUERY [SECONDS] [default|seq]\n";
		return 2;
	}
	const std::string db_path = argv[1];
	const int requested_threads = std::atoi(argv[2]);
	const int connections = std::atoi(argv[3]);
	const std::string query = argv[4];
	const double seconds = argc >= 6 ? std::atof(argv[5]) : 5.0;
	const std::string scan_mode = argc == 7 ? argv[6] : "default";
	if (requested_threads <= 0 || connections <= 0 || seconds <= 0 || (scan_mode != "default" && scan_mode != "seq") ||
	    (query != "single" && query != "point" && query != "in32" && query != "wide" && query != "medium_plain" &&
	     query != "medium_zstd")) {
		std::cerr << "invalid benchmark arguments\n";
		return 2;
	}

	DuckDB database(db_path);
	Connection setup(database);
	const auto configured_threads = ConfigureThreads(setup, requested_threads);
	const bool sequential = scan_mode == "seq";
	ConfigureWorkload(setup, query, sequential);
	ValidateWorkload(setup);

	StartGate gate(connections);
	std::vector<std::vector<int64_t>> worker_nanos(connections);
	std::vector<std::thread> workers;
	for (int i = 0; i < connections; i++) {
		workers.emplace_back(Worker, std::ref(database), std::cref(query), sequential, std::ref(gate),
		                     std::ref(worker_nanos[i]));
	}

	gate.WaitForReady();
	rusage before {}, after {};
	getrusage(RUSAGE_SELF, &before);
	const auto wall_start = Clock::now();
	const auto deadline =
	    wall_start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
	gate.Start(deadline);
	for (auto &worker : workers) {
		worker.join();
	}
	const auto wall_end = Clock::now();
	getrusage(RUSAGE_SELF, &after);

	std::vector<int64_t> nanos;
	for (auto &local : worker_nanos) {
		nanos.insert(nanos.end(), local.begin(), local.end());
	}
	std::sort(nanos.begin(), nanos.end());
	const double elapsed = std::chrono::duration<double>(wall_end - wall_start).count();
	const double user_cpu_s = TimevalSeconds(after.ru_utime) - TimevalSeconds(before.ru_utime);
	const double system_cpu_s = TimevalSeconds(after.ru_stime) - TimevalSeconds(before.ru_stime);
	const double cpu_seconds = user_cpu_s + system_cpu_s;
	const auto completed = nanos.size();
	const auto affinity = CurrentAffinity();
	std::cout << "{\"threads\":" << configured_threads << ",\"requested_threads\":" << requested_threads
	          << ",\"connections\":" << connections << ",\"application_threads\":" << connections << ",\"query\":\""
	          << query << "\",\"scan_mode\":\"" << scan_mode << "\",\"affinity\":\"" << affinity
	          << "\",\"completed\":" << completed << ",\"requested_seconds\":" << std::setprecision(9) << seconds
	          << ",\"elapsed_s\":" << elapsed << ",\"user_cpu_s\":" << user_cpu_s
	          << ",\"system_cpu_s\":" << system_cpu_s << ",\"qps\":" << (completed / elapsed)
	          << ",\"p50_ms\":" << Percentile(nanos, 0.50) / 1e6 << ",\"p95_ms\":" << Percentile(nanos, 0.95) / 1e6
	          << ",\"cpu_ms_per_query\":" << (completed ? cpu_seconds * 1000.0 / completed : 0)
	          << ",\"rss_peak_kb\":" << after.ru_maxrss << "}\n";
}
