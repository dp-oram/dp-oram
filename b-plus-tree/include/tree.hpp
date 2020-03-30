#pragma once

#include "definitions.h"
#include "storage-adapter.hpp"

namespace BPlusTree
{
	using namespace std;

	class Tree
	{
		public:
		number search(number key);

		Tree(AbsStorageAdapter *storage);
		Tree(AbsStorageAdapter *storage, vector<pair<number, bytes>> data);

		private:
		AbsStorageAdapter *storage;
		number root;
		number b;
		number leftmostDataBlock; // for testing

		number createDataBlock(bytes data, number next);
		pair<bytes, number> readDataBlock(number address);

		number createNodeBlock(vector<pair<number, number>> data);
		vector<pair<number, number>> readNodeBlock(number address);

		vector<pair<number, number>> pushLayer(vector<pair<number, number>> input);

		friend class TreeTest_ReadDataLayer_Test;
		friend class TreeTest_CreateNodeBlockTooBig_Test;
		friend class TreeTest_CreateNodeBlock_Test;
		friend class TreeTest_ReadNodeBlock_Test;
		friend class TreeTest_PushLayer_Test;
	};
}
