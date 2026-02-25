#include <vulkan/RenderGraph.h>

#include <cassert>
#include <unordered_map>

int main()
{
    RenderTaskGraph graph{};

    const auto passA = graph.addPass(RenderTaskGraph::PassNode{});
    const auto passB = graph.addPass(RenderTaskGraph::PassNode{});
    const auto passC = graph.addPass(RenderTaskGraph::PassNode{});
    const auto passD = graph.addPass(RenderTaskGraph::PassNode{});

    graph.addDependency(passA, passB);
    graph.addDependency(passA, passC);
    graph.addDependency(passB, passD);
    graph.addDependency(passC, passD);

    const auto compiled = graph.compile();
    assert(compiled.hasValue());

    std::unordered_map<RenderTaskGraph::PassId, RenderTaskGraph::CompiledPass> byId{};
    for (const auto& pass : compiled.value()) {
        byId.insert_or_assign(pass.id, pass);
    }

    assert(byId.contains(passA));
    assert(byId.contains(passB));
    assert(byId.contains(passC));
    assert(byId.contains(passD));

    assert(byId.at(passA).scheduleLevel == 0);
    assert(byId.at(passB).scheduleLevel == 1);
    assert(byId.at(passC).scheduleLevel == 1);
    assert(byId.at(passD).scheduleLevel == 2);

    assert(byId.at(passA).scheduleOrder < byId.at(passB).scheduleOrder);
    assert(byId.at(passA).scheduleOrder < byId.at(passC).scheduleOrder);
    assert(byId.at(passB).scheduleOrder < byId.at(passD).scheduleOrder);
    assert(byId.at(passC).scheduleOrder < byId.at(passD).scheduleOrder);

    return 0;
}
