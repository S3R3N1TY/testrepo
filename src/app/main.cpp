#include <Engine.h>
#include "Simulation.h"

int main()
{
    Simulation simulation{};
    Engine engine{};

    Engine::RunConfig cfg{};
    cfg.vertexShaderPath = "shaders/triangle.vert.spv";
    cfg.fragmentShaderPath = "shaders/triangle.frag.spv";

    engine.run(simulation, cfg);
}
