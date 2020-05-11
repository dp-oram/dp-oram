#pragma once

#include "definitions.h"

#include <string>

namespace DPORAM
{
	using namespace std;

	tuple<number, number, number, number> padToBuckets(pair<number, number> query, number min, number max, number buckets);

	number optimalMu(double beta, number k, number N, number epsilon);

	vector<pair<number, number>> BRC(number fanout, number from, number to);

	double sampleLaplace(double mu, double lambda);
}