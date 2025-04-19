// main.cpp
#include "UI.hpp"

int main(int, char**)
{
	int maxThreads = 16;
	UI ui(new TestSettings(maxThreads));

	if (!ui.Init())
	{
		return -1;
	}
	ui.Update();
	ui.Cleanup();
	return 0;
}
