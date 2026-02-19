#include <Engine.h>

#include "Simulation.h"

int main()
{
    Simulation simulation{};
    Engine engine{};
    engine.run(simulation);
}
