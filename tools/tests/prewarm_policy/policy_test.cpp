#include <cassert>
#include <cstdio>
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelinePrewarmPolicy.h"
using namespace D3D12PipelinePrewarmPolicy;

int main()
{
	// used recipes: kept for 29 days however many launches happened, pruned from day 30
	assert(!ShouldPruneRecipe(false, false, 37, 0));
	assert(!ShouldPruneRecipe(false, false, 500, 29));
	assert(ShouldPruneRecipe(false, false, 0, 30));
	// must-be-ready (rain pass, full-screen passes) never go
	assert(!ShouldPruneRecipe(false, true, 1000, 1000));
	assert(!ShouldPruneRecipe(true, true, 1000, 1000));
	// never used by a draw: gone after 2 sessions, days do not matter
	assert(!ShouldPruneRecipe(true, false, 1, 400));
	assert(ShouldPruneRecipe(true, false, 2, 0));

	// tiers: must-be-ready is always core
	assert(ClassifyRecipe(true, 1, 41) == Tier::Core);
	// BOTW at session 41: core from 20 sessions (half), frequent from 3
	assert(ClassifyRecipe(false, 20, 41) == Tier::Core);
	assert(ClassifyRecipe(false, 19, 41) == Tier::Frequent);
	assert(ClassifyRecipe(false, 3, 41) == Tier::Frequent);
	assert(ClassifyRecipe(false, 2, 41) == Tier::Occasional);
	// young game (MK8 at session 7): core starts at 3 sessions, never below the frequent threshold
	assert(ClassifyRecipe(false, 3, 7) == Tier::Core);
	assert(ClassifyRecipe(false, 2, 7) == Tier::Occasional);
	assert(ClassifyRecipe(false, 1, 1) == Tier::Occasional);
	assert(ClassifyRecipe(false, 0, 0) == Tier::Occasional);
	std::puts("prune and tier policy ok");
}
