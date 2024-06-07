#include "Vector3.h"
#include <assert.h>

Vector3::Vector3(std::size_t x, std::size_t y, std::size_t z)
{
	dims[0] = x;
	dims[1] = y;
	dims[2] = z;
	values.resize(x * y * z);
}

void Vector3::set(std::size_t x, std::size_t y, std::size_t z, double val)
{
	const std::size_t idx = (z * dims[0] * dims[1]) + (y * dims[0]) + x;
	assert(idx < values.size());
	values[idx] = val;
}

double Vector3::get(std::size_t x, std::size_t y, std::size_t z) const
{
	const std::size_t idx = (z * dims[0] * dims[1]) + (y * dims[0]) + x;
	assert(idx < values.size());
	return values[idx];
}
