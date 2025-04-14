// main.cpp
#include "UI.hpp"

int main(int, char**)
{
	UI ui(new TestSettings(16));

	if (!ui.Init())
	{
		return -1;
	}
	ui.Update();
	ui.Cleanup();
	return 0;
}
