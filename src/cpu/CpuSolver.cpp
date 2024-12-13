#include "CpuSolver.h"
#include <assert.h>
#include <iostream>
#include <chrono>
#include <math.h>
#include <omp.h>
#include "../Timer.h"

void CpuSolver::solve(CpuGridData& grid)
{
	// Compute inital residual
	double initialResidual = compResidual(grid, 0);
	std::cout << "Inital residual: " << initialResidual << '\n';

	for (std::size_t i = 0; i < grid.maxiter; i++) {
		Timer::start();
		double res = vcycle(grid);

		std::cout << "iter: " << i << " residual: " << res << ' ';
		Timer::stop();
	}
}

double CpuSolver::compResidual(CpuGridData& grid, std::size_t levelNum)
{
	CpuGridData::LevelData& level = grid.getLevel(levelNum);

	/*
	* Compute the residual on multiple threads using openMP
	* Save the computed residual in the following array using the thread id as index
	* The used indicies are space out by CACHE_LINE_SIZE bytes to prevent false sharing
	*/
#define CACHE_LINE_SIZE 64
	constexpr int THREAD_OFFSET = CACHE_LINE_SIZE / sizeof(double);
	std::vector<double> shards(16 * THREAD_OFFSET);

#pragma omp parallel for schedule(static,8)
	for (std::int64_t x = 1; x < level.levelDim[0]+1; x++) {
		const auto omp_rank = omp_get_thread_num() * THREAD_OFFSET;
		for (std::size_t y = 1; y < level.levelDim[1]+1; y++) {
			for (std::size_t z = 1; z < level.levelDim[2]+1; z++) {
				
				double stencilsum = 0.0;
				for (std::size_t i = 0; i < grid.stencil.values.size(); i++) {
					double vVal = level.v.get(x + grid.stencil.getXOffset(i), y + grid.stencil.getYOffset(i), z + grid.stencil.getZOffset(i));
					stencilsum += grid.stencil.values[i] * vVal;
				}

				stencilsum /= level.h * level.h;

				if (!grid.isLinear) {

					// See tutorial_multigrid.pdf, page 102, Formula 6.13
					double ex = exp(level.v.get(x, y, z));
					double nonLinear = grid.gamma * level.v.get(x, y, z) * ex;
					stencilsum += nonLinear;
				}

				double r = level.f.get(x, y, z) - stencilsum;
				level.r.set(x, y, z, r);

				shards[omp_rank] += r * r;
			}
		}
	}

	// Accumulate the computed residual parts
	double res = 0.0;
	for (std::size_t i = 0; i < shards.size(); i += THREAD_OFFSET) {
		res += shards[i];
	}
	
	return sqrt(res);
}

double CpuSolver::vcycle(CpuGridData& grid)
{
	for (std::size_t i = 0; i < grid.numLevels()-1; i++) {
		jacobi(grid, i, grid.preSmoothing);

		CpuGridData::LevelData& nextLevel = grid.getLevel(i + 1);

		// compute residual
		compResidual(grid, i);
		Vector3& r = grid.getLevel(i).r;

		// restrict residual to next level f
		// f^2h = r^2h
		restrict(r, nextLevel.f);

		if (grid.isLinear) {
			nextLevel.v.fill(0.0);
		}else {
			// See tutorial_multigrid.pdf, page 98, Full Approximation Scheme (FAS)

			// restrict v^h to next level v^2h
			restrict(grid.getLevel(i).v, nextLevel.restV);
			restrict(grid.getLevel(i).v, nextLevel.v);

			// Compute A^2h (v^2h) and store it in r
			applyStencil(grid, i + 1, nextLevel.restV);
			// Add A^2h (v^2h) to r^2h
			nextLevel.f += nextLevel.r;
		}
	}
	
	// reached coarsed level, solve now
	jacobi(grid, grid.numLevels() - 1, grid.preSmoothing+grid.postSmoothing);

	for (std::size_t i = grid.numLevels() - 1; i > 0; i--) {
		
		if (!grid.isLinear) {
			CpuGridData::LevelData& level = grid.getLevel(i);
			// compute u^2h = u^2h - v^2h
			level.v -= level.restV;
		}

		// interpolate v^2h to previos level e^h
		interpolate(grid, i - 1);

		// v = v + e
		auto& levelPrev = grid.getLevel(i - 1);
		levelPrev.v += levelPrev.e;

		jacobi(grid, i - 1, grid.postSmoothing);
	}

	// returns current residual
	return compResidual(grid, 0);
}

void CpuSolver::jacobi(CpuGridData& grid, std::size_t levelNum, std::size_t maxiter)
{	
	CpuGridData::LevelData& level = grid.getLevel(levelNum);
	const double preFac = grid.stencil.values[0] / (level.h * level.h);
	const double alpha = (level.h * level.h) / grid.stencil.values[0]; // stencil center

	for (std::size_t i = 0; i < maxiter; i++) {
		
		compResidual(grid, levelNum);
		
#pragma omp parallel for schedule(static,8)
		for (std::int64_t x = 1; x < level.levelDim[0] + 1; x++) {
			for (std::size_t y = 1; y < level.levelDim[1] + 1; y++) {
				for (std::size_t z = 1; z < level.levelDim[2] + 1; z++) {

					double newV;
					if (grid.isLinear) {
						newV = level.v.get(x, y, z) + grid.omega * (alpha * level.r.get(x, y, z));
					}else {
						// See tutorial_multigrid.pdf, page 103, Formula 6.14
						double ex = exp(level.v.get(x, y, z));
						double denuminator = preFac + grid.gamma * (1 + level.v.get(x, y, z)) * ex;

						newV = level.v.get(x, y, z) + grid.omega * (level.r.get(x, y, z) / denuminator);
					}

					level.v.set(x, y, z, newV);
				}
			}
		}
	}
}

// Only needed for non-linear code
void CpuSolver::applyStencil(CpuGridData& grid, std::size_t levelNum, const Vector3& v)
{
	CpuGridData::LevelData& level = grid.getLevel(levelNum);
	assert(level.v.flatSize() == v.flatSize());
	Vector3& result = level.r;

#pragma omp parallel for schedule(static,8)
	for (std::int64_t x = 1; x < level.levelDim[0] + 1; x++) {
		for (std::size_t y = 1; y < level.levelDim[1] + 1; y++) {
			for (std::size_t z = 1; z < level.levelDim[2] + 1; z++) {

				double stencilsum = 0.0;
				for (std::size_t i = 0; i < grid.stencil.values.size(); i++) {
					double vVal = v.get(x + grid.stencil.getXOffset(i), y + grid.stencil.getYOffset(i), z + grid.stencil.getZOffset(i));
					stencilsum += grid.stencil.values[i] * vVal;
				}
				stencilsum /= level.h * level.h;
				// See tutorial_multigrid.pdf, page 102, Formula 6.13
				double nonLinear = grid.gamma * v.get(x, y, z) * exp(v.get(x, y, z));
				stencilsum += nonLinear;

				result.set(x, y, z, stencilsum);
			}
		}
	}
}

void CpuSolver::restrict(const Vector3& fine, Vector3& coarse)
{

#pragma omp parallel for schedule(static,8)
	for (std::int64_t x = 1; x < coarse.getXdim()-1; x++) {
		for (std::size_t y = 1; y < coarse.getYdim()-1; y++) {
			for (std::size_t z = 1; z < coarse.getZdim()-1; z++) {

				std::size_t xCenter = 2 * x;
				std::size_t yCenter = 2 * y;
				std::size_t zCenter = 2 * z;

				double coarseValue = 0.0;

				for (int ii = -2 + 1; ii < 2; ii++) {
					for (int jj = -2 + 1; jj < 2; jj++) {
						for (int kk = -2 + 1; kk < 2; kk++) {
							double fac = 0.125 * ((2.0 - abs(ii)) / 2.0) * ((2.0 - abs(jj)) / 2.0) * ((2.0 - abs(kk)) / 2.0);
							coarseValue += fac * fine.get(xCenter + ii, yCenter + jj, zCenter + kk);
						}
					}
				}

				coarse.set(x, y, z, coarseValue);
			}
		}
	}
}

void CpuSolver::interpolate(CpuGridData& grid, std::size_t level)
{
	const Vector3& coarse = grid.getLevel(level + 1).v;
	Vector3& fine = grid.getLevel(level).e;

	// prepare
	for (std::size_t x = 0; x < fine.getXdim() - 1; x += 2) {
		for (std::size_t y = 0; y < fine.getYdim() - 1; y += 2) {
			for (std::size_t z = 0; z < fine.getZdim() - 1; z += 2) {
				double val = coarse.get(x/2, y/2, z/2);
				fine.set(x, y, z, val);
			}
		}
	}

	// Interpolate in x-direction
	for (std::size_t x = 0; x+2 < fine.getXdim(); x += 2) {
		for (std::size_t y = 0; y < fine.getYdim(); y += 2) {
			for (std::size_t z = 0; z < fine.getZdim(); z += 2) {
				double val = 0.5 * fine.get(x, y, z) + 0.5 * fine.get(x + 2, y, z);
				fine.set(x+1, y, z, val);
			}
		}
	}

	// Interpolate in y-direction
	for (std::size_t x = 0; x < fine.getXdim(); x++) {
		for (std::size_t y = 0; y + 2 < fine.getYdim(); y += 2) {
			for (std::size_t z = 0; z < fine.getZdim(); z += 2) {
				double val = 0.5 * fine.get(x, y, z) + 0.5 * fine.get(x, y+2, z);
				fine.set(x, y+1, z, val);
			}
		}
	}

	// Interpolate in z-direction
	for (std::size_t x = 0; x < fine.getXdim(); x++) {
		for (std::size_t y = 0; y < fine.getYdim(); y++) {
			for (std::size_t z = 0; z + 2 < fine.getZdim(); z += 2) {
				double val = 0.5 * fine.get(x, y, z) + 0.5 * fine.get(x, y, z + 2);
				fine.set(x, y, z+1, val);
			}
		}
	}
}

