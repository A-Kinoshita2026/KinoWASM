
#include "Fiber.hpp"
#include "ScriptSystem.hpp"

void Starter();

extern "C" API int Main(void)
{
	Starter();
	return 0;
}
