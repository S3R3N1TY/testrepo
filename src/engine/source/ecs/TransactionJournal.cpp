#include <ecs/TransactionJournal.h>

#include <ecs/World.h>

#include <queue>
#include <stdexcept>
#include <unordered_map>

Transaction::Transaction(std::vector<JournalEntry> entries)
    : entries_(std::move(entries))
{
}

std::vector<size_t> Transaction::topoOrder() const
{
    std::unordered_map<uint64_t, size_t> indexById{};
    indexById.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!indexById.emplace(entries_[i].id, i).second) {
            throw std::runtime_error("Transaction duplicate entry id");
        }
    }

    std::vector<std::vector<size_t>> children(entries_.size());
    std::vector<size_t> indegree(entries_.size(), 0);
    for (size_t i = 0; i < entries_.size(); ++i) {
        for (uint64_t depId : entries_[i].dependsOn) {
            const auto depIt = indexById.find(depId);
            if (depIt == indexById.end()) {
                throw std::runtime_error("Transaction dependency references unknown entry id");
            }
            children[depIt->second].push_back(i);
            indegree[i] += 1;
        }
    }

    std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready{};
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) {
            ready.push(i);
        }
    }

    std::vector<size_t> order{};
    order.reserve(entries_.size());
    while (!ready.empty()) {
        const size_t index = ready.top();
        ready.pop();
        order.push_back(index);
        for (size_t child : children[index]) {
            indegree[child] -= 1;
            if (indegree[child] == 0) {
                ready.push(child);
            }
        }
    }

    if (order.size() != entries_.size()) {
        throw std::runtime_error("Transaction dependency graph contains a cycle");
    }

    return order;
}

bool Transaction::validateGraphAcyclic() const
{
    try {
        (void)topoOrder();
        return true;
    }
    catch (...) {
        return false;
    }
}

void Transaction::execute(World& world, const FailureInjectionConfig* failure) const
{
    const std::vector<size_t> order = topoOrder();
    if (failure && failure->failAtPhase == TransactionPhase::Prepare) {
        throw std::runtime_error("Injected prepare failure");
    }

    for (size_t idx : order) {
        const JournalEntry& entry = entries_[idx];
        if (entry.validate && !entry.validate(world)) {
            throw std::runtime_error("Transaction entry validation failed");
        }
        if (failure && failure->failAtEntryId == entry.id) {
            throw std::runtime_error("Injected validate failure");
        }
    }

    if (failure && failure->failAtPhase == TransactionPhase::Commit) {
        throw std::runtime_error("Injected commit failure");
    }

    std::vector<size_t> applied{};
    applied.reserve(order.size());
    size_t applyCount = 0;
    try {
        for (size_t idx : order) {
            const JournalEntry& entry = entries_[idx];
            if (entry.apply) {
                entry.apply(world);
            }
            applied.push_back(idx);
            applyCount += 1;
            if (failure && failure->failAfterNApply && applyCount >= *failure->failAfterNApply) {
                throw std::runtime_error("Injected apply failure");
            }
        }
    }
    catch (...) {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            const JournalEntry& entry = entries_[*it];
            if (entry.undo) {
                entry.undo(world);
            }
        }
        throw;
    }
}
