#include "b-plus-tree/tree.hpp"
#include "b-plus-tree/utility.hpp"
#include "definitions.h"
#include "path-oram/oram.hpp"
#include "path-oram/utility.hpp"

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <ctime>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>

using namespace std;
using namespace DPORAM;

namespace po = boost::program_options;
namespace pt = boost::property_tree;

string timeToString(long long time);
number salaryToNumber(string salary);
double numberToSalary(number salary);
string filename(string filename, int i);
string redishost(string host, int i);
template <class INPUT, class OUTPUT>
vector<OUTPUT> transform(vector<INPUT> input, function<OUTPUT(const INPUT&)> application);

void LOG(LOG_LEVEL level, string message);
void LOG(LOG_LEVEL level, boost::format message);

#pragma region GLOBALS

auto COUNT				   = 1000uLL;
auto ORAM_BLOCK_SIZE	   = 256uLL;
auto ORAM_LOG_CAPACITY	   = 10uLL;
auto ORAMS_NUMBER		   = 1;
auto PARALLEL			   = true;
const auto ORAM_Z		   = 3uLL;
const auto TREE_BLOCK_SIZE = 64uLL;
auto ORAM_STORAGE		   = FileSystem;
auto USE_ORAMS			   = true;
const auto BATCH_SIZE	   = 1000;

const auto FILES_DIR		 = "./storage-files";
const auto KEY_FILE			 = "key";
const auto TREE_FILE		 = "tree";
const auto ORAM_STORAGE_FILE = "oram-storage";
const auto ORAM_MAP_FILE	 = "oram-map";
const auto ORAM_STASH_FILE	 = "oram-stash";

const auto DATA_FILE  = "../../experiments-scripts/scripts/data.csv";
const auto QUERY_FILE = "../../experiments-scripts/scripts/query.csv";

#pragma endregion

int main(int argc, char* argv[])
{
#pragma region COMMAND_LINE_ARGUMENTS

	po::options_description desc("range query processor");
	desc.add_options()("help", "produce help message");
	desc.add_options()("generateIndices", po::value<bool>()->default_value(true), "if set, will generate ORAM and tree indices, otherwise will read files");
	desc.add_options()("readInputs", po::value<bool>()->default_value(true), "if set, will read inputs from files");
	desc.add_options()("parallel", po::value<bool>(&PARALLEL)->default_value(true), "if set, will query orams in parallel");
	desc.add_options()("oramStorage", po::value<ORAM_BACKEND>(&ORAM_STORAGE)->default_value(FileSystem), "the ORAM backend to use");
	desc.add_options()("oramsNumber", po::value<int>(&ORAMS_NUMBER)->notifier([](int v) { if (v < 1 || v > 96) { throw Exception("malformed --oramsNumber"); } })->default_value(1), "the number of parallel ORAMs to use");
	desc.add_options()("useOrams", po::value<bool>(&USE_ORAMS)->default_value(true), "if set will use ORAMs, otherwise each query will download everythin every query");
	desc.add_options()("verbosity", po::value<LOG_LEVEL>(&__logLevel)->default_value(INFO), "verbosity level to output");
	desc.add_options()("redisHost", po::value<string>()->default_value("tcp://127.0.0.1:6379"), "Redis host to use");
	desc.add_options()("aerospikeHost", po::value<string>()->default_value("127.0.0.1"), "Aerospike host to use");

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (vm.count("help"))
	{
		cout << desc << "\n";
		exit(1);
	}

	if (
		vm["oramStorage"].as<ORAM_BACKEND>() == FileSystem &&
		!vm["useOrams"].as<bool>() &&
		vm["parallel"].as<bool>())
	{
		LOG(WARNING, "Can't use FS strawman storage in parallel. PARALLEL will be set to false.");
		PARALLEL = false;
	}

#pragma endregion

#pragma region GENERATE_DATA

	LOG(INFO, "Constructing data set...");

	vector<pair<number, bytes>> oramIndex;
	vector<pair<number, bytes>> treeIndex;
	vector<pair<number, number>> queries;
	if (vm["readInputs"].as<bool>())
	{
		ifstream dataFile(DATA_FILE);

		string line = "";
		auto i		= 0;
		while (getline(dataFile, line))
		{
			vector<string> record;
			boost::algorithm::split(record, line, boost::is_any_of(","));
			auto salary = salaryToNumber(record[7]);

			LOG(TRACE, boost::format("Salary: %9.2f, data length: %3i") % numberToSalary(salary) % line.size());

			oramIndex.push_back({i, PathORAM::fromText(line, ORAM_BLOCK_SIZE)});
			treeIndex.push_back({salary, BPlusTree::bytesFromNumber(i)});
			i++;
		}
		dataFile.close();

		ifstream queryFile(QUERY_FILE);

		line = "";
		while (getline(queryFile, line))
		{
			vector<string> query;
			boost::algorithm::split(query, line, boost::is_any_of(","));
			auto left  = salaryToNumber(query[0]);
			auto right = salaryToNumber(query[1]);

			LOG(TRACE, boost::format("Query: {%9.2f, %9.2f}") % numberToSalary(left) % numberToSalary(right));

			queries.push_back({left, right});
		}
		queryFile.close();
	}
	else
	{
		for (number i = 0; i < COUNT; i++)
		{
			ostringstream text;
			for (auto j = 0; j < 10; j++)
			{
				text << to_string(i) + (j < 9 ? "," : "");
			}
			oramIndex.push_back({i, PathORAM::fromText(text.str(), ORAM_BLOCK_SIZE)});
			treeIndex.push_back({salaryToNumber(to_string(i)), BPlusTree::bytesFromNumber(i)});
		}

		for (number i = 0; i < COUNT / 10; i++)
		{
			queries.push_back({salaryToNumber(to_string(8 * i + 3)), salaryToNumber(to_string(8 * i + 8))});
		}
	}

	COUNT = oramIndex.size();

	auto sizes		= transform<pair<number, bytes>, int>(oramIndex, [](pair<number, bytes> val) { return val.second.size(); });
	ORAM_BLOCK_SIZE = *max_element(sizes.begin(), sizes.end());

	ORAM_LOG_CAPACITY = ceil(log2(COUNT / ORAMS_NUMBER)) + 1;

	LOG(INFO, boost::format("COUNT = %1%") % COUNT);
	LOG(INFO, boost::format("ORAM_BLOCK_SIZE = %1%") % ORAM_BLOCK_SIZE);
	LOG(INFO, boost::format("ORAM_LOG_CAPACITY = %1%") % ORAM_LOG_CAPACITY);
	LOG(INFO, boost::format("ORAMS_NUMBER = %1%") % ORAMS_NUMBER);
	LOG(INFO, boost::format("PARALLEL = %1%") % PARALLEL);
	LOG(INFO, boost::format("ORAM_Z = %1%") % ORAM_Z);
	LOG(INFO, boost::format("TREE_BLOCK_SIZE = %1%") % TREE_BLOCK_SIZE);
	LOG(INFO, boost::format("ORAM_BACKEND = %1%") % oramBackendStrings[ORAM_STORAGE]);
	LOG(INFO, boost::format("USE_ORAMS = %1%") % USE_ORAMS);

	LOG(INFO, boost::format("REDIS = %1%") % vm["redisHost"].as<string>());
	LOG(INFO, boost::format("AEROSPIKE = %1%") % vm["aerospikeHost"].as<string>());

#pragma endregion

#pragma region CONSTRUCT_INDICES

	LOG(INFO,
		vm["generateIndices"].as<bool>() ?
			"Generating indices..." :
			"Reading from files...");

	if (vm["generateIndices"].as<bool>())
	{
		boost::filesystem::remove_all(FILES_DIR);
		boost::filesystem::create_directories(FILES_DIR);
	}

	vector<pair<number, number>> measurements;

	if (vm["useOrams"].as<bool>())
	{
		vector<vector<pair<number, bytes>>> oramIndexBrokenUp;
		oramIndexBrokenUp.resize(ORAMS_NUMBER);
		for (auto record : oramIndex)
		{
			oramIndexBrokenUp[record.first % ORAMS_NUMBER].push_back({record.first / ORAMS_NUMBER, record.second});
		}
		vector<ORAMSet> oramSets;
		for (auto i = 0; i < ORAMS_NUMBER; i++)
		{
			bytes oramKey;
			if (vm["generateIndices"].as<bool>())
			{
				oramKey = PathORAM::getRandomBlock(KEYSIZE);
				PathORAM::storeKey(oramKey, filename(KEY_FILE, i));
			}
			else
			{
				oramKey = PathORAM::loadKey(filename(KEY_FILE, i));
			}

			shared_ptr<PathORAM::AbsStorageAdapter> oramStorage;
			switch (ORAM_STORAGE)
			{
				case InMemory:
					oramStorage = make_shared<PathORAM::InMemoryStorageAdapter>(((1 << ORAM_LOG_CAPACITY) * ORAM_Z) + ORAM_Z, ORAM_BLOCK_SIZE, oramKey);
					break;
				case FileSystem:
					oramStorage = make_shared<PathORAM::FileSystemStorageAdapter>(((1 << ORAM_LOG_CAPACITY) * ORAM_Z) + ORAM_Z, ORAM_BLOCK_SIZE, oramKey, filename(ORAM_STORAGE_FILE, i), vm["generateIndices"].as<bool>());
					break;
				case Redis:
					oramStorage = make_shared<PathORAM::RedisStorageAdapter>(((1 << ORAM_LOG_CAPACITY) * ORAM_Z) + ORAM_Z, ORAM_BLOCK_SIZE, oramKey, redishost(vm["redisHost"].as<string>(), i), vm["generateIndices"].as<bool>());
					break;
				case Aerospike:
					oramStorage = make_shared<PathORAM::AerospikeStorageAdapter>(((1 << ORAM_LOG_CAPACITY) * ORAM_Z) + ORAM_Z, ORAM_BLOCK_SIZE, oramKey, vm["aerospikeHost"].as<string>(), vm["generateIndices"].as<bool>(), to_string(i));
					break;
			}

			auto oramPositionMap = make_shared<PathORAM::InMemoryPositionMapAdapter>(((1 << ORAM_LOG_CAPACITY) * ORAM_Z) + ORAM_Z);
			if (!vm["generateIndices"].as<bool>())
			{
				oramPositionMap->loadFromFile(filename(ORAM_MAP_FILE, i));
			}
			auto oramStash = make_shared<PathORAM::InMemoryStashAdapter>(3 * ORAM_LOG_CAPACITY * ORAM_Z);
			if (!vm["generateIndices"].as<bool>())
			{
				oramStash->loadFromFile(filename(ORAM_STASH_FILE, i), ORAM_BLOCK_SIZE);
			}
			auto oram = make_shared<PathORAM::ORAM>(
				ORAM_LOG_CAPACITY,
				ORAM_BLOCK_SIZE,
				ORAM_Z,
				oramStorage,
				oramPositionMap,
				oramStash,
				vm["generateIndices"].as<bool>());

			if (vm["generateIndices"].as<bool>())
			{
				oram->load(oramIndexBrokenUp[i]);
				oramPositionMap->storeToFile(filename(ORAM_MAP_FILE, i));
				oramStash->storeToFile(filename(ORAM_STASH_FILE, i));
			}

			oramSets.push_back({oramStorage, oramPositionMap, oramStash, oram});
		}

		auto treeStorage = make_shared<BPlusTree::FileSystemStorageAdapter>(TREE_BLOCK_SIZE, filename(TREE_FILE, -1), vm["generateIndices"].as<bool>());
		auto tree		 = vm["generateIndices"].as<bool>() ? make_shared<BPlusTree::Tree>(treeStorage, treeIndex) : make_shared<BPlusTree::Tree>(treeStorage);

		auto orams = transform<ORAMSet, shared_ptr<PathORAM::ORAM>>(oramSets, [](ORAMSet val) { return get<3>(val); });

#pragma endregion

#pragma region QUERY

		LOG(INFO, boost::format("Running %1% queries...") % queries.size());

		auto queryOram = [](vector<number> ids, shared_ptr<PathORAM::ORAM> oram, promise<vector<bytes>>* promise) -> vector<bytes> {
			vector<bytes> answer;
			for (auto id : ids)
			{
				auto block = oram->get(id);
				answer.push_back(block);
			}

			if (promise != NULL)
			{
				promise->set_value(answer);
			}

			return answer;
		};

		for (auto query : queries)
		{
			auto start = chrono::steady_clock::now();

			auto oramIds = tree->search(query.first, query.second);
			vector<vector<number>> blockIds;
			blockIds.resize(ORAMS_NUMBER);
			for (auto oramId : oramIds)
			{
				auto blockId = BPlusTree::numberFromBytes(oramId);
				blockIds[blockId % ORAMS_NUMBER].push_back(blockId / ORAMS_NUMBER);
			}

			auto count = 0;
			if (PARALLEL)
			{
				thread threads[ORAMS_NUMBER];
				promise<vector<bytes>> promises[ORAMS_NUMBER];
				future<vector<bytes>> futures[ORAMS_NUMBER];

				for (auto i = 0; i < ORAMS_NUMBER; i++)
				{
					futures[i] = promises[i].get_future();
					threads[i] = thread(queryOram, blockIds[i], orams[i], &promises[i]);
				}

				for (auto i = 0; i < ORAMS_NUMBER; i++)
				{
					auto result = futures[i].get();
					threads[i].join();
					count += result.size();
				}
			}
			else
			{
				for (auto i = 0; i < ORAMS_NUMBER; i++)
				{
					auto result = queryOram(blockIds[i], orams[i], NULL);
					count += result.size();
				}
			}

			auto elapsed = chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now() - start).count();
			measurements.push_back({elapsed, count});

			LOG(DEBUG, boost::format("For query {%9.2f, %9.2f} the result size is %3i (completed in %7s, or %7s μs per record)") % numberToSalary(query.first) % numberToSalary(query.second) % count % timeToString(elapsed) % (count > 0 ? timeToString(elapsed / count) : "0 ns"));
		}
#pragma endregion
	}
	else
	{
#pragma region STRAWMAN
		bytes storageKey;
		if (vm["generateIndices"].as<bool>())
		{
			storageKey = PathORAM::getRandomBlock(KEYSIZE);
			PathORAM::storeKey(storageKey, filename(KEY_FILE, -1));
		}
		else
		{
			storageKey = PathORAM::loadKey(filename(KEY_FILE, -1));
		}

		shared_ptr<PathORAM::AbsStorageAdapter> storage;
		switch (ORAM_STORAGE)
		{
			case InMemory:
				storage = make_shared<PathORAM::InMemoryStorageAdapter>(COUNT, ORAM_BLOCK_SIZE, storageKey);
				break;
			case FileSystem:
				storage = make_shared<PathORAM::FileSystemStorageAdapter>(COUNT, ORAM_BLOCK_SIZE, storageKey, filename(ORAM_STORAGE_FILE, -1), vm["generateIndices"].as<bool>());
				break;
			case Redis:
				storage = make_shared<PathORAM::RedisStorageAdapter>(COUNT, ORAM_BLOCK_SIZE, storageKey, redishost(vm["redisHost"].as<string>(), -1), vm["generateIndices"].as<bool>());
				break;
			case Aerospike:
				storage = make_shared<PathORAM::AerospikeStorageAdapter>(COUNT, ORAM_BLOCK_SIZE, storageKey, vm["aerospikeHost"].as<string>(), vm["generateIndices"].as<bool>());
				break;
		}

		vector<pair<number, pair<number, bytes>>> batch;
		for (number i = 0; i < oramIndex.size(); i++)
		{
			batch.push_back({i, oramIndex[i]});
			if (i % BATCH_SIZE == 0 || i == oramIndex.size() - 1)
			{
				if (batch.size() > 0)
				{
					storage->set(batch);
				}
				batch.clear();
			}
		}

		auto storageQuery = [storage](number indexFrom, number indexTo, number queryFrom, number queryTo, promise<vector<string>>* promise) -> vector<string> {
			vector<string> answer;

			vector<number> batch;
			for (auto i = indexFrom; i < indexTo; i++)
			{
				batch.push_back(i);
				if (i % BATCH_SIZE == 0 || i == indexTo - 1)
				{
					if (batch.size() > 0)
					{
						auto returned = storage->get(batch);
						for (auto record : returned)
						{
							auto text = PathORAM::toText(record.second, ORAM_BLOCK_SIZE);

							vector<string> broken;
							boost::algorithm::split(broken, text, boost::is_any_of(","));
							auto salary = salaryToNumber(broken[7]);

							if (salary >= queryFrom && salary <= queryTo)
							{
								answer.push_back(text);
							}
						}
						batch.clear();
					}
				}
			}

			if (promise != NULL)
			{
				promise->set_value(answer);
			}

			return answer;
		};

		for (auto query : queries)
		{
			auto start = chrono::steady_clock::now();

			auto count = 0;
			if (PARALLEL)
			{
				thread threads[ORAMS_NUMBER];
				promise<vector<string>> promises[ORAMS_NUMBER];
				future<vector<string>> futures[ORAMS_NUMBER];

				for (auto i = 0; i < ORAMS_NUMBER; i++)
				{
					futures[i] = promises[i].get_future();
					threads[i] = thread(storageQuery, i * COUNT / ORAMS_NUMBER, (i + 1) * COUNT / ORAMS_NUMBER, query.first, query.second, &promises[i]);
				}

				for (auto i = 0; i < ORAMS_NUMBER; i++)
				{
					auto result = futures[i].get();
					threads[i].join();
					count += result.size();
				}
			}
			else
			{
				for (auto i = 0; i < ORAMS_NUMBER; i++)
				{
					auto result = storageQuery(i * COUNT / ORAMS_NUMBER, (i + 1) * COUNT / ORAMS_NUMBER, query.first, query.second, NULL);
					count += result.size();
				}
			}

			auto elapsed = chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now() - start).count();
			measurements.push_back({elapsed, count});

			LOG(DEBUG, boost::format("For query {%9.2f, %9.2f} the result size is %3i (completed in %7s, or %7s per record)") % numberToSalary(query.first) % numberToSalary(query.second) % count % timeToString(elapsed) % (count > 0 ? timeToString(elapsed / count) : "0 ns"));
		}
#pragma endregion
	}

	LOG(INFO, "Complete!");

	auto overheads = transform<pair<number, number>, number>(measurements, [](pair<number, number> val) { return val.first; });
	auto counts	   = transform<pair<number, number>, number>(measurements, [](pair<number, number> val) { return val.second; });

	auto sum		   = accumulate(overheads.begin(), overheads.end(), 0LL);
	auto average	   = sum / overheads.size();
	auto perResultItem = sum / accumulate(counts.begin(), counts.end(), 0LL);

#pragma region WRITE_JSON

	LOG(INFO, boost::format("Total: %1%, average: %2% per query, %3% per result item") % timeToString(sum) % timeToString(average) % timeToString(perResultItem));

	pt::ptree root;
	pt::ptree overheadsNode;

	for (auto measurement : measurements)
	{
		pt::ptree overhead;
		overhead.put("overhead", measurement.first);
		overhead.put("queries", measurement.second);
		overheadsNode.push_back({"", overhead});
	}
	root.put("COUNT", COUNT);
	root.put("ORAM_BLOCK_SIZE", ORAM_BLOCK_SIZE);
	root.put("ORAM_LOG_CAPACITY", ORAM_LOG_CAPACITY);
	root.put("ORAMS_NUMBER", ORAMS_NUMBER);
	root.put("PARALLEL", PARALLEL);
	root.put("ORAM_Z", ORAM_Z);
	root.put("TREE_BLOCK_SIZE", TREE_BLOCK_SIZE);
	root.put("ORAM_BACKEND", oramBackendStrings[ORAM_STORAGE]);
	root.put("USE_ORAMS", USE_ORAMS);
	root.put("REDIS", vm["redisHost"].as<string>());
	root.put("AEROSPIKE", vm["aerospikeHost"].as<string>());

	auto timestamp = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch()).count();
	root.put("TIMESTAMP", timestamp);

	pt::ptree aggregates;
	aggregates.put("totalElapsed", sum);
	aggregates.put("perQuery", average);
	aggregates.put("perResultItem", perResultItem);
	root.add_child("aggregates", aggregates);

	root.add_child("queries", overheadsNode);

	auto rawtime = time(nullptr);
	stringstream timestream;
	timestream << put_time(localtime(&rawtime), "%Y-%m-%d-%H-%M-%S");

	auto filename = boost::str(boost::format("./results/%1%-%2%.json") % timestream.str() % timestamp);

	pt::write_json(filename, root);

	LOG(INFO, boost::format("Log written to %1%") % filename);

#pragma endregion

	return 0;
}

#pragma region HELPERS

template <class INPUT, class OUTPUT>
vector<OUTPUT> transform(vector<INPUT> input, function<OUTPUT(const INPUT&)> application)
{
	vector<OUTPUT> output;
	output.resize(input.size());
	transform(input.begin(), input.end(), output.begin(), application);

	return output;
}

string timeToString(long long time)
{
	ostringstream text;
	vector<string> units = {
		"ns",
		"μs",
		"ms",
		"s"};
	for (number i = 0; i < units.size(); i++)
	{
		if (time < 10000 || i == units.size() - 1)
		{
			text << time << " " << units[i];
			break;
		}
		else
		{
			time /= 1000;
		}
	}

	return text.str();
}

number salaryToNumber(string salary)
{
	auto salaryDouble = stod(salary) * 100;
	auto salaryNumber = (long long)salaryDouble + (LLONG_MAX / 4);
	return (number)salaryNumber;
}

double numberToSalary(number salary)
{
	return ((long long)salary - (LLONG_MAX / 4)) * 0.01;
}

string filename(string filename, int i)
{
	return string(FILES_DIR) + "/" + filename + (i > -1 ? ("-" + to_string(i)) : "") + ".bin";
}

string redishost(string host, int i)
{
	return host + (i > -1 ? ("/" + to_string(i)) : "");
}

void LOG(LOG_LEVEL level, boost::format message)
{
	LOG(level, boost::str(message));
}

void LOG(LOG_LEVEL level, string message)
{
	if (level >= __logLevel)
	{
		auto t = time(nullptr);
		cout << "[" << put_time(localtime(&t), "%d/%m/%Y %H:%M:%S") << "] " << setw(8) << logLevelColors[level] << logLevelStrings[level] << ": " << message << RESET << endl;
	}
}

#pragma endregion
