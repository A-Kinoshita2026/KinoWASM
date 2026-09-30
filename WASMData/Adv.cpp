#include "System/Fiber.hpp"
#include "System/ScriptSystem.hpp"
#include "System/Adventure.hpp"

Fiber<void> ADVStart()
{
	LoadTextTable(u"data/Test.tsv");
	TextSet(0);
	TextSet(1);
	TextSet(2);
}

