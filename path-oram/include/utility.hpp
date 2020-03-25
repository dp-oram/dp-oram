#include "definitions.h"

namespace PathORAM
{
	using namespace std;

	bytes getRandomBlock(ulong blockSize);
	ulong getRandomULong(ulong max);
	void seedRandom(int seed);
	bytes encrypt(bytes key, bytes iv, bytes input, EncryptionMode mode);

	bytes fromText(string text, ulong BLOCK_SIZE);
	string toText(bytes data, ulong BLOCK_SIZE);
}
