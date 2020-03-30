#include "storage-adapter.hpp"

#include <boost/format.hpp>

namespace BPlusTree
{
	using namespace std;
	using boost::format;

	AbsStorageAdapter::~AbsStorageAdapter()
	{
	}

	AbsStorageAdapter::AbsStorageAdapter(number blockSize) :
		blockSize(blockSize)
	{
	}

	number AbsStorageAdapter::getBlockSize()
	{
		return blockSize;
	}

	InMemoryStorageAdapter::InMemoryStorageAdapter(number blockSize) :
		AbsStorageAdapter(blockSize)
	{
	}

	InMemoryStorageAdapter::~InMemoryStorageAdapter()
	{
	}

	bytes InMemoryStorageAdapter::get(number location)
	{
		checkLocation(location);

		return memory[location];
	}

	void InMemoryStorageAdapter::set(number location, bytes data)
	{
		if (data.size() != blockSize)
		{
			throw Exception(boost::format("data size (%1%) does not match block size (%2%)") % data.size() % blockSize);
		}

		checkLocation(location);

		memory[location] = data;
	}

	number InMemoryStorageAdapter::malloc()
	{
		return locationCounter++;
	}

	number InMemoryStorageAdapter::start()
	{
		return ROOT;
	}

	number InMemoryStorageAdapter::empty()
	{
		return EMPTY;
	}

	void InMemoryStorageAdapter::checkLocation(number location)
	{
		if (location >= locationCounter)
		{
			throw Exception(boost::format("attempt to access memory that was not malloced (%1%)") % location);
		}
	}
}